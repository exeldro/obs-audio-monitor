
#include "audio-monitor-dock.hpp"

#include <QMainWindow>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>

#include "audio-output-control.hpp"
#include "obs-frontend-api.h"
#include "obs-module.h"
#include "obs.h"
#include "util/platform.h"

#define QT_UTF8(str) QString::fromUtf8(str)
#define QT_TO_UTF8(str) str.toUtf8().constData()

MODULE_EXPORT void load_audio_monitor_dock()
{
	const auto main_window =
		static_cast<QMainWindow *>(obs_frontend_get_main_window());
	obs_frontend_push_ui_translation(obs_module_get_string);
	obs_frontend_add_dock(new AudioMonitorDock(main_window));
	obs_frontend_pop_ui_translation();
}

AudioMonitorDock::AudioMonitorDock(QWidget *parent) : QDockWidget(parent)
{

	mainLayout = new QGridLayout;
	mainLayout->setAlignment(Qt::AlignTop | Qt::AlignLeft);
	mainLayout->setRowStretch(1, 1);
	mainLayout->setContentsMargins(4, 4, 4, 4);
	mainLayout->setSpacing(8);

	char *file = obs_module_config_path("config.json");
	obs_data_t *data = nullptr;
	if (file) {
		data = obs_data_create_from_json_file(file);
		bfree(file);
	}
	if (data) {
		showOutputMeter = obs_data_get_bool(data, "showOutputMeter");
		showOutputSlider = obs_data_get_bool(data, "showOutputSlider");
		showOnlyActive = obs_data_get_bool(data, "showOnlyActive");
		auto *outputs = obs_data_get_array(data, "outputs");
		if (outputs) {
			auto output_count = obs_data_array_count(outputs);
			if (output_count > MAX_AUDIO_MIXES)
				output_count = MAX_AUDIO_MIXES;
			for (int i = 0; i < output_count; i++) {
				auto *output_data =
					obs_data_array_item(outputs, i);
				if (output_data) {
					if (obs_data_get_bool(output_data,
							      "enabled")) {
						addOutputTrack(i, output_data);
					}
					obs_data_release(output_data);
				}
			}
		} else {
			auto *control = new AudioOutputControl(0);
			control->setSizePolicy(QSizePolicy::Preferred,
					       QSizePolicy::Expanding);
			mainLayout->addWidget(control, 1, 1);
		}
		obs_data_release(data);
	} else {
		showOutputMeter = false;
		showOutputSlider = false;
		showOnlyActive = false;
		auto *control = new AudioOutputControl(0);
		control->setSizePolicy(QSizePolicy::Preferred,
				       QSizePolicy::Expanding);
		mainLayout->addWidget(control, 1, 1);
	}
	setFeatures(AllDockWidgetFeatures);
	setWindowTitle(QT_UTF8(obs_module_text("AudioMonitor")));
	setObjectName("AudioMonitorDock");
	setFloating(true);
	hide();

	signal_handler_connect_global(obs_get_signal_handler(), OBSSignal,
				      this);
	auto *dockWidgetContents = new QWidget;
	dockWidgetContents->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(dockWidgetContents, &QWidget::customContextMenuRequested, this,
		&AudioMonitorDock::ConfigClicked);

	auto *scrollArea = new HScrollArea();
	scrollArea->setContextMenuPolicy(Qt::CustomContextMenu);
	scrollArea->setFrameShape(QFrame::StyledPanel);
	scrollArea->setFrameShadow(QFrame::Sunken);
	scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
	scrollArea->setWidgetResizable(true);

	auto *stackedMixerArea = new QStackedWidget;
	stackedMixerArea->setObjectName(QStringLiteral("stackedMixerArea"));
	stackedMixerArea->addWidget(scrollArea);

	auto *config = new QPushButton(this);
	config->setProperty("themeID", "configIconSmall");
	config->setFlat(true);
	config->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);
	config->setMaximumSize(22, 22);
	config->setAutoDefault(false);

	mainLayout->addWidget(config, 1, 0);

	connect(config, &QAbstractButton::clicked, this,
		&AudioMonitorDock::ConfigClicked);

	dockWidgetContents->setLayout(mainLayout);
	scrollArea->setWidget(dockWidgetContents);

	setWidget(stackedMixerArea);
}

