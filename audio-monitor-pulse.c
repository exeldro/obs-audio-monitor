#include "audio-monitor-pulse.h"
#include <obs.h>
#include <util/threading.h>
#if LIBOBS_API_VER >= MAKE_SEMANTIC_VERSION(30, 1, 0)
#include <util/deque.h>
#define circlebuf_peek_front deque_peek_front
#define circlebuf_peek_back deque_peek_back
#define circlebuf_push_front deque_push_front
#define circlebuf_push_back deque_push_back
#define circlebuf_pop_front deque_pop_front
#define circlebuf_pop_back deque_pop_back
#define circlebuf_init deque_init
#define circlebuf_free deque_free
#else
#include <util/circlebuf.h>
#endif
#include <media-io/audio-resampler.h>
#include <pulse/stream.h>
#include <pulse/introspect.h>
#include <pulse/thread-mainloop.h>

static uint_fast32_t pulseaudio_refs = 0;
static pthread_mutex_t pulseaudio_mutex = PTHREAD_MUTEX_INITIALIZER;
static pa_threaded_mainloop *pulseaudio_mainloop = NULL;
static pa_context *pulseaudio_context = NULL;

struct audio_monitor {
	pa_stream *stream;
	pa_buffer_attr attr;
	enum speaker_layout speakers;
	pa_sample_format_t format;
	uint_fast32_t samples_per_sec;
	uint_fast32_t bytes_per_frame;

	uint_fast8_t channels;

#if LIBOBS_API_VER >= MAKE_SEMANTIC_VERSION(30, 1, 0)
	struct deque new_data;
#else
	struct circlebuf new_data;
#endif

	size_t buffer_size;
	size_t bytesRemaining;
	size_t bytes_per_channel;

	audio_resampler_t *resampler;
	float volume;
	bool mono;
	float balance;
	pthread_mutex_t mutex;
	char *device_id;
	char *source_name;
};

struct pulseaudio_default_output {
	char *default_sink_name;
};

void pulseaudio_lock()
{
	pa_threaded_mainloop_lock(pulseaudio_mainloop);
}

void pulseaudio_unlock()
{
	pa_threaded_mainloop_unlock(pulseaudio_mainloop);
}

void pulseaudio_wait()
{
	pa_threaded_mainloop_wait(pulseaudio_mainloop);
}

void pulseaudio_signal(int wait_for_accept)
{
	pa_threaded_mainloop_signal(pulseaudio_mainloop, wait_for_accept);
}

static void pulseaudio_context_state_changed(pa_context *c, void *userdata)
{
	UNUSED_PARAMETER(userdata);
	UNUSED_PARAMETER(c);

	pulseaudio_signal(0);
}

static pa_proplist *pulseaudio_properties()
{
	pa_proplist *p = pa_proplist_new();

	pa_proplist_sets(p, PA_PROP_APPLICATION_NAME, "OBS");
	pa_proplist_sets(p, PA_PROP_APPLICATION_ICON_NAME, "obs");
	pa_proplist_sets(p, PA_PROP_MEDIA_ROLE, "production");

	return p;
}

static void pulseaudio_init_context()
{
	pulseaudio_lock();

	pa_proplist *p = pulseaudio_properties();
	pulseaudio_context = pa_context_new_with_proplist(pa_threaded_mainloop_get_api(pulseaudio_mainloop), "OBS-Monitor", p);

	pa_context_set_state_callback(pulseaudio_context, pulseaudio_context_state_changed, NULL);

	pa_context_connect(pulseaudio_context, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL);
	pa_proplist_free(p);

	pulseaudio_unlock();
}

int_fast32_t pulseaudio_init()
{
	pthread_mutex_lock(&pulseaudio_mutex);

	if (pulseaudio_refs == 0) {
		pulseaudio_mainloop = pa_threaded_mainloop_new();
		pa_threaded_mainloop_start(pulseaudio_mainloop);

		pulseaudio_init_context();
	}

	pulseaudio_refs++;

	pthread_mutex_unlock(&pulseaudio_mutex);

	return 0;
}

static int_fast32_t pulseaudio_context_ready()
{
	pulseaudio_lock();

	if (!PA_CONTEXT_IS_GOOD(pa_context_get_state(pulseaudio_context))) {
		pulseaudio_unlock();
		return -1;
	}

	while (pa_context_get_state(pulseaudio_context) != PA_CONTEXT_READY)
		pulseaudio_wait();

	pulseaudio_unlock();
	return 0;
}

