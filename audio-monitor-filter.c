#include "audio-monitor-filter.h"

#include "obs-frontend-api.h"
#include "obs-module.h"
#include "obs.h"
#include "version.h"
#include "util/circlebuf.h"

struct audio_monitor_context {
	obs_source_t *source;
	struct audio_monitor *monitor;
	long long delay;
	struct circlebuf audio_buffer;
};

static const char *audio_monitor_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("AudioMonitor");
}
struct updateFilterNameData {

	const char *device_id;
	const char *device_name;
};

bool updateFilterName(void *data, const char *name, const char *id)
{
	struct updateFilterNameData *d = data;
	if (strcmp(id, d->device_id) == 0) {
		d->device_name = name;
		return false;
	}
	return true;
}

#define LOG_OFFSET_DB 6.0f
#define LOG_RANGE_DB 96.0f

static void audio_monitor_update(void *data, obs_data_t *settings)
{
	struct audio_monitor_context *audio_monitor = data;

	audio_monitor->delay = obs_data_get_int(settings, "delay");
	int port = 0;
	char *device_id = obs_data_get_string(settings, "device");
	if (strcmp(device_id, "VBAN") == 0) {
		device_id = obs_data_get_string(settings, "ip");
		port = obs_data_get_int(settings, "port");
	}
	if (!audio_monitor->monitor ||
	    strcmp(audio_monitor_get_device_id(audio_monitor->monitor),
		   device_id) != 0) {
		if (!port) {
			struct updateFilterNameData d;
			d.device_id = device_id;
			d.device_name = NULL;
			obs_enum_audio_monitoring_devices(updateFilterName, &d);
			if (d.device_name) {
				obs_data_set_string(settings, "deviceName",
						    d.device_name);
			}
		} else {
			obs_data_set_string(settings, "deviceName", device_id);
		}
		struct audio_monitor *old = audio_monitor->monitor;
		audio_monitor->monitor = NULL;
		audio_monitor_destroy(old);
		audio_monitor->monitor = audio_monitor_create(
			device_id, obs_source_get_name(audio_monitor->source),
			port);
		if (port) {
			audio_monitor_set_format(audio_monitor->monitor,
						 obs_data_get_int(settings,
								  "format"));
			audio_monitor_set_samples_per_sec(
				audio_monitor->monitor,
				obs_data_get_int(settings, "samples_per_sec"));
		}
		audio_monitor_start(audio_monitor->monitor);
	} else if (port) {
		audio_monitor_set_format(audio_monitor->monitor,
					 obs_data_get_int(settings, "format"));
		audio_monitor_set_samples_per_sec(
			audio_monitor->monitor,
			obs_data_get_int(settings, "samples_per_sec"));
	}
	float def = (float)obs_data_get_double(settings, "volume") / 100.0f;
	float db;
	if (def >= 1.0f)
		db = 0.0f;
	else if (def <= 0.0f)
		db = -INFINITY;
	else
		db = -(LOG_RANGE_DB + LOG_OFFSET_DB) *
			     powf((LOG_RANGE_DB + LOG_OFFSET_DB) /
					  LOG_OFFSET_DB,
				  -def) +
		     LOG_OFFSET_DB;
	const float mul = isfinite((double)db) ? powf(10.0f, db / 20.0f) : 0.0f;

	audio_monitor_set_volume(audio_monitor->monitor, mul);

	struct calldata cd;
	uint8_t stack[128];
	calldata_init_fixed(&cd, stack, sizeof(stack));
	calldata_set_ptr(&cd, "source", audio_monitor->source);
	signal_handler_signal(
		obs_source_get_signal_handler(audio_monitor->source), "updated",
		&cd);
}

static void *audio_monitor_filter_create(obs_data_t *settings,
					 obs_source_t *source)
{
	struct audio_monitor_context *audio_monitor =
		bzalloc(sizeof(struct audio_monitor_context));
	audio_monitor->source = source;
	signal_handler_add(obs_source_get_signal_handler(source),
			   "void updated(ptr source)");
	audio_monitor_update(audio_monitor, settings);
	return audio_monitor;
}

