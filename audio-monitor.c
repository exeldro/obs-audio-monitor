#include "audio-monitor.h"

#include <initguid.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <obs-module.h>

#include "media-io/audio-resampler.h"
#include "util/platform.h"

#define safe_release(ptr)                          \
	do {                                       \
		if (ptr) {                         \
			ptr->lpVtbl->Release(ptr); \
		}                                  \
	} while (false)

#ifndef KSAUDIO_SPEAKER_4POINT1
#define KSAUDIO_SPEAKER_4POINT1 \
	(KSAUDIO_SPEAKER_SURROUND | SPEAKER_LOW_FREQUENCY)
#endif

#define ACTUALLY_DEFINE_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
	EXTERN_C const GUID DECLSPEC_SELECTANY name = {                       \
		l, w1, w2, {b1, b2, b3, b4, b5, b6, b7, b8}}

ACTUALLY_DEFINE_GUID(CLSID_MMDeviceEnumerator, 0xBCDE0395, 0xE52F, 0x467C, 0x8E,
		     0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
ACTUALLY_DEFINE_GUID(IID_IMMDeviceEnumerator, 0xA95664D2, 0x9614, 0x4F35, 0xA7,
		     0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);
ACTUALLY_DEFINE_GUID(IID_IAudioClient, 0x1CB9AD4C, 0xDBFA, 0x4C32, 0xB1, 0x78,
		     0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2);
ACTUALLY_DEFINE_GUID(IID_IAudioRenderClient, 0xF294ACFC, 0x3146, 0x4483, 0xA7,
		     0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2);

struct audio_monitor_info {

	obs_source_t *source;
	char *device_id;

	IMMDevice *device;
	IAudioClient *client;
	IAudioRenderClient *render;
	uint32_t sample_rate;
	WORD channels;
	audio_resampler_t *resampler;
	float volume;
};

static const char *audio_monitor_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("AudioMonitor");
}

static enum speaker_layout convert_speaker_layout(DWORD layout, WORD channels)
{
	switch (layout) {
	case KSAUDIO_SPEAKER_2POINT1:
		return SPEAKERS_2POINT1;
	case KSAUDIO_SPEAKER_SURROUND:
		return SPEAKERS_4POINT0;
	case KSAUDIO_SPEAKER_4POINT1:
		return SPEAKERS_4POINT1;
	case KSAUDIO_SPEAKER_5POINT1:
		return SPEAKERS_5POINT1;
	case KSAUDIO_SPEAKER_7POINT1:
		return SPEAKERS_7POINT1;
	}

	return (enum speaker_layout)channels;
}

static void audio_monitor_update(void *data, obs_data_t *settings)
{
	struct audio_monitor_info *audio_monitor = data;
	audio_monitor->volume = (float)obs_data_get_double(settings, "volume") / 100.0f;
	const char *device_id = obs_data_get_string(settings, "device");
	if (audio_monitor->device_id && strcmp(device_id, audio_monitor->device_id) == 0)
		return;
	bfree(audio_monitor->device_id);
	audio_monitor->device_id = bstrdup(device_id);

	if (audio_monitor->client)
		audio_monitor->client->lpVtbl->Stop(
			audio_monitor->client);

	safe_release(audio_monitor->device);
	safe_release(audio_monitor->client);
	safe_release(audio_monitor->render);
	audio_resampler_destroy(audio_monitor->resampler);

	IMMDeviceEnumerator *immde = NULL;
	HRESULT hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL,
	                              CLSCTX_ALL,
	                              &IID_IMMDeviceEnumerator,
	                              (void **)&immde);
	if (FAILED(hr)) {
		return;
	}
	wchar_t w_id[512];
	os_utf8_to_wcs(device_id, 0, w_id, 512);

	hr = immde->lpVtbl->GetDevice(immde, w_id,
	                              &audio_monitor->device);
	if (FAILED(hr)) {
		safe_release(immde);
		return;
	}
	hr = audio_monitor->device->lpVtbl->Activate(
		audio_monitor->device, &IID_IAudioClient, CLSCTX_ALL,
		NULL, (void **)&audio_monitor->client);
	if (FAILED(hr)) {
		safe_release(immde);
		return;
	}
	WAVEFORMATEX *wfex = NULL;
	hr = audio_monitor->client->lpVtbl->GetMixFormat(
		audio_monitor->client, &wfex);
	if (FAILED(hr)) {
		safe_release(immde);
		return;
	}
	hr = audio_monitor->client->lpVtbl->Initialize(
		audio_monitor->client, AUDCLNT_SHAREMODE_SHARED, 0,
		10000000, 0, wfex, NULL);
	if (FAILED(hr)) {
		safe_release(immde);
		CoTaskMemFree(wfex);
		return;
	}
	const struct audio_output_info *info =
		audio_output_get_info(obs_get_audio());
	WAVEFORMATEXTENSIBLE *ext = (WAVEFORMATEXTENSIBLE *)wfex;
	struct resample_info from;
	struct resample_info to;

	from.samples_per_sec = info->samples_per_sec;
	from.speakers = info->speakers;
	from.format = AUDIO_FORMAT_FLOAT_PLANAR;

	to.samples_per_sec = (uint32_t)wfex->nSamplesPerSec;
	to.speakers = convert_speaker_layout(ext->dwChannelMask,
	                                     wfex->nChannels);
	to.format = AUDIO_FORMAT_FLOAT;
	audio_monitor->sample_rate = (uint32_t)wfex->nSamplesPerSec;
	audio_monitor->channels = wfex->nChannels;

	CoTaskMemFree(wfex);

	audio_monitor->resampler = audio_resampler_create(&to, &from);

	UINT32 frames;
	hr = audio_monitor->client->lpVtbl->GetBufferSize(
		audio_monitor->client, &frames);
	if (FAILED(hr)) {
		safe_release(immde);
		return;
	}
	hr = audio_monitor->client->lpVtbl->GetService(
		audio_monitor->client, &IID_IAudioRenderClient,
		(void **)&audio_monitor->render);
	if (FAILED(hr)) {
		safe_release(immde);
		return;
	}
	hr = audio_monitor->client->lpVtbl->Start(
		audio_monitor->client);

	safe_release(immde);
}

