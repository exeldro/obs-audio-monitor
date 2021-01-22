#include "audio-monitor.h"

#ifdef WIN32
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#endif
#ifdef __APPLE__
#include <AudioUnit/AudioUnit.h>
#include <AudioToolbox/AudioQueue.h>
#include <CoreFoundation/CFString.h>
#include <CoreAudio/CoreAudio.h>
#include <util/circlebuf.h>
#endif
#include <obs-module.h>

#include "media-io/audio-resampler.h"
#include "util/platform.h"
#include "util/threading.h"

#ifndef KSAUDIO_SPEAKER_4POINT1
#define KSAUDIO_SPEAKER_4POINT1 \
	(KSAUDIO_SPEAKER_SURROUND | SPEAKER_LOW_FREQUENCY)
#endif

#ifdef __APPLE__
static bool success_(OSStatus stat, const char *func, const char *call)
{
	if (stat != noErr) {
		blog(LOG_WARNING, "%s: %s failed: %d", func, call, (int)stat);
		return false;
	}

	return true;
}

#define success(stat, call) success_(stat, __FUNCTION__, call)
#endif

#ifdef WIN32
#define safe_release(ptr)                          \
	do {                                       \
		if (ptr) {                         \
			ptr->lpVtbl->Release(ptr); \
		}                                  \
	} while (false)

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

#endif

struct audio_monitor_info {

	obs_source_t *source;
	char *device_id;

#ifdef WIN32
	IMMDevice *device;
	IAudioClient *client;
	IAudioRenderClient *render;
	uint32_t sample_rate;
#endif
#ifdef __APPLE__
	AudioQueueRef queue;
	AudioQueueBufferRef buffers[3];
	size_t buffer_size;
	size_t wait_size;
	struct circlebuf empty_buffers;
	struct circlebuf new_data;
	volatile bool active;
	bool paused;
#endif
	uint32_t channels;
	audio_resampler_t *resampler;
	float volume;
	pthread_mutex_t mutex;
};

static const char *audio_monitor_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("AudioMonitor");
}

#ifdef WIN32
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
#endif

#ifdef __APPLE__
static inline bool fill_buffer(struct audio_monitor_info *monitor)
{
	AudioQueueBufferRef buf;
	OSStatus stat;

	if (monitor->new_data.size < monitor->buffer_size) {
		return false;
	}

	circlebuf_pop_front(&monitor->empty_buffers, &buf, sizeof(buf));
	circlebuf_pop_front(&monitor->new_data, buf->mAudioData,
			    monitor->buffer_size);

	buf->mAudioDataByteSize = monitor->buffer_size;

	stat = AudioQueueEnqueueBuffer(monitor->queue, buf, 0, NULL);
	if (!success(stat, "AudioQueueEnqueueBuffer")) {
		blog(LOG_WARNING, "%s: %s", __FUNCTION__,
		     "Failed to enqueue buffer");
		AudioQueueStop(monitor->queue, false);
	}
	return true;
}

static void buffer_audio(void *data, AudioQueueRef aq, AudioQueueBufferRef buf)
{
	struct audio_monitor_info *monitor = data;

	pthread_mutex_lock(&monitor->mutex);
	circlebuf_push_back(&monitor->empty_buffers, &buf, sizeof(buf));
	while (monitor->empty_buffers.size > 0) {
		if (!fill_buffer(monitor)) {
			break;
		}
	}
	if (monitor->empty_buffers.size == sizeof(buf) * 3) {
		monitor->paused = true;
		monitor->wait_size = monitor->buffer_size * 3;
		AudioQueuePause(monitor->queue);
	}
	pthread_mutex_unlock(&monitor->mutex);

	UNUSED_PARAMETER(aq);
}
#endif

void audio_monitor_stop(struct audio_monitor_info *audio_monitor)
{
#ifdef __APPLE__
	if (audio_monitor->active) {
		AudioQueueStop(audio_monitor->queue, true);
	}
	for (size_t i = 0; i < 3; i++) {
		if (audio_monitor->buffers[i]) {
			AudioQueueFreeBuffer(audio_monitor->queue,
					     audio_monitor->buffers[i]);
		}
	}
	if (audio_monitor->queue) {
		AudioQueueDispose(audio_monitor->queue, true);
	}
	circlebuf_free(&audio_monitor->empty_buffers);
	circlebuf_free(&audio_monitor->new_data);
#endif
#ifdef WIN32
	if (audio_monitor->client)
		audio_monitor->client->lpVtbl->Stop(audio_monitor->client);

	safe_release(audio_monitor->device);
	safe_release(audio_monitor->client);
	safe_release(audio_monitor->render);
#endif
	audio_resampler_destroy(audio_monitor->resampler);
	audio_monitor->resampler = NULL;
}