AudioMonitorDock::~AudioMonitorDock()
{
	signal_handler_disconnect_global(obs_get_signal_handler(), OBSSignal,
					 this);
	char *file = obs_module_config_path("config.json");
	if (file) {
		obs_data_t *data = obs_data_create_from_json_file(file);
		if (!data)
			data = obs_data_create();
		obs_data_set_bool(data, "showOutputMeter", showOutputMeter);
		obs_data_set_bool(data, "showOutputSlider", showOutputSlider);
		obs_data_set_bool(data, "showOnlyActive", showOnlyActive);
		auto *outputs = obs_data_array_create();
		for (int i = 0; i < MAX_AUDIO_MIXES; i++) {
			auto *item = mainLayout->itemAtPosition(1, i + 1);
			obs_data_t *output;
			if (item) {
				AudioOutputControl *control =
					static_cast<AudioOutputControl *>(
						item->widget());
				output = control->GetSettings();
				obs_data_set_bool(output, "enabled", true);
			} else {
				output = obs_data_create();
				obs_data_set_bool(output, "enabled", false);
			}
			obs_data_array_push_back(outputs, output);
			obs_data_release(output);
		}
		obs_data_set_array(data, "outputs", outputs);
		obs_data_array_release(outputs);

		if (!obs_data_save_json(data, file)) {
			char *path = obs_module_config_path("");
			if (path) {
				os_mkdirs(path);
				bfree(path);
			}
			obs_data_save_json(data, file);
		}
		obs_data_release(data);
		bfree(file);
	}
}

void AudioMonitorDock::OBSSignal(void *data, const char *signal,
				 calldata_t *call_data)
{
	obs_source_t *source =
		static_cast<obs_source_t *>(calldata_ptr(call_data, "source"));
	if (!source)
		return;
	const uint32_t flags = obs_source_get_output_flags(source);
	if ((flags & OBS_SOURCE_AUDIO) == 0)
		return;
	AudioMonitorDock *dock = static_cast<AudioMonitorDock *>(data);

	if (strcmp(signal, "source_create") == 0) {
		signal_handler_connect(obs_source_get_signal_handler(source),
				       "filter_add", OBSFilterAdd, data);
		signal_handler_connect(obs_source_get_signal_handler(source),
				       "filter_remove", OBSFilterRemove, data);
		if (!dock->showOnlyActive || obs_source_active(source)) {
			QMetaObject::invokeMethod(dock, "AddAudioSource",
						  Q_ARG(OBSSource,
							OBSSource(source)));
		}
	} else if (strcmp(signal, "source_load") == 0) {
		if (!dock->showOnlyActive || obs_source_active(source)) {
			QMetaObject::invokeMethod(dock, "AddAudioSource",
						  Q_ARG(OBSSource,
							OBSSource(source)));
		}
	} else if (strcmp(signal, "source_remove") == 0 ||
		   strcmp(signal, "source_destroy") == 0) {
		signal_handler_disconnect(obs_source_get_signal_handler(source),
					  "filter_add", OBSFilterAdd, data);
		signal_handler_disconnect(obs_source_get_signal_handler(source),
					  "filter_remove", OBSFilterRemove,
					  data);
		const QString sourceName = obs_source_get_name(source);
		QMetaObject::invokeMethod(dock, "RemoveAudioControl",
					  Q_ARG(QString, QString(sourceName)));
	} else if (strcmp(signal, "source_volume") == 0) {
	} else if (strcmp(signal, "source_rename") == 0) {
		QString new_name =
			QT_UTF8(calldata_string(call_data, "new_name"));
		QString prev_name =
			QT_UTF8(calldata_string(call_data, "prev_name"));
		QMetaObject::invokeMethod(dock, "RenameAudioControl",
					  Q_ARG(QString, QString(new_name)),
					  Q_ARG(QString, QString(prev_name)));
	} else if (strcmp(signal, "source_activate") == 0) {
		if (!dock->showOnlyActive)
			return;
		QMetaObject::invokeMethod(dock, "AddAudioSource",
					  Q_ARG(OBSSource, OBSSource(source)));
	} else if (strcmp(signal, "source_deactivate") == 0) {
		if (!dock->showOnlyActive)
			return;
		QString sourceName = QT_UTF8(obs_source_get_name(source));
		QMetaObject::invokeMethod(dock, "RemoveAudioControl",
					  Q_ARG(QString, QString(sourceName)));
	}
}

void AudioMonitorDock::RenameAudioControl(QString new_name, QString prev_name)
{

	int columns = mainLayout->columnCount();

	for (int column = MAX_AUDIO_MIXES + 1; column < columns; column++) {
		QLayoutItem *item = mainLayout->itemAtPosition(0, column);
		if (!item)
			continue;
		auto *l = static_cast<QLabel *>(item->widget());
		if (prev_name == l->objectName()) {
			l->setText(new_name);
			l->setObjectName(new_name);
		}
	}
}