int_fast32_t pulseaudio_get_server_info(pa_server_info_cb_t cb, void *userdata)
{
	if (pulseaudio_context_ready() < 0)
		return -1;

	pulseaudio_lock();

	pa_operation *op = pa_context_get_server_info(pulseaudio_context, cb, userdata);
	if (!op) {
		pulseaudio_unlock();
		return -1;
	}
	while (pa_operation_get_state(op) == PA_OPERATION_RUNNING)
		pulseaudio_wait();
	pa_operation_unref(op);

	pulseaudio_unlock();
	return 0;
}

pa_stream *pulseaudio_stream_new(const char *name, const pa_sample_spec *ss, const pa_channel_map *map)
{
	if (pulseaudio_context_ready() < 0)
		return NULL;

	pulseaudio_lock();

	pa_proplist *p = pulseaudio_properties();
	pa_stream *s = pa_stream_new_with_proplist(pulseaudio_context, name, ss, map, p);
	pa_proplist_free(p);

	pulseaudio_unlock();
	return s;
}

int_fast32_t pulseaudio_connect_playback(pa_stream *s, const char *name, const pa_buffer_attr *attr, pa_stream_flags_t flags)
{
	if (pulseaudio_context_ready() < 0)
		return -1;

	size_t dev_len = strlen(name);
	char *device = bzalloc(dev_len + 1);
	memcpy(device, name, dev_len);

	pulseaudio_lock();
	int_fast32_t ret = pa_stream_connect_playback(s, device, attr, flags, NULL, NULL);
	pulseaudio_unlock();

	bfree(device);
	return ret;
}

static void pulseaudio_default_devices(pa_context *c, const pa_server_info *i, void *userdata)
{
	UNUSED_PARAMETER(c);
	struct pulseaudio_default_output *d = (struct pulseaudio_default_output *)userdata;
	d->default_sink_name = bstrdup(i->default_sink_name);
	pulseaudio_signal(0);
}

void pulseaudio_unref()
{
	pthread_mutex_lock(&pulseaudio_mutex);

	if (--pulseaudio_refs == 0) {
		pulseaudio_lock();
		if (pulseaudio_context != NULL) {
			pa_context_disconnect(pulseaudio_context);
			pa_context_unref(pulseaudio_context);
			pulseaudio_context = NULL;
		}
		pulseaudio_unlock();

		if (pulseaudio_mainloop != NULL) {
			pa_threaded_mainloop_stop(pulseaudio_mainloop);
			pa_threaded_mainloop_free(pulseaudio_mainloop);
			pulseaudio_mainloop = NULL;
		}
	}

	pthread_mutex_unlock(&pulseaudio_mutex);
}

void get_default_id(char **id)
{
	pulseaudio_init();
	struct pulseaudio_default_output *pdo = bzalloc(sizeof(struct pulseaudio_default_output));
	pulseaudio_get_server_info((pa_server_info_cb_t)pulseaudio_default_devices, (void *)pdo);

	if (!pdo->default_sink_name || !*pdo->default_sink_name) {
		*id = NULL;
	} else {
		*id = pdo->default_sink_name;
	}

	bfree(pdo);
	pulseaudio_unref();
}

static void pulseaudio_server_info(pa_context *c, const pa_server_info *i, void *userdata)
{
	UNUSED_PARAMETER(c);
	UNUSED_PARAMETER(userdata);

	blog(LOG_INFO, "Server name: '%s %s'", i->server_name, i->server_version);

	pulseaudio_signal(0);
}

int_fast32_t pulseaudio_get_sink_info(pa_sink_info_cb_t cb, const char *name, void *userdata)
{
	if (pulseaudio_context_ready() < 0)
		return -1;

	pulseaudio_lock();

	pa_operation *op = pa_context_get_sink_info_by_name(pulseaudio_context, name, cb, userdata);
	if (!op) {
		pulseaudio_unlock();
		return -1;
	}
	while (pa_operation_get_state(op) == PA_OPERATION_RUNNING)
		pulseaudio_wait();
	pa_operation_unref(op);

	pulseaudio_unlock();

	return 0;
}