static void *audio_monitor_create(obs_data_t *settings, obs_source_t *source)
{
	struct audio_monitor_info *audio_monitor =
		bzalloc(sizeof(struct audio_monitor_info));
	audio_monitor->source = source;
	audio_monitor_update(audio_monitor, settings);
	return audio_monitor;
}

static void audio_monitor_destroy(void *data)
{
	struct audio_monitor_info *audio_monitor = data;
	bfree(audio_monitor);
}

static bool add_monitoring_device(void *data, const char *name, const char *id)
{
	obs_property_list_add_string(data, name, id);
	return true;
}

static obs_properties_t *audio_monitor_properties(void *data)
{
	struct audio_move_info *audio_monitor = data;
	obs_properties_t *ppts = obs_properties_create();
	obs_property_t *p = obs_properties_add_list(ppts, "device",
						    obs_module_text("Device"),
						    OBS_COMBO_TYPE_LIST,
						    OBS_COMBO_FORMAT_STRING);
	obs_enum_audio_monitoring_devices(add_monitoring_device, p);
	obs_properties_add_float_slider(
		ppts, "volume", obs_module_text("Volume"), 0.0, 100.0, 1.0);
	return ppts;
}

void audio_monitor_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, "volume", 100.0);
}

struct obs_audio_data *audio_monitor_audio(void *data,
					   struct obs_audio_data *audio)
{
	struct audio_monitor_info *audio_monitor = data;
	if (!audio_monitor->resampler)
		return audio;

	uint8_t *resample_data[MAX_AV_PLANES];
	uint32_t resample_frames;
	uint64_t ts_offset;
	bool success = audio_resampler_resample(
		audio_monitor->resampler, resample_data, &resample_frames,
		&ts_offset, (const uint8_t *const *)audio->data,
		(uint32_t)audio->frames);
	if (!success)
		return audio;

	UINT32 pad = 0;
	audio_monitor->client->lpVtbl->GetCurrentPadding(audio_monitor->client,
							 &pad);
	BYTE *output;
	HRESULT hr = audio_monitor->render->lpVtbl->GetBuffer(
		audio_monitor->render, resample_frames, &output);
	if (FAILED(hr))
		return audio;
	/* apply volume */

	if (!close_float(audio_monitor->volume, 1.0f, EPSILON)) {
		register float *cur = (float *)resample_data[0];
		register float *end =
			cur + resample_frames * audio_monitor->channels;

		while (cur < end)
			*(cur++) *= audio_monitor->volume;
	}
	memcpy(output, resample_data[0],
	       resample_frames * audio_monitor->channels * sizeof(float));
	audio_monitor->render->lpVtbl->ReleaseBuffer(audio_monitor->render,
						     resample_frames, 0);
	return audio;
}

struct obs_source_info audio_monitor = {
	.id = "audio_monitor",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_AUDIO,
	.get_name = audio_monitor_get_name,
	.create = audio_monitor_create,
	.destroy = audio_monitor_destroy,
	.update = audio_monitor_update,
	.load = audio_monitor_update,
	.get_defaults = audio_monitor_defaults,
	.get_properties = audio_monitor_properties,
	.filter_audio = audio_monitor_audio,
};

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("Exeldro");
OBS_MODULE_USE_DEFAULT_LOCALE("audio-monitor", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("Description");
}

bool obs_module_load(void)
{
	obs_register_source(&audio_monitor);
	return true;
}