void AudioMonitorDock::AddAudioSource(OBSSource source)
{
	OBSAddAudioSource(this, source);
}

void AudioMonitorDock::moveAudioControl(int fromColumn, int toColumn)
{
	int rows = mainLayout->rowCount();
	for (int row = 0; row < rows; row++) {
		QLayoutItem *item = mainLayout->itemAtPosition(row, fromColumn);
		if (item) {
			mainLayout->removeItem(item);
			if (toColumn < 0) {
				QWidget *w = item->widget();
				delete w;
			} else {
				mainLayout->addItem(item, row, toColumn);
			}
		}
	}
}

void AudioMonitorDock::addAudioControl(obs_source_t *source, int column,
				       obs_source_t *filter)
{
	QString sourceName = QT_UTF8(obs_source_get_name(source));
	auto *nameLabel = new QLabel();
	QFont font = nameLabel->font();
	font.setPointSize(font.pointSize() - 1);
	nameLabel->setWordWrap(true);

	nameLabel->setText(sourceName);
	nameLabel->setFont(font);
	nameLabel->setObjectName(sourceName);
	nameLabel->setAlignment(Qt::AlignCenter);

	mainLayout->addWidget(nameLabel, 0, column);

	auto *audioControl =
		new AudioControl(obs_source_get_weak_source(source));
	audioControl->setSizePolicy(QSizePolicy::Preferred,
				    QSizePolicy::Expanding);

	audioControl->ShowOutputMeter(showOutputMeter);
	obs_data_t *priv_settings = obs_source_get_private_settings(source);
	bool hidden = obs_data_get_bool(priv_settings, "mixer_hidden");
	obs_data_release(priv_settings);
	audioControl->ShowOutputSlider(showOutputSlider && !hidden);
	mainLayout->addWidget(audioControl, 1, column);
	if (filter)
		addFilter(column, filter);
}

void AudioMonitorDock::OBSFilterAdd(void *data, calldata_t *call_data)
{
	obs_source_t *filter;
	calldata_get_ptr(call_data, "filter", &filter);
	obs_source_t *source;
	calldata_get_ptr(call_data, "source", &source);

	const char *filter_id = obs_source_get_unversioned_id(filter);
	if (strcmp("audio_monitor", filter_id) != 0)
		return;
	AudioMonitorDock *dock = static_cast<AudioMonitorDock *>(data);
	QMetaObject::invokeMethod(dock, "AddFilter",
				  Q_ARG(OBSSource, OBSSource(source)),
				  Q_ARG(OBSSource, OBSSource(filter)));
}

void AudioMonitorDock::AddFilter(OBSSource source, OBSSource filter)
{

	const int columns = mainLayout->columnCount();
	if (columns <= MAX_AUDIO_MIXES + 1) {
		addAudioControl(source, MAX_AUDIO_MIXES + 1, filter);
		return;
	}
	QString sourceName = QT_UTF8(obs_source_get_name(source));
	for (int i = MAX_AUDIO_MIXES + 1; i < columns; i++) {
		QLayoutItem *item = mainLayout->itemAtPosition(0, i);
		if (item) {
			QWidget *w = item->widget();
			if (sourceName.localeAwareCompare(w->objectName()) ==
			    0) {

				addFilter(i, filter);
				return;
			}
		}
	}

	if (showOnlyActive && !obs_source_active(source))
		return;

	for (int i = columns - 1; i > MAX_AUDIO_MIXES; i--) {
		QLayoutItem *item = mainLayout->itemAtPosition(0, i);
		if (item) {
			QWidget *w = item->widget();
			if (sourceName.localeAwareCompare(w->objectName()) <
			    0) {
				moveAudioControl(i, i + 1);
			} else {
				addAudioControl(source, i + 1, filter);
				break;
			}
		}
		if (i == MAX_AUDIO_MIXES + 1) {
			addAudioControl(source, i, filter);
		}
	}
}

void AudioMonitorDock::OBSFilterRemove(void *data, calldata_t *call_data)
{
	obs_source_t *filter;
	calldata_get_ptr(call_data, "filter", &filter);
	obs_source_t *source;
	calldata_get_ptr(call_data, "source", &source);

	const char *filter_id = obs_source_get_unversioned_id(filter);
	if (strcmp("audio_monitor", filter_id) != 0)
		return;

	AudioMonitorDock *dock = static_cast<AudioMonitorDock *>(data);
	QString sourceName = QT_UTF8(obs_source_get_name(source));
	QString filterName = QT_UTF8(obs_source_get_name(filter));
	QMetaObject::invokeMethod(dock, "RemoveFilter",
				  Q_ARG(QString, sourceName),
				  Q_ARG(QString, filterName));
}

