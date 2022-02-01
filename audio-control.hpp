#pragma once

#include <QCheckBox>
#include <qgridlayout.h>
#include <QLabel>
#include <QSlider>
#include <QWidget>
#include "volume-meter.hpp"

#include "obs.h"
#include "obs.hpp"

class AudioControl : public QWidget {
	Q_OBJECT

private:
	const int lockRow = 0;
	const int sliderRow = 1;
	const int muteRow = 2;
	const int nameRow = 3;

	OBSWeakSource source;
	VolumeMeter *volMeter;
	obs_volmeter_t *obs_volmeter;
	QGridLayout *mainLayout;
	bool showSliderNames;

	static void OBSVolumeLevel(void *data,
				   const float magnitude[MAX_AUDIO_CHANNELS],
				   const float peak[MAX_AUDIO_CHANNELS],
				   const float inputPeak[MAX_AUDIO_CHANNELS]);
	static void OBSVolume(void *data, calldata_t *calldata);
	static void OBSMute(void *data, calldata_t *calldata);
	static void OBSFilterRename(void *data, calldata_t *calldata);
	static void OBSFilterUpdated(void *data, calldata_t *calldata);
	static void OBSFilterEnable(void *data, calldata_t *calldata);

	void addFilterColumn(int i, obs_source_t *filter);

private slots:
	void LockVolumeControl(bool lock);
	void MuteVolumeControl(bool mute);
	void SliderChanged(int vol);
	void SetOutputVolume(double volume);
	void SetMute(bool muted);
	void RenameFilter(QString prev_name, QString new_name);
	void FilterUpdated(QString name, double volume, bool locked, bool custom_color, QColor color);
	void FilterEnable(QString name, bool enabled);
signals:

public:
	explicit AudioControl(OBSWeakSource source);
	~AudioControl();

	inline obs_weak_source *GetSource() const { return source; }

	void AddFilter(obs_source_t *filter);
	void RemoveFilter(QString filterName);
	bool HasSliders();
	void ShowOutputMeter(bool output);
	void ShowOutputSlider(bool output);
	void ShowSliderNames(bool show);

	//void SetMeterDecayRate(qreal q);
	//void setPeakMeterType(enum obs_peak_meter_type peakMeterType);
};