void audio_monitor_start(struct audio_monitor_info *audio_monitor)
{

	const struct audio_output_info *info =
		audio_output_get_info(obs_get_audio());

#ifdef __APPLE__
	audio_monitor->channels = get_audio_channels(info->speakers);
	audio_monitor->buffer_size = audio_monitor->channels * sizeof(float) *
				     info->samples_per_sec / 100 * 3;
	audio_monitor->wait_size = audio_monitor->buffer_size * 3;
	AudioStreamBasicDescription desc = {
		.mSampleRate = (Float64)info->samples_per_sec,
		.mFormatID = kAudioFormatLinearPCM,
		.mFormatFlags = kAudioFormatFlagIsFloat |
				kAudioFormatFlagIsPacked,
		.mBytesPerPacket = sizeof(float) * audio_monitor->channels,
		.mFramesPerPacket = 1,
		.mBytesPerFrame = sizeof(float) * audio_monitor->channels,
		.mChannelsPerFrame = audio_monitor->channels,
		.mBitsPerChannel = sizeof(float) * 8};

	OSStatus stat = AudioQueueNewOutput(&desc, buffer_audio, audio_monitor,
					    NULL, NULL, 0,
					    &audio_monitor->queue);
	if (!success(stat, "AudioStreamBasicDescription")) {
		pthread_mutex_unlock(&audio_monitor->mutex);
		return;
	}
	if (strcmp(audio_monitor->device_id, "default") != 0) {
		CFStringRef cf_uid = CFStringCreateWithBytes(
			NULL, (const UInt8 *)audio_monitor->device_id,
			strlen(audio_monitor->device_id), kCFStringEncodingUTF8,
			false);

		stat = AudioQueueSetProperty(audio_monitor->queue,
					     kAudioQueueProperty_CurrentDevice,
					     &cf_uid, sizeof(cf_uid));
		CFRelease(cf_uid);
		if (!success(stat, "set current device")) {
			pthread_mutex_unlock(&audio_monitor->mutex);
			return;
		}
	}
	stat = AudioQueueSetParameter(audio_monitor->queue,
				      kAudioQueueParam_Volume, 1.0);
	if (!success(stat, "set volume")) {
		pthread_mutex_unlock(&audio_monitor->mutex);
		return;
	}

	for (size_t i = 0; i < 3; i++) {
		stat = AudioQueueAllocateBuffer(audio_monitor->queue,
						audio_monitor->buffer_size,
						&audio_monitor->buffers[i]);
		if (!success(stat, "allocation of buffer")) {
			pthread_mutex_unlock(&audio_monitor->mutex);
			return;
		}

		circlebuf_push_back(&audio_monitor->empty_buffers,
				    &audio_monitor->buffers[i],
				    sizeof(audio_monitor->buffers[i]));
	}
	struct resample_info from = {.samples_per_sec = info->samples_per_sec,
				     .speakers = info->speakers,
				     .format = AUDIO_FORMAT_FLOAT_PLANAR};
	struct resample_info to = {.samples_per_sec = info->samples_per_sec,
				   .speakers = info->speakers,
				   .format = AUDIO_FORMAT_FLOAT};
	audio_monitor->resampler = audio_resampler_create(&to, &from);
	if (!audio_monitor->resampler) {
		pthread_mutex_unlock(&audio_monitor->mutex);
		return;
	}

	stat = AudioQueueStart(audio_monitor->queue, NULL);
	if (!success(stat, "start")) {
		pthread_mutex_unlock(&audio_monitor->mutex);
		return;
	}
	audio_monitor->active = true;

#endif
#ifdef WIN32
	IMMDeviceEnumerator *immde = NULL;
	HRESULT hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL,
				      CLSCTX_ALL, &IID_IMMDeviceEnumerator,
				      (void **)&immde);
	if (FAILED(hr)) {
		pthread_mutex_unlock(&audio_monitor->mutex);
		return;
	}
	if (strcmp(audio_monitor->device_id, "default") == 0) {
		hr = immde->lpVtbl->GetDefaultAudioEndpoint(
			immde, eRender, eConsole, &audio_monitor->device);
	} else {
		wchar_t w_id[512];
		os_utf8_to_wcs(audio_monitor->device_id, 0, w_id, 512);

		hr = immde->lpVtbl->GetDevice(immde, w_id,
					      &audio_monitor->device);
	}
	if (FAILED(hr)) {
		safe_release(immde);
		pthread_mutex_unlock(&audio_monitor->mutex);
		return;
	}
	hr = audio_monitor->device->lpVtbl->Activate(
		audio_monitor->device, &IID_IAudioClient, CLSCTX_ALL, NULL,
		(void **)&audio_monitor->client);
	if (FAILED(hr)) {
		safe_release(immde);
		pthread_mutex_unlock(&audio_monitor->mutex);
		return;
	}
	WAVEFORMATEX *wfex = NULL;
	hr = audio_monitor->client->lpVtbl->GetMixFormat(audio_monitor->client,
							 &wfex);
	if (FAILED(hr)) {
		safe_release(immde);
		pthread_mutex_unlock(&audio_monitor->mutex);
		return;
	}
	hr = audio_monitor->client->lpVtbl->Initialize(audio_monitor->client,
						       AUDCLNT_SHAREMODE_SHARED,
						       0, 10000000, 0, wfex,
						       NULL);
	if (FAILED(hr)) {
		safe_release(immde);
		CoTaskMemFree(wfex);
		pthread_mutex_unlock(&audio_monitor->mutex);
		return;
	}
	WAVEFORMATEXTENSIBLE *ext = (WAVEFORMATEXTENSIBLE *)wfex;
	struct resample_info from;
	struct resample_info to;

	from.samples_per_sec = info->samples_per_sec;
	from.speakers = info->speakers;
	from.format = AUDIO_FORMAT_FLOAT_PLANAR;

	to.samples_per_sec = (uint32_t)wfex->nSamplesPerSec;
	to.speakers =
		convert_speaker_layout(ext->dwChannelMask, wfex->nChannels);
	to.format = AUDIO_FORMAT_FLOAT;
	audio_monitor->sample_rate = (uint32_t)wfex->nSamplesPerSec;
	audio_monitor->channels = wfex->nChannels;

	CoTaskMemFree(wfex);

	audio_monitor->resampler = audio_resampler_create(&to, &from);

	UINT32 frames;
	hr = audio_monitor->client->lpVtbl->GetBufferSize(audio_monitor->client,
							  &frames);
	if (FAILED(hr)) {
		safe_release(immde);
		pthread_mutex_unlock(&audio_monitor->mutex);
		return;
	}
	hr = audio_monitor->client->lpVtbl->GetService(
		audio_monitor->client, &IID_IAudioRenderClient,
		(void **)&audio_monitor->render);
	if (FAILED(hr)) {
		safe_release(immde);
		pthread_mutex_unlock(&audio_monitor->mutex);
		return;
	}
	hr = audio_monitor->client->lpVtbl->Start(audio_monitor->client);

	safe_release(immde);
