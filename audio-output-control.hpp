#pragma once

#include <QCheckBox>
#include <qgridlayout.h>
#include <QLabel>
#include <QSlider>
#include <QWidget>
#include "volume-meter.hpp"

#include "obs.h"
#include "obs.hpp"
#include "audio-monitor-filter.h"

class AudioOutputControl : public QWidget {
	Q_OBJECT

private:
	const int lockRow = 0;
	const int sliderRow = 1;
	const int muteRow = 2;

	int track;
	VolumeMeter *volMeter;
	QGridLayout *mainLayout;
	QMap<QString, audio_monitor*> audioDevices;

	float prev_samples[MAX_AUDIO_CHANNELS][4];
	float magnitude[MAX_AUDIO_CHANNELS];
	float peak[MAX_AUDIO_CHANNELS];

	static void OBSOutputAudio(void *param, size_t mix_idx,
	                           struct audio_data *data);

	void addDeviceColumn(int column, QString device_id, QString deviceName, float volume = 100.0f, bool mute = false, bool lock = false);

private slots:
	void LockVolumeControl(bool lock);
	void MuteVolumeControl(bool mute);
	void SliderChanged(int vol);
signals:

public:
	explicit AudioOutputControl(int track, obs_data_t * settings = nullptr);
	~AudioOutputControl();

	obs_data_t *GetSettings();
	bool HasDevice(QString device_id);
	void AddDevice(QString device_id, QString device_name);
	void RemoveDevice(QString device_id);
	//void AddDevice(QString deviceId, QString deviceName);
	//void RemoveDevice(QString deviceId);
	//bool HasSliders();

};