static enum audio_format pulseaudio_to_obs_audio_format(pa_sample_format_t format)
{
	switch (format) {
	case PA_SAMPLE_U8:
		return AUDIO_FORMAT_U8BIT;
	case PA_SAMPLE_S16LE:
		return AUDIO_FORMAT_16BIT;
	case PA_SAMPLE_S32LE:
		return AUDIO_FORMAT_32BIT;
	case PA_SAMPLE_FLOAT32LE:
		return AUDIO_FORMAT_FLOAT;
	default:
		return AUDIO_FORMAT_UNKNOWN;
	}
}

static pa_channel_map pulseaudio_channel_map(enum speaker_layout layout)
{
	pa_channel_map ret;

	ret.map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
	ret.map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
	ret.map[2] = PA_CHANNEL_POSITION_FRONT_CENTER;
	ret.map[3] = PA_CHANNEL_POSITION_LFE;
	ret.map[4] = PA_CHANNEL_POSITION_REAR_LEFT;
	ret.map[5] = PA_CHANNEL_POSITION_REAR_RIGHT;
	ret.map[6] = PA_CHANNEL_POSITION_SIDE_LEFT;
	ret.map[7] = PA_CHANNEL_POSITION_SIDE_RIGHT;

	switch (layout) {
	case SPEAKERS_MONO:
		ret.channels = 1;
		ret.map[0] = PA_CHANNEL_POSITION_MONO;
		break;

	case SPEAKERS_STEREO:
		ret.channels = 2;
		break;

	case SPEAKERS_2POINT1:
		ret.channels = 3;
		ret.map[2] = PA_CHANNEL_POSITION_LFE;
		break;

	case SPEAKERS_4POINT0:
		ret.channels = 4;
		ret.map[3] = PA_CHANNEL_POSITION_REAR_CENTER;
		break;

	case SPEAKERS_4POINT1:
		ret.channels = 5;
		ret.map[4] = PA_CHANNEL_POSITION_REAR_CENTER;
		break;

	case SPEAKERS_5POINT1:
		ret.channels = 6;
		break;

	case SPEAKERS_7POINT1:
		ret.channels = 8;
		break;

	case SPEAKERS_UNKNOWN:
	default:
		ret.channels = 0;
		break;
	}

	return ret;
}

static enum speaker_layout pulseaudio_channels_to_obs_speakers(uint_fast32_t channels)
{
	switch (channels) {
	case 0:
		return SPEAKERS_UNKNOWN;
	case 1:
		return SPEAKERS_MONO;
	case 2:
		return SPEAKERS_STEREO;
	case 3:
		return SPEAKERS_2POINT1;
	case 4:
		return SPEAKERS_4POINT0;
	case 5:
		return SPEAKERS_4POINT1;
	case 6:
		return SPEAKERS_5POINT1;
	case 8:
		return SPEAKERS_7POINT1;
	default:
		return SPEAKERS_UNKNOWN;
	}
}

static void pulseaudio_sink_info(pa_context *c, const pa_sink_info *i, int eol, void *userdata)
{
	UNUSED_PARAMETER(c);
	struct audio_monitor *data = userdata;
	// An error occurred
	if (eol < 0) {
		data->format = PA_SAMPLE_INVALID;
		goto skip;
	}
	// Terminating call for multi instance callbacks
	if (eol > 0)
		goto skip;

	blog(LOG_INFO, "Audio format: %s, %" PRIu32 " Hz, %" PRIu8 " channels", pa_sample_format_to_string(i->sample_spec.format),
	     i->sample_spec.rate, i->sample_spec.channels);

	pa_sample_format_t format = i->sample_spec.format;
	if (pulseaudio_to_obs_audio_format(format) == AUDIO_FORMAT_UNKNOWN) {
		format = PA_SAMPLE_FLOAT32LE;

		blog(LOG_INFO,
		     "Sample format %s not supported by OBS,"
		     "using %s instead for recording",
		     pa_sample_format_to_string(i->sample_spec.format), pa_sample_format_to_string(format));
	}

	uint8_t channels = i->sample_spec.channels;
	if (pulseaudio_channels_to_obs_speakers(channels) == SPEAKERS_UNKNOWN) {
		channels = 2;

		blog(LOG_INFO,
		     "%c channels not supported by OBS,"
		     "using %c instead for recording",
		     i->sample_spec.channels, channels);
	}

	data->format = format;
	data->samples_per_sec = i->sample_spec.rate;
	data->channels = channels;
skip:
	pulseaudio_signal(0);
}

