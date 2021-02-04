#pragma once
#include <QDockWidget>
#include <QHBoxLayout>
#include <QScrollArea>

#include "obs.h"
#include "audio-control.hpp"
#include "audio-monitor-filter.h"

class AudioMonitorDock : public QDockWidget {
	Q_OBJECT

private:
	QGridLayout *mainLayout;
	QMap<QString, QString> audioDevices;

	void addAudioControl(obs_source_t *source, int column,
	                     obs_source_t *filter);
	void moveAudioControl(int fromColumn, int toColumn);
	void addFilter(int column, obs_source_t *filter);
	void addOutputTrack(int i, obs_data_t *obs_data = nullptr);
	static void OBSSignal(void *data, const char *signal,
			      calldata_t *calldata);

	static void OBSFilterAdd(void *data, calldata_t *calldata);
	static void OBSFilterRemove(void *data, calldata_t *calldata);
	static bool OBSAddAudioDevice(void *data, const char *name,
				      const char *id);
	static bool OBSAddAudioSource(void *, obs_source_t *);
	static void OBSFilterAdd(obs_source_t *parent, obs_source_t *child,
				 void *data);
	bool showOutputMeter;
	bool showOutputSlider;
	bool showOnlyActive;
	void ConfigClicked();
private slots:
	void MeterOutputChanged();
	void OutputSliderChanged();
	void OnlyActiveChanged();
	void AddAudioSource(OBSSource source);
	void RemoveAudioControl(const QString &sourceName);
	void RenameAudioControl(QString new_name, QString prev_name);
	void AddFilter(OBSSource source, OBSSource filter);
	void RemoveFilter(QString sourceName, QString filterName);
	void LoadTrackMenu();
	void ShowOutputChanged();
	void OutputDeviceChanged();

public:
	AudioMonitorDock(QWidget *parent = nullptr);
	~AudioMonitorDock();
};

class HScrollArea : public QScrollArea {
	Q_OBJECT

public:
	inline HScrollArea(QWidget *parent = nullptr) : QScrollArea(parent)
	{
		setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	}

protected:
	virtual void resizeEvent(QResizeEvent *event) override;
};