void AudioMonitorDock::RemoveFilter(QString sourceName, QString filterName)
{
	int columns = mainLayout->columnCount();
	int removed = 0;
	for (int i = MAX_AUDIO_MIXES + 1; i < columns; i++) {
		QLayoutItem *item = mainLayout->itemAtPosition(0, i);
		if (item) {
			QWidget *w = item->widget();
			if (sourceName.localeAwareCompare(w->objectName()) ==
			    0) {
				item = mainLayout->itemAtPosition(1, i);
				if (!item)
					continue;

				AudioControl *audioControl =
					static_cast<AudioControl *>(
						item->widget());
				audioControl->RemoveFilter(filterName);
				if (!audioControl->HasSliders()) {
					moveAudioControl(i, -1);
					removed++;
				}
			} else if (removed > 0) {
				moveAudioControl(i, i - removed);
			}
		}
	}
}

void AudioMonitorDock::RemoveAudioControl(const QString &sourceName)
{
	const int columns = mainLayout->columnCount();
	int removed = 0;
	for (int i = MAX_AUDIO_MIXES + 1; i < columns; i++) {
		QLayoutItem *item = mainLayout->itemAtPosition(0, i);
		if (item) {
			QWidget *w = item->widget();
			if (sourceName.localeAwareCompare(w->objectName()) ==
			    0) {
				item = mainLayout->itemAtPosition(1, i);
				if (!item)
					continue;

				moveAudioControl(i, -1);
				removed++;
			} else if (removed > 0) {
				moveAudioControl(i, i - removed);
			}
		}
	}
}

void AudioMonitorDock::addFilter(int column, obs_source_t *filter)
{
	QLayoutItem *item = mainLayout->itemAtPosition(1, column);
	if (!item)
		return;

	AudioControl *audioControl =
		static_cast<AudioControl *>(item->widget());
	audioControl->AddFilter(filter);
}

bool AudioMonitorDock::OBSAddAudioDevice(void *data, const char *name,
					 const char *id)
{
	if (!id || !strlen(id))
		return true;
	AudioMonitorDock *dock = static_cast<AudioMonitorDock *>(data);
	if (!name || !strlen(name)) {
		dock->audioDevices[QT_UTF8(id)] = QT_UTF8(id);
	} else {
		dock->audioDevices[QT_UTF8(id)] = QT_UTF8(name);
	}
	return true;
}

void AudioMonitorDock::ConfigClicked()
{
	QMenu popup;
	auto *a = popup.addAction(QT_UTF8(obs_module_text("MeterOutput")));
	a->setCheckable(true);
	a->setChecked(showOutputMeter);
	connect(a, SIGNAL(triggered()), this, SLOT(MeterOutputChanged()));
	a = popup.addAction(QT_UTF8(obs_module_text("OutputSlider")));
	a->setCheckable(true);
	a->setChecked(showOutputSlider);
	connect(a, SIGNAL(triggered()), this, SLOT(OutputSliderChanged()));
	a = popup.addAction(QT_UTF8(obs_module_text("OnlyActive")));
	a->setCheckable(true);
	a->setChecked(showOnlyActive);
	connect(a, SIGNAL(triggered()), this, SLOT(OnlyActiveChanged()));

	auto *outputs = popup.addMenu(QT_UTF8(obs_module_text("Outputs")));
	for (int i = 0; i < MAX_AUDIO_MIXES; i++) {
		QString trackName = QT_UTF8(obs_module_text("Track"));
		trackName += " ";
		trackName += QString::number(i + 1);
		auto *trackMenu = outputs->addMenu(trackName);
		trackMenu->setProperty("track", i);
		connect(trackMenu, SIGNAL(aboutToShow()), this,
			SLOT(LoadTrackMenu()));
	}

	audioDevices.clear();
	obs_enum_audio_monitoring_devices(OBSAddAudioDevice, this);

	popup.exec(QCursor::pos());
}

