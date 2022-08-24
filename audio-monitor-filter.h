#pragma once
#include "obs.h"
#ifdef __cplusplus
extern "C" {
#endif
struct audio_monitor;
void audio_monitor_stop(struct audio_monitor *audio_monitor);
void audio_monitor_start(struct audio_monitor *audio_monitor);
void audio_monitor_audio(void *data, struct obs_audio_data *audio);
void audio_monitor_set_volume(struct audio_monitor *audio_monitor,
			      float volume);
void audio_monitor_set_format(struct audio_monitor *audio_monitor,
			      enum audio_format format);
void audio_monitor_set_samples_per_sec(struct audio_monitor *audio_monitor,
				       long long samples_per_sec);
struct audio_monitor *audio_monitor_create(const char *device_id,
					   const char *source_name, int port);
void audio_monitor_destroy(struct audio_monitor *audio_monitor);
const char *audio_monitor_get_device_id(struct audio_monitor *audio_monitor);

#ifdef __cplusplus
}
#endif
