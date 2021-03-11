#include "audio-monitor-pulse.h"
#include <obs.h>
#include <util/threading.h>
#include <util/platform.h>
#include <media-io/audio-resampler.h>
#include <inttypes.h>
#include <pulse/stream.h>
#include <pulse/context.h>
#include <pulse/introspect.h>
#include <pulse/thread-mainloop.h>

static uint_fast32_t pulseaudio_refs = 0;
static pthread_mutex_t pulseaudio_mutex = PTHREAD_MUTEX_INITIALIZER;
static pa_threaded_mainloop *pulseaudio_mainloop = NULL;
static pa_context *pulseaudio_context = NULL;

struct audio_monitor {
	pa_stream *stream;
	pa_buffer_attr attr;
	pa_sample_format_t format;
	uint_fast32_t samples_per_sec;
	size_t bytesRemaining;

	uint32_t channels;
	audio_resampler_t *resampler;
	float volume;
	pthread_mutex_t mutex;
	char *device_id;
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

void pulseaudio_accept()
{
	pa_threaded_mainloop_accept(pulseaudio_mainloop);
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
	pulseaudio_context = pa_context_new_with_proplist(
		pa_threaded_mainloop_get_api(pulseaudio_mainloop),
		"OBS-Monitor", p);

	pa_context_set_state_callback(pulseaudio_context,
				      pulseaudio_context_state_changed, NULL);

	pa_context_connect(pulseaudio_context, NULL, PA_CONTEXT_NOAUTOSPAWN,
			   NULL);
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

	pa_operation *op =
		pa_context_get_server_info(pulseaudio_context, cb, userdata);
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

static void pulseaudio_default_devices(pa_context *c, const pa_server_info *i,
				       void *userdata)
{
	UNUSED_PARAMETER(c);
	struct pulseaudio_default_output *d =
		(struct pulseaudio_default_output *)userdata;
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
	struct pulseaudio_default_output *pdo =
		bzalloc(sizeof(struct pulseaudio_default_output));
	pulseaudio_get_server_info(
		(pa_server_info_cb_t)pulseaudio_default_devices, (void *)pdo);

	if (!pdo->default_sink_name || !*pdo->default_sink_name) {
		*id = NULL;
	} else {
		*id = bzalloc(strlen(pdo->default_sink_name) + 9);
		strcat(*id, pdo->default_sink_name);
		strcat(*id, ".monitor");
		bfree(pdo->default_sink_name);
	}

	bfree(pdo);
	pulseaudio_unref();
}

static void pulseaudio_server_info(pa_context *c, const pa_server_info *i,
				   void *userdata)
{
	UNUSED_PARAMETER(c);
	UNUSED_PARAMETER(userdata);

	blog(LOG_INFO, "Server name: '%s %s'", i->server_name,
	     i->server_version);

	pulseaudio_signal(0);
}



int_fast32_t pulseaudio_get_source_info(pa_source_info_cb_t cb,
					const char *name, void *userdata)
{
	if (pulseaudio_context_ready() < 0)
		return -1;

	pulseaudio_lock();

	pa_operation *op = pa_context_get_source_info_by_name(
		pulseaudio_context, name, cb, userdata);
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

static enum audio_format
pulseaudio_to_obs_audio_format(pa_sample_format_t format)
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

static enum speaker_layout
pulseaudio_channels_to_obs_speakers(uint_fast32_t channels)
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

static void pulseaudio_source_info(pa_context *c, const pa_source_info *i,
				   int eol, void *userdata)
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

	blog(LOG_INFO, "Audio format: %s, %" PRIu32 " Hz, %" PRIu8 " channels",
	     pa_sample_format_to_string(i->sample_spec.format),
	     i->sample_spec.rate, i->sample_spec.channels);

	pa_sample_format_t format = i->sample_spec.format;
	if (pulseaudio_to_obs_audio_format(format) == AUDIO_FORMAT_UNKNOWN) {
		format = PA_SAMPLE_FLOAT32LE;

		blog(LOG_INFO,
		     "Sample format %s not supported by OBS,"
		     "using %s instead for recording",
		     pa_sample_format_to_string(i->sample_spec.format),
		     pa_sample_format_to_string(format));
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

void pulseaudio_write_callback(pa_stream *p, pa_stream_request_cb_t cb,
			       void *userdata)
{
	if (pulseaudio_context_ready() < 0)
		return;

	pulseaudio_lock();
	pa_stream_set_write_callback(p, cb, userdata);
	pulseaudio_unlock();
}

void pulseaudio_set_underflow_callback(pa_stream *p, pa_stream_notify_cb_t cb,
				       void *userdata)
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
	//if (obs_source_active(data->source))
		data->attr.tlength = (data->attr.tlength * 3) / 2;

	pa_stream_set_buffer_attr(data->stream, &data->attr, NULL, NULL);
	pthread_mutex_unlock(&data->mutex);

	pulseaudio_signal(0);
}

void audio_monitor_stop(struct audio_monitor *audio_monitor){
    audio_resampler_destroy(audio_monitor->resampler);
	audio_monitor->resampler = NULL;
}

void audio_monitor_start(struct audio_monitor *audio_monitor){
    const struct audio_output_info *info =
		audio_output_get_info(obs_get_audio());
        	pulseaudio_init();
	char *device = NULL;
	if (strcmp(audio_monitor->device_id, "default") == 0) {
		get_default_id(&device);
	}else {
		device = bstrdup(audio_monitor->device_id);
	}
	if (!device)
		return;
	if (pulseaudio_get_server_info(pulseaudio_server_info,
				       (void *)audio_monitor) < 0) {
		blog(LOG_ERROR, "Unable to get server info !");
		bfree(device);
		return;
	}
	if (pulseaudio_get_source_info(pulseaudio_source_info,
				       device,
				       (void *)audio_monitor) < 0) {
		blog(LOG_ERROR, "Unable to get source info !");
		bfree(device);
		return;
	}
	bfree(device);

	pulseaudio_write_callback(audio_monitor->stream,
				  pulseaudio_stream_write,
				  (void *)audio_monitor);

	pulseaudio_set_underflow_callback(audio_monitor->stream,
					  pulseaudio_underflow,
					  (void *)audio_monitor);
}

void audio_monitor_audio(void *data, struct obs_audio_data *audio){
    struct audio_monitor *audio_monitor = data;
	if (!audio_monitor->resampler && audio_monitor->device_id &&
	    strlen(audio_monitor->device_id) &&
	    pthread_mutex_trylock(&audio_monitor->mutex) == 0) {
		audio_monitor_start(audio_monitor);
		pthread_mutex_unlock(&audio_monitor->mutex);
	}
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
    // todo pulse
	pthread_mutex_unlock(&audio_monitor->mutex);
}

void audio_monitor_set_volume(struct audio_monitor *audio_monitor,
			      float volume){
    audio_monitor->volume = volume;
}

struct audio_monitor *audio_monitor_create(const char *device_id){
	struct audio_monitor *audio_monitor =
		bzalloc(sizeof(struct audio_monitor));
	audio_monitor->device_id = bstrdup(device_id);
	pthread_mutex_init(&audio_monitor->mutex, NULL);
	return audio_monitor;
}

void audio_monitor_destroy(struct audio_monitor *audio_monitor){
	if (!audio_monitor)
		return;
	audio_monitor_stop(audio_monitor);
	bfree(audio_monitor->device_id);
	bfree(audio_monitor);
}

const char *audio_monitor_get_device_id(struct audio_monitor *audio_monitor){
    return audio_monitor->device_id;
}