void AudioMonitorDock::LoadTrackMenu()
{
	auto *menu = static_cast<QMenu *>(sender());
	if (!menu->isEmpty())
		return;
	int track = menu->property("track").toInt();
	auto *a = menu->addAction(QT_UTF8(obs_module_text("Show")));
	a->setProperty("track", track);
	a->setCheckable(true);
	connect(a, SIGNAL(triggered()), this, SLOT(ShowOutputChanged()));
	auto *item = mainLayout->itemAtPosition(1, track + 1);
	if (!item)
		return;
	auto *output = static_cast<AudioOutputControl *>(item->widget());
	a->setChecked(true);
	menu->addSeparator();
	auto d = audioDevices.begin();
	while (d != audioDevices.end()) {
		a = menu->addAction(d.value());
		a->setCheckable(true);
		a->setProperty("track", track);
		a->setProperty("device_id", d.key());
		a->setChecked(output->HasDevice(d.key()));
		connect(a, SIGNAL(triggered()), this,
			SLOT(OutputDeviceChanged()));
		++d;
	}
}

void AudioMonitorDock::ShowOutputChanged()
{
	auto *a = static_cast<QAction *>(sender());
	bool checked = a->isChecked();
	int track = a->property("track").toInt();
	if (checked) {
		auto *item = mainLayout->itemAtPosition(1, track + 1);
		if (item)
			return;
		addOutputTrack(track);
	} else {
		moveAudioControl(track + 1, -1);
	}
}

void AudioMonitorDock::OutputDeviceChanged()
{
	auto *a = static_cast<QAction *>(sender());
	bool checked = a->isChecked();
	int track = a->property("track").toInt();
	auto *item = mainLayout->itemAtPosition(1, track + 1);
	if (!item)
		return;
	AudioOutputControl *control =
		static_cast<AudioOutputControl *>(item->widget());

	QString device_id = a->property("device_id").toString();
	if (checked) {
		control->AddDevice(device_id, a->text());
	} else {
		control->RemoveDevice(device_id);
	}
}

void AudioMonitorDock::MeterOutputChanged()
{
	QAction *a = static_cast<QAction *>(sender());
	showOutputMeter = a->isChecked();
	const int columns = mainLayout->columnCount();
	for (int column = MAX_AUDIO_MIXES + 1; column < columns; column++) {
		QLayoutItem *item = mainLayout->itemAtPosition(1, column);
		if (item) {
			AudioControl *audioControl =
				static_cast<AudioControl *>(item->widget());
			audioControl->ShowOutputMeter(showOutputMeter);
		}
	}
}

void AudioMonitorDock::OutputSliderChanged()
{
	QAction *a = static_cast<QAction *>(sender());
	showOutputSlider = a->isChecked();
	if (showOutputSlider) {
		obs_enum_sources(OBSAddAudioSource, this);
	} else {
		const int columns = mainLayout->columnCount();
		int removed = 0;
		for (int column = MAX_AUDIO_MIXES + 1; column < columns;
		     column++) {
			QLayoutItem *item =
				mainLayout->itemAtPosition(1, column);
			if (item) {
				AudioControl *audioControl =
					static_cast<AudioControl *>(
						item->widget());
				audioControl->ShowOutputSlider(
					showOutputSlider);
				if (!audioControl->HasSliders()) {
					moveAudioControl(column, -1);
					removed++;
				} else if (removed > 0) {
					moveAudioControl(column,
							 column - removed);
				}
			}
		}
	}
}

void AudioMonitorDock::OBSFilterAdd(obs_source_t *source, obs_source_t *filter,
				    void *data)
{
	const char *filter_id = obs_source_get_unversioned_id(filter);
	if (strcmp("audio_monitor", filter_id) != 0)
		return;

	AudioMonitorDock *dock = static_cast<AudioMonitorDock *>(data);
	QString sourceName = QT_UTF8(obs_source_get_name(source));
	const int columns = dock->mainLayout->columnCount();
	for (int i = MAX_AUDIO_MIXES + 1; i < columns; i++) {
		QLayoutItem *item = dock->mainLayout->itemAtPosition(0, i);
		if (item) {
			QWidget *w = item->widget();
			if (sourceName.localeAwareCompare(w->objectName()) ==
			    0) {

				dock->addFilter(i, filter);
				return;
			}
		}
	}
}