void pulseaudio_write_callback(pa_stream *p, pa_stream_request_cb_t cb, void *userdata)
{
	if (pulseaudio_context_ready() < 0)
		return;

	pulseaudio_lock();
	pa_stream_set_write_callback(p, cb, userdata);
	pulseaudio_unlock();
}

void pulseaudio_set_underflow_callback(pa_stream *p, pa_stream_notify_cb_t cb, void *userdata)
{
	if (pulseaudio_context_ready() < 0)
		return;

	pulseaudio_lock();
	pa_stream_set_underflow_callback(p, cb, userdata);
	pulseaudio_unlock();
}

static void pulseaudio_stream_write(pa_stream *p, size_t nbytes, void *userdata)
{
	UNUSED_PARAMETER(p);
	struct audio_monitor *data = userdata;

	pthread_mutex_lock(&data->mutex);
	data->bytesRemaining += nbytes;
	pthread_mutex_unlock(&data->mutex);

	pulseaudio_signal(0);
}

static void pulseaudio_underflow(pa_stream *p, void *userdata)
{
	UNUSED_PARAMETER(p);
	struct audio_monitor *data = userdata;

	pthread_mutex_lock(&data->mutex);
	data->attr.tlength = (data->attr.tlength * 3) / 2;
	pa_stream_set_buffer_attr(data->stream, &data->attr, NULL, NULL);
	pthread_mutex_unlock(&data->mutex);

	pulseaudio_signal(0);
}

void audio_monitor_stop(struct audio_monitor *audio_monitor)
{
	if (!audio_monitor)
		return;

	if (audio_monitor->stream) {
		/* Stop the stream */
		pulseaudio_lock();
		pa_stream_disconnect(audio_monitor->stream);
		pulseaudio_unlock();

		/* Remove the callbacks, to ensure we no longer try to do anything
        * with this stream object */
		pulseaudio_write_callback(audio_monitor->stream, NULL, NULL);
		pulseaudio_set_underflow_callback(audio_monitor->stream, NULL, NULL);

		/* Unreference the stream and drop it. PA will free it when it can. */
		pulseaudio_lock();
		pa_stream_unref(audio_monitor->stream);
		pulseaudio_unlock();

		audio_monitor->stream = NULL;
	}

	blog(LOG_INFO, "Stopped Monitoring in '%s'", audio_monitor->device_id);

	audio_resampler_destroy(audio_monitor->resampler);
	audio_monitor->resampler = NULL;
}