#endif
}

static void audio_monitor_update(void *data, obs_data_t *settings)
{
	struct audio_monitor_info *audio_monitor = data;
	audio_monitor->volume =
		(float)obs_data_get_double(settings, "volume") / 100.0f;

	const char *device_id = obs_data_get_string(settings, "device");
	if (audio_monitor->device_id &&
	    strcmp(device_id, audio_monitor->device_id) == 0)
		return;
	bfree(audio_monitor->device_id);
	audio_monitor->device_id = bstrdup(device_id);

	if (!device_id || !*device_id)
		return;

	pthread_mutex_lock(&audio_monitor->mutex);
	audio_monitor_stop(audio_monitor);
	audio_monitor_start(audio_monitor);
	pthread_mutex_unlock(&audio_monitor->mutex);
}

static void *audio_monitor_create(obs_data_t *settings, obs_source_t *source)
{
	struct audio_monitor_info *audio_monitor =
		bzalloc(sizeof(struct audio_monitor_info));
	audio_monitor->source = source;
	pthread_mutex_init(&audio_monitor->mutex, NULL);
	audio_monitor_update(audio_monitor, settings);
	return audio_monitor;
}

static void audio_monitor_destroy(void *data)
{
	struct audio_monitor_info *audio_monitor = data;

#ifdef __APPLE__
	if (audio_monitor->active) {
		AudioQueueStop(audio_monitor->queue, true);
	}
	for (size_t i = 0; i < 3; i++) {
		if (audio_monitor->buffers[i]) {
			AudioQueueFreeBuffer(audio_monitor->queue,
					     audio_monitor->buffers[i]);
		}
	}
	if (audio_monitor->queue) {
		AudioQueueDispose(audio_monitor->queue, true);
	}
#endif
#ifdef WIN32
	if (audio_monitor->client)
		audio_monitor->client->lpVtbl->Stop(audio_monitor->client);

	safe_release(audio_monitor->device);
	safe_release(audio_monitor->client);
	safe_release(audio_monitor->render);
#endif

	audio_resampler_destroy(audio_monitor->resampler);
	pthread_mutex_destroy(&audio_monitor->mutex);
	bfree(audio_monitor);
}