bool AudioMonitorDock::OBSAddAudioSource(void *data, obs_source_t *source)
{
	const uint32_t flags = obs_source_get_output_flags(source);
	if ((flags & OBS_SOURCE_AUDIO) == 0)
		return true;
	AudioMonitorDock *dock = static_cast<AudioMonitorDock *>(data);
	QString sourceName = QT_UTF8(obs_source_get_name(source));
	int columns = dock->mainLayout->columnCount();
	for (int i = MAX_AUDIO_MIXES + 1; i < columns; i++) {
		QLayoutItem *item = dock->mainLayout->itemAtPosition(0, i);
		if (item) {
			QWidget *w = item->widget();
			if (sourceName == w->objectName()) {
				item = dock->mainLayout->itemAtPosition(1, i);
				AudioControl *audioControl =
					static_cast<AudioControl *>(
						item->widget());
				obs_data_t *priv_settings =
					obs_source_get_private_settings(source);
				bool hidden = obs_data_get_bool(priv_settings,
								"mixer_hidden");
				obs_data_release(priv_settings);
				audioControl->ShowOutputSlider(
					dock->showOutputSlider && !hidden);
				if (!dock->showOutputSlider || hidden)
					dock->RemoveSourcesWithoutSliders();
				return true;
			}
		}
	}
	if (dock->showOnlyActive && !obs_source_active(source))
		return true;
	if (columns <= MAX_AUDIO_MIXES + 1) {
		dock->addAudioControl(source, MAX_AUDIO_MIXES + 1, nullptr);
	} else {
		for (int i = columns - 1; i > MAX_AUDIO_MIXES; i--) {
			QLayoutItem *item =
				dock->mainLayout->itemAtPosition(0, i);
			if (item) {
				QWidget *w = item->widget();
				if (sourceName.localeAwareCompare(
					    w->objectName()) < 0) {
					dock->moveAudioControl(i, i + 1);
				} else {
					dock->addAudioControl(source, i + 1,
							      nullptr);
					break;
				}
			}
			if (i == MAX_AUDIO_MIXES + 1) {
				dock->addAudioControl(source, i, nullptr);
			}
		}
	}
	obs_source_enum_filters(source, OBSFilterAdd, data);
	dock->RemoveSourcesWithoutSliders();
	return true;
}

void AudioMonitorDock::RemoveSourcesWithoutSliders()
{
	const int columns = mainLayout->columnCount();
	int removed = 0;
	for (int column = MAX_AUDIO_MIXES + 1; column < columns; column++) {
		QLayoutItem *item = mainLayout->itemAtPosition(1, column);
		if (item) {
			AudioControl *audioControl =
				static_cast<AudioControl *>(item->widget());
			if (!audioControl->HasSliders()) {
				moveAudioControl(column, -1);
				removed++;
			} else if (removed > 0) {
				moveAudioControl(column, column - removed);
			}
		}
	}
}

void AudioMonitorDock::OnlyActiveChanged()
{
	QAction *a = static_cast<QAction *>(sender());
	showOnlyActive = a->isChecked();
	if (showOnlyActive) {
		int columns = mainLayout->columnCount();
		int removed = 0;
		for (int column = MAX_AUDIO_MIXES + 1; column < columns;
		     column++) {
			QLayoutItem *item =
				mainLayout->itemAtPosition(1, column);
			if (item) {
				AudioControl *audioControl =
					static_cast<AudioControl *>(
						item->widget());
				obs_source_t *s = obs_weak_source_get_source(
					audioControl->GetSource());
				if (s) {
					if (!obs_source_active(s)) {
						moveAudioControl(column, -1);
						removed++;
					} else if (removed > 0) {
						moveAudioControl(
							column,
							column - removed);
					}
					obs_source_release(s);
				} else if (removed > 0) {
					moveAudioControl(column,
							 column - removed);
				}
			}
		}
	} else {
		obs_enum_sources(OBSAddAudioSource, this);
	}
}

void AudioMonitorDock::addOutputTrack(int i, obs_data_t *obs_data)
{
	auto *control = new AudioOutputControl(i, obs_data);
	control->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
	mainLayout->addWidget(control, 1, i + 1);
	QString trackName = QT_UTF8(obs_module_text("Track"));
	trackName += " ";
	trackName += QString::number(i + 1);
	auto *nameLabel = new QLabel();
	QFont font = nameLabel->font();
	font.setPointSize(font.pointSize() - 1);
	nameLabel->setWordWrap(true);

	nameLabel->setText(trackName);
	nameLabel->setFont(font);
	nameLabel->setAlignment(Qt::AlignCenter);

	mainLayout->addWidget(nameLabel, 0, i + 1);
}

void HScrollArea::resizeEvent(QResizeEvent *event)
{
	if (!!widget())
		widget()->setMaximumHeight(event->size().height());

	QScrollArea::resizeEvent(event);
}
