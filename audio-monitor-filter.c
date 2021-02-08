#include "audio-monitor-filter.h"

#include "obs-frontend-api.h"
#include "obs-module.h"
#include "obs.h"
#include "version.h"

struct audio_monitor_context {
	obs_source_t *source;
	struct audio_monitor *monitor;
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

	const char *device_id = obs_data_get_string(settings, "device");

	if (!audio_monitor->monitor ||
	    strcmp(audio_monitor_get_device_id(audio_monitor->monitor),
		   device_id) != 0) {
		struct updateFilterNameData d;
		d.device_id = device_id;
		d.device_name = NULL;
		obs_enum_audio_monitoring_devices(updateFilterName, &d);
		if (d.device_name) {
			obs_data_set_string(settings, "deviceName",
					    d.device_name);
		}
		audio_monitor_destroy(audio_monitor->monitor);
		audio_monitor->monitor = audio_monitor_create(device_id);
		audio_monitor_start(audio_monitor->monitor);
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
	bfree(audio_monitor);
}

struct obs_audio_data *audio_monitor_filter_audio(void *data,
						  struct obs_audio_data *audio)
{
	struct audio_monitor_context *audio_monitor = data;
	audio_monitor_audio(audio_monitor->monitor, audio);
	return audio;
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
	obs_properties_add_bool(ppts, "locked", obs_module_text("Locked"));
	return ppts;
}

void audio_monitor_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, "volume", 100.0);
	obs_data_set_default_string(settings, "device", "default");
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