static bool add_monitoring_device(void *data, const char *name, const char *id)
{
	obs_property_list_add_string(data, name, id);
	return true;
}

static obs_properties_t *audio_monitor_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *ppts = obs_properties_create();
	obs_property_t *p = obs_properties_add_list(ppts, "device",
						    obs_module_text("Device"),
						    OBS_COMBO_TYPE_LIST,
						    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, obs_module_text("Default"), "default");
	obs_enum_audio_monitoring_devices(add_monitoring_device, p);
	obs_properties_add_float_slider(
		ppts, "volume", obs_module_text("Volume"), 0.0, 100.0, 1.0);
	return ppts;
}

void audio_monitor_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, "volume", 100.0);
	obs_data_set_default_string(settings, "device", "default");
}

struct obs_audio_data *audio_monitor_audio(void *data,
					   struct obs_audio_data *audio)
{
	struct audio_monitor_info *audio_monitor = data;
	if (!audio_monitor->resampler && audio_monitor->device_id &&
	    strlen(audio_monitor->device_id) &&
	    pthread_mutex_trylock(&audio_monitor->mutex) == 0) {
		audio_monitor_start(audio_monitor);
		pthread_mutex_unlock(&audio_monitor->mutex);
	}
#ifdef __APPLE__
	if (!os_atomic_load_bool(&audio_monitor->active))
		return audio;
#endif
	if (!audio_monitor->resampler ||
	    pthread_mutex_trylock(&audio_monitor->mutex) != 0)
		return audio;

	uint8_t *resample_data[MAX_AV_PLANES];
	uint32_t resample_frames;
	uint64_t ts_offset;
	bool success = audio_resampler_resample(
		audio_monitor->resampler, resample_data, &resample_frames,
		&ts_offset, (const uint8_t *const *)audio->data,
		(uint32_t)audio->frames);
	if (!success) {
		pthread_mutex_unlock(&audio_monitor->mutex);
		return audio;
	}
	/* apply volume */
	if (!close_float(audio_monitor->volume, 1.0f, EPSILON)) {
		register float *cur = (float *)resample_data[0];
		register float *end =
			cur + resample_frames * audio_monitor->channels;

		while (cur < end)
			*(cur++) *= audio_monitor->volume;
	}
#ifdef __APPLE__
	uint32_t bytes =
		sizeof(float) * audio_monitor->channels * resample_frames;
	circlebuf_push_back(&audio_monitor->new_data, resample_data[0], bytes);
	if (audio_monitor->new_data.size >= audio_monitor->wait_size) {
		audio_monitor->wait_size = 0;

		while (audio_monitor->empty_buffers.size > 0) {
			if (!fill_buffer(audio_monitor)) {
				break;
			}
		}

		if (audio_monitor->paused) {
			AudioQueueStart(audio_monitor->queue, NULL);
			audio_monitor->paused = false;
		}
	}
#endif
#ifdef WIN32

	UINT32 pad = 0;
	audio_monitor->client->lpVtbl->GetCurrentPadding(audio_monitor->client,
							 &pad);
	BYTE *output;
	HRESULT hr = audio_monitor->render->lpVtbl->GetBuffer(
		audio_monitor->render, resample_frames, &output);
	if (FAILED(hr)) {
		audio_monitor_stop(audio_monitor);
		pthread_mutex_unlock(&audio_monitor->mutex);
		return audio;
	}

	memcpy(output, resample_data[0],
	       resample_frames * audio_monitor->channels * sizeof(float));
	audio_monitor->render->lpVtbl->ReleaseBuffer(audio_monitor->render,
						     resample_frames, 0);
#endif
	pthread_mutex_unlock(&audio_monitor->mutex);
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