void audio_monitor_start(struct audio_monitor *audio_monitor)
{
	if (!audio_monitor)
		return;
	pulseaudio_init();
	char *device = NULL;
	if (strcmp(audio_monitor->device_id, "default") == 0) {
		get_default_id(&device);
	} else {
		device = bstrdup(audio_monitor->device_id);
	}
	if (!device)
		return;
	if (pulseaudio_get_server_info(pulseaudio_server_info, (void *)audio_monitor) < 0) {
		blog(LOG_ERROR, "Unable to get server info !");
		bfree(device);
		return;
	}
	if (pulseaudio_get_sink_info(pulseaudio_sink_info, device, (void *)audio_monitor) < 0) {
		blog(LOG_ERROR, "Unable to get source info !");
		bfree(device);
		return;
	}
	bfree(device);

	if (audio_monitor->format == PA_SAMPLE_INVALID) {
		blog(LOG_ERROR, "An error occurred while getting the source info!");
		return;
	}

	pa_sample_spec spec;
	spec.format = audio_monitor->format;
	spec.rate = (uint32_t)audio_monitor->samples_per_sec;
	spec.channels = audio_monitor->channels;

	if (!pa_sample_spec_valid(&spec)) {
		blog(LOG_ERROR, "Sample spec is not valid");
		return;
	}

	const struct audio_output_info *info = audio_output_get_info(obs_get_audio());

	struct resample_info from = {.samples_per_sec = info->samples_per_sec,
				     .speakers = info->speakers,
				     .format = AUDIO_FORMAT_FLOAT_PLANAR};
	struct resample_info to = {.samples_per_sec = (uint32_t)audio_monitor->samples_per_sec,
				   .speakers = pulseaudio_channels_to_obs_speakers(audio_monitor->channels),
				   .format = pulseaudio_to_obs_audio_format(audio_monitor->format)};

	audio_monitor->resampler = audio_resampler_create(&to, &from);

	if (!audio_monitor->resampler) {
		blog(LOG_WARNING, "%s: %s", __FUNCTION__, "Failed to create resampler");
		return;
	}

	audio_monitor->bytes_per_channel = get_audio_bytes_per_channel(pulseaudio_to_obs_audio_format(audio_monitor->format));
	audio_monitor->speakers = pulseaudio_channels_to_obs_speakers(spec.channels);
	audio_monitor->bytes_per_frame = pa_frame_size(&spec);

	pa_channel_map channel_map = pulseaudio_channel_map(audio_monitor->speakers);

	audio_monitor->stream = pulseaudio_stream_new(audio_monitor->source_name, &spec, &channel_map);
	if (!audio_monitor->stream) {
		blog(LOG_ERROR, "Unable to create stream");
		return;
	}

	audio_monitor->attr.fragsize = (uint32_t)-1;
	audio_monitor->attr.maxlength = (uint32_t)-1;
	audio_monitor->attr.minreq = (uint32_t)-1;
	audio_monitor->attr.prebuf = (uint32_t)-1;
	audio_monitor->attr.tlength = pa_usec_to_bytes(25000, &spec);

	audio_monitor->buffer_size = audio_monitor->bytes_per_frame * pa_usec_to_bytes(5000, &spec);

	pa_stream_flags_t flags = PA_STREAM_INTERPOLATE_TIMING | PA_STREAM_AUTO_TIMING_UPDATE;

	int_fast32_t ret =
		pulseaudio_connect_playback(audio_monitor->stream, audio_monitor->device_id, &audio_monitor->attr, flags);
	if (ret < 0) {
		audio_monitor_stop(audio_monitor);
		blog(LOG_ERROR, "Unable to connect to stream");
		return;
	}

	blog(LOG_INFO, "Started Monitoring in '%s'", audio_monitor->device_id);

	pulseaudio_write_callback(audio_monitor->stream, pulseaudio_stream_write, (void *)audio_monitor);

	pulseaudio_set_underflow_callback(audio_monitor->stream, pulseaudio_underflow, (void *)audio_monitor);
}

static void do_stream_write(void *param)
{
	struct audio_monitor *data = param;
	uint8_t *buffer = NULL;

	while (data->new_data.size >= data->buffer_size && data->bytesRemaining > 0) {
		size_t bytesToFill = data->buffer_size;

		if (bytesToFill > data->bytesRemaining)
			bytesToFill = data->bytesRemaining;

		pulseaudio_lock();
		pa_stream_begin_write(data->stream, (void **)&buffer, &bytesToFill);
		pulseaudio_unlock();

		circlebuf_pop_front(&data->new_data, buffer, bytesToFill);

		pulseaudio_lock();
		pa_stream_write(data->stream, buffer, bytesToFill, NULL, 0LL, PA_SEEK_RELATIVE);
		pulseaudio_unlock();

		data->bytesRemaining -= bytesToFill;
	}
}

