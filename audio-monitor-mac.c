
#include "audio-monitor-mac.h"
#include <AudioUnit/AudioUnit.h>
#include <AudioToolbox/AudioQueue.h>
#include <CoreFoundation/CFString.h>
#include <CoreAudio/CoreAudio.h>
#include <util/circlebuf.h>
#include <obs-module.h>

#include "media-io/audio-resampler.h"
#include "util/platform.h"
#include "util/threading.h"

static bool success_(OSStatus stat, const char *func, const char *call)
{
	if (stat != noErr) {
		blog(LOG_WARNING, "%s: %s failed: %d", func, call, (int)stat);
		return false;
	}

	return true;
}

#define success(stat, call) success_(stat, __FUNCTION__, call)

struct audio_monitor {
	AudioQueueRef queue;
	AudioQueueBufferRef buffers[3];
	size_t buffer_size;
	size_t wait_size;
	struct circlebuf empty_buffers;
	struct circlebuf new_data;
	volatile bool active;
	bool paused;
    uint32_t channels;
	audio_resampler_t *resampler;
	float volume;
	pthread_mutex_t mutex;
    char *device_id;
};

static inline bool fill_buffer(struct audio_monitor *monitor)
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
	struct audio_monitor *monitor = data;

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

void audio_monitor_stop(struct audio_monitor *audio_monitor){
	if (!audio_monitor)
		return;

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
    audio_resampler_destroy(audio_monitor->resampler);
	audio_monitor->resampler = NULL;
}

void audio_monitor_start(struct audio_monitor *audio_monitor){
	if (!audio_monitor)
		return;
    const struct audio_output_info *info =
		audio_output_get_info(obs_get_audio());
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

}

void audio_monitor_audio(void *data, struct obs_audio_data *audio){
	struct audio_monitor *audio_monitor = data;
	if (!audio_monitor->resampler && audio_monitor->device_id &&
	    strlen(audio_monitor->device_id) &&
	    pthread_mutex_trylock(&audio_monitor->mutex) == 0) {
		audio_monitor_start(audio_monitor);
		pthread_mutex_unlock(&audio_monitor->mutex);
	}
    if (!os_atomic_load_bool(&audio_monitor->active))
		return;
	if (!audio_monitor->resampler ||
	    pthread_mutex_trylock(&audio_monitor->mutex) != 0)
		return;

	uint8_t *resample_data[MAX_AV_PLANES];
	uint32_t resample_frames;
	uint64_t ts_offset;
	bool success = audio_resampler_resample(
		audio_monitor->resampler, resample_data, &resample_frames,
		&ts_offset, (const uint8_t *const *)audio->data,
		(uint32_t)audio->frames);
	if (!success) {
		pthread_mutex_unlock(&audio_monitor->mutex);
		return;
	}
	/* apply volume */
	if (!close_float(audio_monitor->volume, 1.0f, EPSILON)) {
		register float *cur = (float *)resample_data[0];
		register float *end =
			cur + resample_frames * audio_monitor->channels;

		while (cur < end)
			*(cur++) *= audio_monitor->volume;
	}
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
    pthread_mutex_unlock(&audio_monitor->mutex);
}

void audio_monitor_set_volume(struct audio_monitor *audio_monitor, float volume){
	if (!audio_monitor)
		return;
    audio_monitor->volume = volume;
}

struct audio_monitor *audio_monitor_create(const char *device_id, const char* source_name, int port){
	struct audio_monitor *audio_monitor = bzalloc(sizeof(struct audio_monitor));
	audio_monitor->device_id = bstrdup(device_id);
	pthread_mutex_init(&audio_monitor->mutex, NULL);
	return audio_monitor;
}

void audio_monitor_destroy(struct audio_monitor *audio_monitor){
	if (!audio_monitor)
		return;
    audio_monitor_stop(audio_monitor);
	pthread_mutex_destroy(&audio_monitor->mutex);
    bfree(audio_monitor->device_id);
	bfree(audio_monitor);
}

const char *audio_monitor_get_device_id(struct audio_monitor *audio_monitor){
	if (!audio_monitor)
		return NULL;
    return audio_monitor->device_id;
}

void audio_monitor_set_format(struct audio_monitor *audio_monitor,
			      enum audio_format format){
}

void audio_monitor_set_samples_per_sec(struct audio_monitor *audio_monitor,
				       long long samples_per_sec){
}
