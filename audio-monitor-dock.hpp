#pragma once
#include <QDockWidget>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QStackedWidget>

#include "obs.h"
#include "audio-control.hpp"
#include "audio-monitor-filter.h"
#include <obs-frontend-api.h>

class AudioMonitorDock : public QStackedWidget {
	Q_OBJECT

private:
	QGridLayout *mainLayout;
	QMap<QString, QString> audioDevices;
	obs_hotkey_id resetHotkey = OBS_INVALID_HOTKEY_ID;

	void addAudioControl(obs_source_t *source, int column, obs_source_t *filter);
	void moveAudioControl(int fromColumn, int toColumn);
	void addFilter(int column, obs_source_t *filter);
	void addOutputTrack(int i, obs_data_t *obs_data = nullptr);
	QString GetTrackName(int i);
	static void OBSSignal(void *data, const char *signal, calldata_t *calldata);
	void RemoveAllSources();
	static void OBSFrontendEvent(enum obs_frontend_event event, void *data);

	static void OBSFilterAdd(void *data, calldata_t *calldata);
	static void OBSFilterRemove(void *data, calldata_t *calldata);
	static bool OBSAddAudioDevice(void *data, const char *name, const char *id);
	static bool OBSAddAudioSource(void *, obs_source_t *);
	static void OBSFilterAdd(obs_source_t *parent, obs_source_t *child, void *data);
	static void ResetHotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed);
	bool showOutputMeter;
	bool showOutputSlider;
	bool showOnlyActive;
	bool showSliderNames;
	void ConfigClicked();
	void RemoveSourcesWithoutSliders();
private slots:
	void MeterOutputChanged();
	void OutputSliderChanged();
	void OnlyActiveChanged();

	void SliderNamesChanged();
	void AddAudioSource(OBSSource source);
	void RemoveAudioControl(const QString &sourceName);
	void RenameAudioControl(QString new_name, QString prev_name);
	void AddFilter(OBSSource source, OBSSource filter);
	void RemoveFilter(QString sourceName, QString filterName);
	void LoadTrackMenu();
	void ShowOutputChanged();
	void OutputDeviceChanged();
	void UpdateTrackNames();

public:
	AudioMonitorDock(QWidget *parent = nullptr);
	~AudioMonitorDock();
};

class HScrollArea : public QScrollArea {
	Q_OBJECT

public:
	inline HScrollArea(QWidget *parent = nullptr) : QScrollArea(parent) { setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff); }

protected:
	virtual void resizeEvent(QResizeEvent *event) override;
};