void audio_monitor_audio(void *data, struct obs_audio_data *audio)
{
	struct audio_monitor *audio_monitor = data;
	if (!audio_monitor->resampler && audio_monitor->device_id && strlen(audio_monitor->device_id) &&
	    pthread_mutex_trylock(&audio_monitor->mutex) == 0) {
		audio_monitor_start(audio_monitor);
		pthread_mutex_unlock(&audio_monitor->mutex);
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
		if (pulseaudio_to_obs_audio_format(audio_monitor->format) == AUDIO_FORMAT_FLOAT) {
			register float *cur = (float *)resample_data[0];
			register float *end = cur + resample_frames * audio_monitor->channels;

			while (cur < end)
				*(cur++) *= vol;
		} else if (pulseaudio_to_obs_audio_format(audio_monitor->format) == AUDIO_FORMAT_32BIT) {
			register int32_t *cur = (int32_t *)resample_data[0];
			register int32_t *end = cur + resample_frames * audio_monitor->channels;
			while (cur < end) {
				*cur = (int32_t)((float)*cur * vol);
				cur++;
			}
		} else if (pulseaudio_to_obs_audio_format(audio_monitor->format) == AUDIO_FORMAT_16BIT) {
			register int16_t *cur = (int16_t *)resample_data[0];
			register int16_t *end = cur + resample_frames * audio_monitor->channels;

			while (cur < end) {
				*cur = (int16_t)((float)*cur * vol);
				cur++;
			}
		} else if (pulseaudio_to_obs_audio_format(audio_monitor->format) == AUDIO_FORMAT_U8BIT) {
			register uint8_t *cur = resample_data[0];
			register uint8_t *end = cur + resample_frames * audio_monitor->channels;

			while (cur < end) {
				*cur = (uint8_t)((float)*cur * vol);
				cur++;
			}
		}
	}

	/* apply mono */
	if (audio_monitor->mono && audio_monitor->channels > 1) {
		if (pulseaudio_to_obs_audio_format(audio_monitor->format) == AUDIO_FORMAT_FLOAT) {
			for (uint32_t frame = 0; frame < resample_frames; frame++) {
				float avg = 0.0f;
				for (uint32_t channel = 0; channel < audio_monitor->channels; channel++) {
					avg += ((float *)resample_data[0])[frame * audio_monitor->channels + channel];
				}
				avg /= (float)audio_monitor->channels;
				for (uint32_t channel = 0; channel < audio_monitor->channels; channel++) {
					((float *)resample_data[0])[frame * audio_monitor->channels + channel] = avg;
				}
			}
		} else if (pulseaudio_to_obs_audio_format(audio_monitor->format) == AUDIO_FORMAT_32BIT) {
			for (uint32_t frame = 0; frame < resample_frames; frame++) {
				int64_t avg = 0;
				for (uint32_t channel = 0; channel < audio_monitor->channels; channel++) {
					avg += ((int32_t *)resample_data[0])[frame * audio_monitor->channels + channel];
				}
				avg /= audio_monitor->channels;
				for (uint32_t channel = 0; channel < audio_monitor->channels; channel++) {
					((int32_t *)resample_data[0])[frame * audio_monitor->channels + channel] = (int32_t)avg;
				}
			}
		} else if (pulseaudio_to_obs_audio_format(audio_monitor->format) == AUDIO_FORMAT_16BIT) {
			for (uint32_t frame = 0; frame < resample_frames; frame++) {
				int64_t avg = 0;
				for (uint32_t channel = 0; channel < audio_monitor->channels; channel++) {
					avg += ((int16_t *)resample_data[0])[frame * audio_monitor->channels + channel];
				}
				avg /= audio_monitor->channels;
				for (uint32_t channel = 0; channel < audio_monitor->channels; channel++) {
					((int16_t *)resample_data[0])[frame * audio_monitor->channels + channel] = (int16_t)avg;
				}
			}
		} else if (pulseaudio_to_obs_audio_format(audio_monitor->format) == AUDIO_FORMAT_U8BIT) {
			for (uint32_t frame = 0; frame < resample_frames; frame++) {
				uint64_t avg = 0;
				for (uint32_t channel = 0; channel < audio_monitor->channels; channel++) {
					avg += ((uint8_t *)resample_data[0])[frame * audio_monitor->channels + channel];
				}
				avg /= audio_monitor->channels;
				for (uint32_t channel = 0; channel < audio_monitor->channels; channel++) {
					((uint8_t *)resample_data[0])[frame * audio_monitor->channels + channel] = (uint8_t)avg;
				}
			}
		}
	}

	/* apply balance */
	float bal = (audio_monitor->balance + 1.0f) / 2.0f;
	if (!close_float(bal, 0.5f, EPSILON) && audio_monitor->channels > 1) {
		if (pulseaudio_to_obs_audio_format(audio_monitor->format) == AUDIO_FORMAT_FLOAT) {
			for (uint32_t frame = 0; frame < resample_frames; frame++) {
				((float *)resample_data[0])[frame * audio_monitor->channels + 0] =
					((float *)resample_data[0])[frame * audio_monitor->channels + 0] *
					sinf((1.0f - bal) * (M_PI / 2.0f));
				((float *)resample_data[0])[frame * audio_monitor->channels + 1] =
					((float *)resample_data[0])[frame * audio_monitor->channels + 1] *
					sinf(bal * (M_PI / 2.0f));
			}
		} else if (pulseaudio_to_obs_audio_format(audio_monitor->format) == AUDIO_FORMAT_32BIT) {
			for (uint32_t frame = 0; frame < resample_frames; frame++) {
				((int32_t *)resample_data[0])[frame * audio_monitor->channels + 0] =
					(int32_t)((float)((int32_t *)resample_data[0])[frame * audio_monitor->channels + 0] *
						  sinf((1.0f - bal) * (M_PI / 2.0f)));
				((int32_t *)resample_data[0])[frame * audio_monitor->channels + 1] =
					(int32_t)((float)((int32_t *)resample_data[0])[frame * audio_monitor->channels + 1] *
						  sinf(bal * (M_PI / 2.0f)));
			}
		} else if (pulseaudio_to_obs_audio_format(audio_monitor->format) == AUDIO_FORMAT_16BIT) {
			for (uint32_t frame = 0; frame < resample_frames; frame++) {
				((int16_t *)resample_data[0])[frame * audio_monitor->channels + 0] =
					(int16_t)((float)((int16_t *)resample_data[0])[frame * audio_monitor->channels + 0] *
						  sinf((1.0f - bal) * (M_PI / 2.0f)));
				((int16_t *)resample_data[0])[frame * audio_monitor->channels + 1] =
					(int16_t)((float)((int16_t *)resample_data[0])[frame * audio_monitor->channels + 1] *
						  sinf(bal * (M_PI / 2.0f)));
			}
		} else if (pulseaudio_to_obs_audio_format(audio_monitor->format) == AUDIO_FORMAT_U8BIT) {
			for (uint32_t frame = 0; frame < resample_frames; frame++) {
				((uint8_t *)resample_data[0])[frame * audio_monitor->channels + 0] =
					(uint8_t)((float)((uint8_t *)resample_data[0])[frame * audio_monitor->channels + 0] *
						  sinf((1.0f - bal) * (M_PI / 2.0f)));
				((uint8_t *)resample_data[0])[frame * audio_monitor->channels + 1] =
					(uint8_t)((float)((uint8_t *)resample_data[0])[frame * audio_monitor->channels + 1] *
						  sinf(bal * (M_PI / 2.0f)));
			}
		}
	}

	size_t bytes = audio_monitor->bytes_per_frame * resample_frames;

	circlebuf_push_back(&audio_monitor->new_data, resample_data[0], bytes);
	pthread_mutex_unlock(&audio_monitor->mutex);
	do_stream_write(data);
}