static void audio_monitor_filter_destroy(void *data)
{
	struct audio_monitor_context *audio_monitor = data;
	audio_monitor_destroy(audio_monitor->monitor);
	while (audio_monitor->audio_buffer.size) {
		struct obs_audio_data cached;
		circlebuf_pop_front(&audio_monitor->audio_buffer, &cached,
				    sizeof(cached));
		for (size_t i = 0; i < MAX_AV_PLANES; i++)
			bfree(cached.data[i]);
	}
	circlebuf_free(&audio_monitor->audio_buffer);
	bfree(audio_monitor);
}

struct obs_audio_data *audio_monitor_filter_audio(void *data,
						  struct obs_audio_data *audio)
{
	struct audio_monitor_context *audio_monitor = data;
	if (audio_monitor->delay) {
		struct obs_audio_data cached = *audio;
		for (size_t i = 0; i < MAX_AV_PLANES; i++) {
			if (!audio->data[i])
				break;

			cached.data[i] = bmemdup(audio->data[i],
						 audio->frames * sizeof(float));
		}
		circlebuf_push_back(&audio_monitor->audio_buffer, &cached,
				    sizeof(cached));
		circlebuf_peek_front(&audio_monitor->audio_buffer, &cached,
				     sizeof(cached));
		uint64_t diff = cached.timestamp > audio->timestamp
					? cached.timestamp - audio->timestamp
					: audio->timestamp - cached.timestamp;
		while (audio_monitor->audio_buffer.size > sizeof(cached) &&
		       diff >= audio_monitor->delay * 1000000) {

			circlebuf_pop_front(&audio_monitor->audio_buffer, NULL,
					    sizeof(cached));

			audio_monitor_audio(audio_monitor->monitor, &cached);

			for (size_t i = 0; i < MAX_AV_PLANES; i++)
				bfree(cached.data[i]);

			circlebuf_peek_front(&audio_monitor->audio_buffer,
					     &cached, sizeof(cached));
			diff = cached.timestamp > audio->timestamp
				       ? cached.timestamp - audio->timestamp
				       : audio->timestamp - cached.timestamp;
		}
	} else {
		if (audio_monitor->monitor)
			audio_monitor_audio(audio_monitor->monitor, audio);
		while (audio_monitor->audio_buffer.size) {
			struct obs_audio_data cached;
			circlebuf_pop_front(&audio_monitor->audio_buffer,
					    &cached, sizeof(cached));
			for (size_t i = 0; i < MAX_AV_PLANES; i++)
				bfree(cached.data[i]);
		}
	}
	return audio;
}

static bool add_monitoring_device(void *data, const char *name, const char *id)
{
	obs_property_list_add_string(data, name, id);
	return true;
}

