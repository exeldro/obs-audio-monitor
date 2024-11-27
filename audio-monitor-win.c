#include "audio-monitor-win.h"

#include <obs.h>
#include <media-io/audio-resampler.h>
#include <util/threading.h>
#include "util/platform.h"

#define ACTUALLY_DEFINE_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
	EXTERN_C const GUID DECLSPEC_SELECTANY name = {l, w1, w2, {b1, b2, b3, b4, b5, b6, b7, b8}}

#define do_log(level, format, ...) \
	blog(level, "[audio monitoring: '%s'] " format, obs_source_get_name(monitor->source), ##__VA_ARGS__)

#define warn(format, ...) do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...) do_log(LOG_INFO, format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG, format, ##__VA_ARGS__)

ACTUALLY_DEFINE_GUID(CLSID_MMDeviceEnumerator, 0xBCDE0395, 0xE52F, 0x467C, 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
ACTUALLY_DEFINE_GUID(IID_IMMDeviceEnumerator, 0xA95664D2, 0x9614, 0x4F35, 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);
ACTUALLY_DEFINE_GUID(IID_IAudioClient, 0x1CB9AD4C, 0xDBFA, 0x4C32, 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2);
ACTUALLY_DEFINE_GUID(IID_IAudioRenderClient, 0xF294ACFC, 0x3146, 0x4483, 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2);

struct audio_monitor {
	IMMDevice *device;
	IAudioClient *client;
	IAudioRenderClient *render;
	uint32_t sample_rate;
	uint32_t channels;
	audio_resampler_t *resampler;
	float volume;
	pthread_mutex_t mutex;
	char *device_id;
	char *source_name;
	SOCKET sock;
	struct sockaddr_storage addrDest;
	uint32_t nuFrame;
	byte sr;
	enum audio_format format;
	long long samples_per_sec;
};

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

void audio_monitor_stop(struct audio_monitor *audio_monitor)
{
	if (!audio_monitor)
		return;

	pthread_mutex_lock(&audio_monitor->mutex);

	if (audio_monitor->client)
		audio_monitor->client->lpVtbl->Stop(audio_monitor->client);

	safe_release(audio_monitor->device);
	audio_monitor->device = NULL;
	safe_release(audio_monitor->client);
	audio_monitor->client = NULL;
	safe_release(audio_monitor->render);
	audio_monitor->render = NULL;
	audio_resampler_destroy(audio_monitor->resampler);
	audio_monitor->resampler = NULL;
	pthread_mutex_unlock(&audio_monitor->mutex);
}

void audio_monitor_start(struct audio_monitor *audio_monitor)
{
	if (!audio_monitor)
		return;

	pthread_mutex_lock(&audio_monitor->mutex);
	const struct audio_output_info *info = audio_output_get_info(obs_get_audio());
	struct resample_info to;
	struct resample_info from;
	from.samples_per_sec = info->samples_per_sec;
	from.speakers = info->speakers;
	from.format = AUDIO_FORMAT_FLOAT_PLANAR;
	if (!audio_monitor->sock) {
		IMMDeviceEnumerator *immde = NULL;
		HRESULT hr =
			CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&immde);
		if (FAILED(hr)) {
			pthread_mutex_unlock(&audio_monitor->mutex);
			return;
		}
		if (strcmp(audio_monitor->device_id, "default") == 0) {
			hr = immde->lpVtbl->GetDefaultAudioEndpoint(immde, eRender, eConsole, &audio_monitor->device);
		} else {
			wchar_t w_id[512];
			os_utf8_to_wcs(audio_monitor->device_id, 0, w_id, 512);

			hr = immde->lpVtbl->GetDevice(immde, w_id, &audio_monitor->device);
		}
		if (FAILED(hr)) {
			safe_release(immde);
			pthread_mutex_unlock(&audio_monitor->mutex);
			return;
		}
		hr = audio_monitor->device->lpVtbl->Activate(audio_monitor->device, &IID_IAudioClient, CLSCTX_ALL, NULL,
							     (void **)&audio_monitor->client);
		if (FAILED(hr)) {
			safe_release(immde);
			pthread_mutex_unlock(&audio_monitor->mutex);
			return;
		}
		WAVEFORMATEX *wfex = NULL;
		hr = audio_monitor->client->lpVtbl->GetMixFormat(audio_monitor->client, &wfex);
		if (FAILED(hr)) {
			safe_release(immde);
			pthread_mutex_unlock(&audio_monitor->mutex);
			return;
		}
		hr = audio_monitor->client->lpVtbl->Initialize(audio_monitor->client, AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0,
							       wfex, NULL);
		if (FAILED(hr)) {
			safe_release(immde);
			CoTaskMemFree(wfex);
			pthread_mutex_unlock(&audio_monitor->mutex);
			return;
		}
		WAVEFORMATEXTENSIBLE *ext = (WAVEFORMATEXTENSIBLE *)wfex;
		to.samples_per_sec = (uint32_t)wfex->nSamplesPerSec;
		to.speakers = convert_speaker_layout(ext->dwChannelMask, wfex->nChannels);
		to.format = AUDIO_FORMAT_FLOAT;
		audio_monitor->sample_rate = (uint32_t)wfex->nSamplesPerSec;
		audio_monitor->channels = wfex->nChannels;

		CoTaskMemFree(wfex);

		UINT32 frames;
		hr = audio_monitor->client->lpVtbl->GetBufferSize(audio_monitor->client, &frames);
		if (FAILED(hr)) {
			safe_release(immde);
			pthread_mutex_unlock(&audio_monitor->mutex);
			return;
		}
		hr = audio_monitor->client->lpVtbl->GetService(audio_monitor->client, &IID_IAudioRenderClient,
							       (void **)&audio_monitor->render);
		if (FAILED(hr)) {
			safe_release(immde);
			pthread_mutex_unlock(&audio_monitor->mutex);
			return;
		}
		hr = audio_monitor->client->lpVtbl->Start(audio_monitor->client);

		safe_release(immde);
	} else {
		audio_monitor->channels = info->speakers;
		if (!audio_monitor->samples_per_sec) {
			audio_monitor->samples_per_sec = info->samples_per_sec;
		}
		if (audio_monitor->samples_per_sec == 6000) {
			audio_monitor->sr = 0;
		} else if (audio_monitor->samples_per_sec == 12000) {
			audio_monitor->sr = 1;
		} else if (audio_monitor->samples_per_sec == 24000) {
			audio_monitor->sr = 2;
		} else if (audio_monitor->samples_per_sec == 48000) {
			audio_monitor->sr = 3;
		} else if (audio_monitor->samples_per_sec == 96000) {
			audio_monitor->sr = 4;
		} else if (audio_monitor->samples_per_sec == 192000) {
			audio_monitor->sr = 5;
		} else if (audio_monitor->samples_per_sec == 384000) {
			audio_monitor->sr = 6;
		} else if (audio_monitor->samples_per_sec == 8000) {
			audio_monitor->sr = 7;
		} else if (audio_monitor->samples_per_sec == 16000) {
			audio_monitor->sr = 8;
		} else if (audio_monitor->samples_per_sec == 32000) {
			audio_monitor->sr = 9;
		} else if (audio_monitor->samples_per_sec == 64000) {
			audio_monitor->sr = 10;
		} else if (audio_monitor->samples_per_sec == 128000) {
			audio_monitor->sr = 11;
		} else if (audio_monitor->samples_per_sec == 256000) {
			audio_monitor->sr = 12;
		} else if (audio_monitor->samples_per_sec == 512000) {
			audio_monitor->sr = 13;
		} else if (audio_monitor->samples_per_sec == 11025) {
			audio_monitor->sr = 14;
		} else if (audio_monitor->samples_per_sec == 22050) {
			audio_monitor->sr = 15;
		} else if (audio_monitor->samples_per_sec == 44100) {
			audio_monitor->sr = 16;
		} else if (audio_monitor->samples_per_sec == 88200) {
			audio_monitor->sr = 17;
		} else if (audio_monitor->samples_per_sec == 176400) {
			audio_monitor->sr = 18;
		} else if (audio_monitor->samples_per_sec == 352800) {
			audio_monitor->sr = 19;
		} else if (audio_monitor->samples_per_sec == 705600) {
			audio_monitor->sr = 20;
		}

		to.samples_per_sec = (uint32_t)audio_monitor->samples_per_sec;
		to.speakers = info->speakers;
		to.format = audio_monitor->format;
	}

	audio_monitor->resampler = audio_resampler_create(&to, &from);
	pthread_mutex_unlock(&audio_monitor->mutex);
}

void audio_monitor_audio(void *data, struct obs_audio_data *audio)
{
	struct audio_monitor *audio_monitor = data;
	if (!audio_monitor->resampler && audio_monitor->device_id && strlen(audio_monitor->device_id)) {
		audio_monitor_start(audio_monitor);
	}
	if (!audio_monitor->resampler || pthread_mutex_trylock(&audio_monitor->mutex) != 0)
		return;

	uint8_t *resample_data[MAX_AV_PLANES];
	uint32_t resample_frames;
	uint64_t ts_offset;
	bool success = audio_resampler_resample(audio_monitor->resampler, resample_data, &resample_frames, &ts_offset,
						(const uint8_t *const *)audio->data, (uint32_t)audio->frames);
	if (!success) {
		pthread_mutex_unlock(&audio_monitor->mutex);
		return;
	}
	/* apply volume */
	float vol = audio_monitor->volume;
	if (!close_float(vol, 1.0f, EPSILON)) {
		if (audio_monitor->format == AUDIO_FORMAT_FLOAT) {
			register float *cur = (float *)resample_data[0];
			register float *end = cur + resample_frames * audio_monitor->channels;

			while (cur < end)
				*(cur++) *= vol;
		} else if (audio_monitor->format == AUDIO_FORMAT_32BIT) {
			register int32_t *cur = (int32_t *)resample_data[0];
			register int32_t *end = cur + resample_frames * audio_monitor->channels;

			while (cur < end) {
				*cur = (int32_t)((float)*cur * vol);
				cur++;
			}
		} else if (audio_monitor->format == AUDIO_FORMAT_16BIT) {
			register int16_t *cur = (int16_t *)resample_data[0];
			register int16_t *end = cur + resample_frames * audio_monitor->channels;

			while (cur < end) {
				*cur = (int16_t)((float)*cur * vol);
				cur++;
			}
		} else if (audio_monitor->format == AUDIO_FORMAT_U8BIT) {
			register uint8_t *cur = resample_data[0];
			register uint8_t *end = cur + resample_frames * audio_monitor->channels;

			while (cur < end) {
				*cur = (uint8_t)((float)*cur * vol);
				cur++;
			}
		}
	}

	if (audio_monitor->sock) {

		size_t sample_size = audio_monitor->channels;
		if (audio_monitor->format == AUDIO_FORMAT_16BIT) {
			sample_size *= 2;
		} else if (audio_monitor->format == AUDIO_FORMAT_U8BIT) {

		} else {
			sample_size *= 4;
		}
		size_t frames_per_packet = 1436 / sample_size;
		for (size_t pos = 0; pos < resample_frames; pos += frames_per_packet) {
			size_t msg_length = 28 + sample_size * (pos + frames_per_packet <= resample_frames ? frames_per_packet
													   : resample_frames - pos);
			byte *msg = bzalloc(msg_length);
			msg[0] = 'V';
			msg[1] = 'B';
			msg[2] = 'A';
			msg[3] = 'N';
			msg[4] = audio_monitor->sr;

			if (pos + frames_per_packet <= resample_frames) {
				msg[5] = (byte)(frames_per_packet - 1);
			} else {
				msg[5] = (byte)(resample_frames - pos - 1);
			}
			msg[6] = audio_monitor->channels - 1;
			if (audio_monitor->format == AUDIO_FORMAT_U8BIT) {
				msg[7] = 0;
			} else if (audio_monitor->format == AUDIO_FORMAT_16BIT) {
				msg[7] = 1;
			} else if (audio_monitor->format == AUDIO_FORMAT_32BIT) {
				msg[7] = 3;
			} else {
				msg[7] = 4;
			}

			const size_t len = strlen(audio_monitor->source_name);
			memcpy(msg + 8, audio_monitor->source_name, len > 16 ? 16 : len);

			memcpy(msg + 24, &audio_monitor->nuFrame, sizeof(uint32_t));
			audio_monitor->nuFrame++;

			memcpy(msg + 28, resample_data[0] + pos * sample_size, msg_length - 28);
			int result = sendto(audio_monitor->sock, msg, (int)msg_length, 0,
					    (struct sockaddr *)&audio_monitor->addrDest, sizeof(audio_monitor->addrDest));
			bfree(msg);
		}

		pthread_mutex_unlock(&audio_monitor->mutex);
		return;
	}

	UINT32 pad = 0;
	HRESULT hr = audio_monitor->client->lpVtbl->GetCurrentPadding(audio_monitor->client, &pad);
	if (FAILED(hr)) {
		pthread_mutex_unlock(&audio_monitor->mutex);
		audio_monitor_stop(audio_monitor);
		return;
	}
	BYTE *output;
	hr = audio_monitor->render->lpVtbl->GetBuffer(audio_monitor->render, resample_frames, &output);
	if (FAILED(hr)) {
		pthread_mutex_unlock(&audio_monitor->mutex);
		audio_monitor_stop(audio_monitor);
		return;
	}

	memcpy(output, resample_data[0], resample_frames * audio_monitor->channels * sizeof(float));
	audio_monitor->render->lpVtbl->ReleaseBuffer(audio_monitor->render, resample_frames, 0);
	pthread_mutex_unlock(&audio_monitor->mutex);
}

void audio_monitor_set_volume(struct audio_monitor *audio_monitor, float volume)
{
	if (!audio_monitor)
		return;
	audio_monitor->volume = volume;
}

int resolvehelper(const char *hostname, int family, const char *service, struct sockaddr_storage *pAddr)
{
	int result;
	struct addrinfo *result_list = NULL;
	struct addrinfo hints = {0};
	hints.ai_family = family;
	hints.ai_socktype =
		SOCK_DGRAM; // without this flag, getaddrinfo will return 3x the number of addresses (one for each socket type).
	result = getaddrinfo(hostname, service, &hints, &result_list);
	if (result == 0) {
		//ASSERT(result_list->ai_addrlen <= sizeof(sockaddr_in));
		memcpy(pAddr, result_list->ai_addr, result_list->ai_addrlen);
		freeaddrinfo(result_list);
	}

	return result;
}

struct audio_monitor *audio_monitor_create(const char *device_id, const char *source_name, int port)
{
	struct audio_monitor *audio_monitor = bzalloc(sizeof(struct audio_monitor));
	audio_monitor->device_id = bstrdup(device_id);
	audio_monitor->source_name = bstrdup(source_name);
	audio_monitor->volume = 1.0f;
	audio_monitor->format = AUDIO_FORMAT_FLOAT;
	pthread_mutex_init(&audio_monitor->mutex, NULL);
	if (port) {
		char buffer[10];
		snprintf(buffer, 10, "%d", port);
		audio_monitor->sock = socket(AF_INET, SOCK_DGRAM, 0);
		resolvehelper(device_id, AF_INET, buffer, &audio_monitor->addrDest);
	}
	return audio_monitor;
}

void audio_monitor_destroy(struct audio_monitor *audio_monitor)
{
	if (!audio_monitor)
		return;
	audio_monitor_stop(audio_monitor);
	if (audio_monitor->sock)
		closesocket(audio_monitor->sock);
	bfree(audio_monitor->source_name);
	bfree(audio_monitor->device_id);
	bfree(audio_monitor);
}

const char *audio_monitor_get_device_id(struct audio_monitor *audio_monitor)
{
	if (!audio_monitor)
		return NULL;
	return audio_monitor->device_id;
}

void audio_monitor_set_format(struct audio_monitor *audio_monitor, enum audio_format format)
{
	if (!format || audio_monitor->format == format || format > AUDIO_FORMAT_FLOAT)
		return;
	audio_monitor->format = format;
	if (audio_monitor->resampler) {
		audio_monitor_stop(audio_monitor);
		audio_monitor_start(audio_monitor);
	}
}

void audio_monitor_set_samples_per_sec(struct audio_monitor *audio_monitor, long long samples_per_sec)
{
	if (samples_per_sec <= 0 || audio_monitor->samples_per_sec == samples_per_sec)
		return;
	audio_monitor->samples_per_sec = samples_per_sec;
	if (audio_monitor->resampler) {
		audio_monitor_stop(audio_monitor);
		audio_monitor_start(audio_monitor);
	}
}
