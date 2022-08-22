#include "audio-monitor-filter.h"

#include "obs-frontend-api.h"
#include "obs-module.h"
#include "obs.h"
#include "version.h"
#include "util/circlebuf.h"

#define MUTE_NEVER 0
#define MUTE_NOT_ACTIVE 1
#define MUTE_SOURCE_MUTE 2

struct audio_monitor_context {
	obs_source_t *source;
	struct audio_monitor *monitor;
	long long delay;
	struct circlebuf audio_buffer;
	bool linked;
	bool updating_volume;
	int mute;
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
/* equals -log10f(LOG_OFFSET_DB) */
#define LOG_OFFSET_VAL -0.77815125038364363f
/* equals -log10f(-LOG_RANGE_DB + LOG_OFFSET_DB) */
#define LOG_RANGE_VAL -2.00860017176191756f

void audio_monitor_volume_changed(void *data, calldata_t *call_data)
{
	struct audio_monitor_context *audio_monitor = data;
	if (audio_monitor->updating_volume || !audio_monitor->source ||
	    obs_source_removed(audio_monitor->source))
		return;
	double mul = calldata_float(call_data, "volume");
	float db = obs_mul_to_db((float)mul);
	float def;
	if (db >= 0.0f)
		def = 1.0f;
	else if (db <= -96.0f)
		def = 0.0f;
	else
		def = (-log10f(-db + LOG_OFFSET_DB) - LOG_RANGE_VAL) /
		      (LOG_OFFSET_VAL - LOG_RANGE_VAL);
	obs_data_t *settings = obs_source_get_settings(audio_monitor->source);
	if (settings) {
		float def2 =
			(float)obs_data_get_double(settings, "volume") / 100.0f;
		float db2;
		if (def2 >= 1.0f)
			db2 = 0.0f;
		else if (def2 <= 0.0f)
			db2 = -INFINITY;
		else
			db2 = -(LOG_RANGE_DB + LOG_OFFSET_DB) *
				      powf((LOG_RANGE_DB + LOG_OFFSET_DB) /
						   LOG_OFFSET_DB,
					   -def2) +
			      LOG_OFFSET_DB;
		if (!close_float(db, db2, 0.01f)) {
			obs_data_set_double(settings, "volume", def * 100.f);
			audio_monitor->updating_volume = true;
			obs_source_update(audio_monitor->source, NULL);
			audio_monitor->updating_volume = false;
		}
		obs_data_release(settings);
	}
}

void audio_monitor_mute_changed(void *data, calldata_t *call_data)
{
	struct audio_monitor_context *audio_monitor = data;
	const bool muted = calldata_bool(call_data, "muted");
	if (muted == obs_source_enabled(audio_monitor->source)) {
		obs_source_set_enabled(audio_monitor->source, !muted);
	}
}

void audio_monitor_enabled_changed(void *data, calldata_t *call_data)
{
	struct audio_monitor_context *audio_monitor = data;
	const bool enabled = calldata_bool(call_data, "enabled");
	obs_source_t *parent = obs_filter_get_parent(audio_monitor->source);
	if (parent && obs_source_muted(parent) == enabled) {
		obs_source_set_muted(parent, !enabled);
	}
}

void audio_monitor_activated(void *data, calldata_t *call_data)
{
	UNUSED_PARAMETER(call_data);
	struct audio_monitor_context *audio_monitor = data;
	if (!obs_source_enabled(audio_monitor->source))
		obs_source_set_enabled(audio_monitor->source, true);
}

void audio_monitor_deactivated(void *data, calldata_t *call_data)
{
	UNUSED_PARAMETER(call_data);
	struct audio_monitor_context *audio_monitor = data;
	if (obs_source_enabled(audio_monitor->source))
		obs_source_set_enabled(audio_monitor->source, false);
}

static void audio_monitor_update(void *data, obs_data_t *settings)
{
	struct audio_monitor_context *audio_monitor = data;

	audio_monitor->delay = obs_data_get_int(settings, "delay");
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
	const float mul = obs_db_to_mul(db);
	obs_source_t *parent = obs_filter_get_parent(audio_monitor->source);
	if (parent) {
		if (obs_data_get_bool(settings, "linked")) {
			const float vol = obs_source_get_volume(parent);
			float db2 = obs_mul_to_db(vol);
			if (!audio_monitor->updating_volume &&
			    !close_float(db, db2, 0.01f)) {
				audio_monitor->updating_volume = true;
				obs_source_set_volume(parent, mul);
				audio_monitor->updating_volume = false;
			}
			if (!audio_monitor->linked) {
				signal_handler_t *sh =
					obs_source_get_signal_handler(parent);
				if (sh) {
					signal_handler_connect(
						sh, "volume",
						audio_monitor_volume_changed,
						audio_monitor);
					audio_monitor->linked = true;
				}
			}

		} else if (audio_monitor->linked) {
			signal_handler_t *sh =
				obs_source_get_signal_handler(parent);
			if (sh) {
				signal_handler_disconnect(
					sh, "volume",
					audio_monitor_volume_changed,
					audio_monitor);
				audio_monitor->linked = false;
			}
		}
		const int mute = (int)obs_data_get_int(settings, "mute");
		if (audio_monitor->mute == MUTE_SOURCE_MUTE &&
		    mute != audio_monitor->mute) {
			signal_handler_t *sh =
				obs_source_get_signal_handler(parent);
			if (sh) {
				signal_handler_disconnect(
					sh, "mute", audio_monitor_mute_changed,
					audio_monitor);
				audio_monitor->mute = MUTE_NEVER;
			}
		}
		if (audio_monitor->mute == MUTE_NOT_ACTIVE &&
		    mute != audio_monitor->mute) {
			signal_handler_t *sh =
				obs_source_get_signal_handler(parent);
			if (sh) {
				signal_handler_disconnect(
					sh, "activate", audio_monitor_activated,
					audio_monitor);
				signal_handler_disconnect(
					sh, "deactivated",
					audio_monitor_deactivated,
					audio_monitor);
				audio_monitor->mute = MUTE_NEVER;
			}
		}
		if (mute == MUTE_SOURCE_MUTE) {
			const bool muted = obs_source_muted(parent);
			if (muted ==
			    obs_source_enabled(audio_monitor->source)) {
				obs_source_set_muted(parent, !muted);
			}
			if (audio_monitor->mute != MUTE_SOURCE_MUTE) {
				signal_handler_t *sh =
					obs_source_get_signal_handler(parent);
				if (sh) {
					signal_handler_connect(
						sh, "mute",
						audio_monitor_mute_changed,
						audio_monitor);
					audio_monitor->mute = MUTE_SOURCE_MUTE;
				}
				signal_handler_connect(
					obs_source_get_signal_handler(
						audio_monitor->source),
					"enable", audio_monitor_enabled_changed,
					audio_monitor);
			}
		}
		if (mute == MUTE_NOT_ACTIVE) {
			const bool active = obs_source_active(parent);
			if (active !=
			    obs_source_enabled(audio_monitor->source)) {
				obs_source_set_enabled(audio_monitor->source,
						       active);
			}
			if (audio_monitor->mute != MUTE_NOT_ACTIVE) {
				signal_handler_t *sh =
					obs_source_get_signal_handler(parent);
				if (sh) {
					signal_handler_connect(
						sh, "activate",
						audio_monitor_activated,
						audio_monitor);
					signal_handler_connect(
						sh, "deactivate",
						audio_monitor_deactivated,
						audio_monitor);
					audio_monitor->mute = MUTE_NOT_ACTIVE;
				}
			}
		}
	}

	int port = 0;
	char *device_id = (char *)obs_data_get_string(settings, "device");
	if (strcmp(device_id, "VBAN") == 0) {
		device_id = (char *)obs_data_get_string(settings, "ip");
		port = (int)obs_data_get_int(settings, "port");
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
	obs_source_t *parent = obs_filter_get_parent(audio_monitor->source);
	if (parent) {
		signal_handler_t *sh = obs_source_get_signal_handler(parent);
		signal_handler_disconnect(sh, "volume",
					  audio_monitor_volume_changed,
					  audio_monitor);
		signal_handler_disconnect(
			sh, "mute", audio_monitor_mute_changed, audio_monitor);
		signal_handler_disconnect(
			sh, "activate", audio_monitor_activated, audio_monitor);
		signal_handler_disconnect(sh, "deactivated",
					  audio_monitor_deactivated,
					  audio_monitor);
	}
	audio_monitor->source = NULL;
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
		       diff >= (uint64_t)audio_monitor->delay * 1000000) {

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
	UNUSED_PARAMETER(property);
	obs_property_t *ip = obs_properties_get(props, "ip");
	obs_property_t *port = obs_properties_get(props, "port");
	obs_property_t *format = obs_properties_get(props, "format");
	obs_property_t *samples_per_sec =
		obs_properties_get(props, "samples_per_sec");
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
	struct audio_monitor_context *audio_monitor = data;
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
	obs_data_t *settings = obs_source_get_settings(audio_monitor->source);
	if (settings) {
		const char *device_id = obs_data_get_string(settings, "device");
		if (device_id && strlen(device_id)) {
			const size_t count = obs_property_list_item_count(p);
			bool found = false;
			for (size_t i = 0; i < count; i++) {
				if (strcmp(device_id,
					   obs_property_list_item_string(
						   p, i)) == 0) {
					found = true;
					break;
				}
			}
			if (!found) {
				const char *device_name = obs_data_get_string(
					settings, "deviceName");
				if (device_name && strlen(device_name)) {
					obs_property_list_add_string(
						p, device_name, device_id);
				} else {
					obs_property_list_add_string(
						p, device_id, device_id);
				}
			}
		}

		obs_data_release(settings);
	}

	obs_property_set_modified_callback(p, audio_monitor_device_changed);
	p = obs_properties_add_float_slider(
		ppts, "volume", obs_module_text("Volume"), 0.0, 100.0, 1.0);

	obs_property_float_set_suffix(p, "%");
	obs_properties_add_bool(ppts, "locked", obs_module_text("Locked"));
	obs_properties_add_bool(ppts, "linked", obs_module_text("Linked"));
	p = obs_properties_add_list(ppts, "mute", obs_module_text("Mute"),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Never"), MUTE_NEVER);
	obs_property_list_add_int(p, obs_module_text("NotActiveOutput"),
				  MUTE_NOT_ACTIVE);
	obs_property_list_add_int(p, obs_module_text("SourceMuted"),
				  MUTE_SOURCE_MUTE);

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

	obs_properties_t *custom_color = obs_properties_create();
	obs_properties_add_color(custom_color, "color",
				 obs_module_text("Color"));
	obs_properties_add_group(ppts, "custom_color",
				 obs_module_text("CustomColor"),
				 OBS_GROUP_CHECKABLE, custom_color);
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