bool audio_monitor_device_changed(obs_properties_t *props,
				  obs_property_t *property,
				  obs_data_t *settings)
{
	auto *ip = obs_properties_get(props, "ip");
	auto *port = obs_properties_get(props, "port");
	auto *format = obs_properties_get(props, "format");
	auto *samples_per_sec = obs_properties_get(props, "samples_per_sec");
	if (strcmp("VBAN", obs_data_get_string(settings, "device")) == 0) {
		obs_property_set_visible(ip, true);
		obs_property_set_visible(port, true);
		obs_property_set_visible(format, true);
		obs_property_set_visible(samples_per_sec, true);
	} else {
		obs_property_set_visible(ip, false);
		obs_property_set_visible(port, false);
		obs_property_set_visible(format, false);
		obs_property_set_visible(samples_per_sec, false);
	}
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
#ifdef WIN32
	obs_property_list_add_string(p, obs_module_text("VBAN"), "VBAN");
#endif
	obs_enum_audio_monitoring_devices(add_monitoring_device, p);
	obs_property_set_modified_callback(p, audio_monitor_device_changed);
	p = obs_properties_add_float_slider(
		ppts, "volume", obs_module_text("Volume"), 0.0, 100.0, 1.0);

	obs_property_float_set_suffix(p, "%");
	obs_properties_add_bool(ppts, "locked", obs_module_text("Locked"));

	p = obs_properties_add_int(ppts, "delay", obs_module_text("Delay"), 0,
				   10000, 100);
	obs_property_int_set_suffix(p, "ms");
	obs_properties_add_text(ppts, "ip", obs_module_text("Ip"),
				OBS_TEXT_DEFAULT);
	obs_properties_add_int(ppts, "port", obs_module_text("Port"), 1, 32767,
			       1);
	p = obs_properties_add_list(ppts, "format", obs_module_text("Format"),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("UInt8"),
				  AUDIO_FORMAT_U8BIT);
	obs_property_list_add_int(p, obs_module_text("Int16"),
				  AUDIO_FORMAT_16BIT);
	obs_property_list_add_int(p, obs_module_text("Int32"),
				  AUDIO_FORMAT_32BIT);
	obs_property_list_add_int(p, obs_module_text("Float32"),
				  AUDIO_FORMAT_FLOAT);

	p = obs_properties_add_list(ppts, "samples_per_sec",
				    obs_module_text("SampleRate"),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("6kHz"), 6000);
	obs_property_list_add_int(p, obs_module_text("8kHz"), 8000);
	obs_property_list_add_int(p, obs_module_text("11025Hz"), 11025);
	obs_property_list_add_int(p, obs_module_text("12kHz"), 12000);
	obs_property_list_add_int(p, obs_module_text("16kHz"), 16000);
	obs_property_list_add_int(p, obs_module_text("22050Hz"), 22050);
	obs_property_list_add_int(p, obs_module_text("24kHz"), 24000);
	obs_property_list_add_int(p, obs_module_text("32kHz"), 32000);
	obs_property_list_add_int(p, obs_module_text("44.1kHz"), 44100);
	obs_property_list_add_int(p, obs_module_text("48kHz"), 48000);
	obs_property_list_add_int(p, obs_module_text("64kHz"), 64000);
	obs_property_list_add_int(p, obs_module_text("88.2kHz"), 88200);
	obs_property_list_add_int(p, obs_module_text("96kHz"), 96000);
	obs_property_list_add_int(p, obs_module_text("128kHz"), 128000);
	obs_property_list_add_int(p, obs_module_text("176.4kHz"), 176400);
	obs_property_list_add_int(p, obs_module_text("192kHz"), 192000);
	obs_property_list_add_int(p, obs_module_text("256kHz"), 256000);
	obs_property_list_add_int(p, obs_module_text("352.8kHz"), 352800);
	obs_property_list_add_int(p, obs_module_text("384kHz"), 384000);
	obs_property_list_add_int(p, obs_module_text("512kHz"), 512000);
	obs_property_list_add_int(p, obs_module_text("705.6kHz"), 705600);
	return ppts;
}

void audio_monitor_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, "volume", 100.0);
	obs_data_set_default_string(settings, "device", "default");
	obs_data_set_default_int(settings, "port", 6980);
	obs_data_set_default_int(settings, "format", AUDIO_FORMAT_FLOAT);
	obs_data_set_default_int(
		settings, "samples_per_sec",
		audio_output_get_info(obs_get_audio())->samples_per_sec);
}

struct obs_source_info audio_monitor_filter_info = {
	.id = "audio_monitor",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_AUDIO,
	.get_name = audio_monitor_get_name,
	.create = audio_monitor_filter_create,
	.destroy = audio_monitor_filter_destroy,
	.update = audio_monitor_update,
	.load = audio_monitor_update,
	.get_defaults = audio_monitor_defaults,
	.get_properties = audio_monitor_properties,
	.filter_audio = audio_monitor_filter_audio,
};

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("Exeldro");
OBS_MODULE_USE_DEFAULT_LOCALE("media-controls", "en-US")

extern void load_audio_monitor_dock();
bool obs_module_load()
{
	blog(LOG_INFO, "[Audio Monitor] loaded version %s", PROJECT_VERSION);
	obs_register_source(&audio_monitor_filter_info);
	load_audio_monitor_dock();
	return true;
}

void obs_module_unload() {}

MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("Description");
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return obs_module_text("AudioMonitor");
}
