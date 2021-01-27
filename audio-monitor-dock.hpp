#pragma once
#include <QDockWidget>
#include <QHBoxLayout>
#include <QScrollArea>

#include "obs.h"
#include "audio-control.hpp"

class AudioMonitorDock : public QDockWidget {
	Q_OBJECT

private:
	QGridLayout *mainLayout;
	QMap<QString, QString> audioDevices;

	void addAudioControl(obs_source_t *source, int column,
			     obs_source_t *filter);
	void moveAudioControl(int fromColumn, int toColumn);
	void addFilter(int column, obs_source_t *filter);
	static void OBSSignal(void *data, const char *signal,
			      calldata_t *calldata);

	static void OBSFilterAdd(void *data, calldata_t *calldata);
	static void OBSFilterRemove(void *data, calldata_t *calldata);
	static bool OBSAddAudioDevice(void *data, const char *name,
				      const char *id);
	bool showOutputMeter;
	bool showOutputSlider;
	void ConfigClicked();
private slots:
	void MeterOutputChanged();
	void OutputSliderChanged();

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