void audio_monitor_set_volume(struct audio_monitor *audio_monitor, float volume)
{
	if (!audio_monitor)
		return;
	audio_monitor->volume = volume;
}

void audio_monitor_set_mono(struct audio_monitor *audio_monitor, bool mono)
{
	if (!audio_monitor)
		return;
	audio_monitor->mono = mono;
}

void audio_monitor_set_balance(struct audio_monitor *audio_monitor, float balance)
{
	if (!audio_monitor)
		return;
	audio_monitor->balance = balance;
}

struct audio_monitor *audio_monitor_create(const char *device_id, const char *source_name, int port)
{
	UNUSED_PARAMETER(port);
	struct audio_monitor *audio_monitor = bzalloc(sizeof(struct audio_monitor));
	audio_monitor->device_id = bstrdup(device_id);
	audio_monitor->source_name = bstrdup(source_name);
	pthread_mutex_init(&audio_monitor->mutex, NULL);
	return audio_monitor;
}

void audio_monitor_destroy(struct audio_monitor *audio_monitor)
{
	if (!audio_monitor)
		return;
	audio_monitor_stop(audio_monitor);
	pthread_mutex_destroy(&audio_monitor->mutex);
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
	UNUSED_PARAMETER(audio_monitor);
	UNUSED_PARAMETER(format);
}

void audio_monitor_set_samples_per_sec(struct audio_monitor *audio_monitor, long long samples_per_sec)
{
	UNUSED_PARAMETER(audio_monitor);
	UNUSED_PARAMETER(samples_per_sec);
}
