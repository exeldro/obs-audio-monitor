#include "audio-control.hpp"

#include <QCheckBox>
#include <QVBoxLayout>
#include "utils.hpp"

#include "obs-module.h"

AudioControl::AudioControl(OBSWeakSource source_)
	: source(std::move(source_)),
	  obs_volmeter(obs_volmeter_create(OBS_FADER_LOG))
{
	obs_source_t *s = obs_weak_source_get_source(source);
	obs_volmeter_attach_source(obs_volmeter, s);

	volMeter = new VolumeMeter(nullptr, obs_volmeter);
	volMeter->muted = obs_source_muted(s);
	volMeter->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

	mainLayout = new QGridLayout;
	mainLayout->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
	mainLayout->setContentsMargins(0, 0, 0, 0);
	mainLayout->setSpacing(2);
	mainLayout->addWidget(volMeter, 0, 0, -1, 1);

	setLayout(mainLayout);

	obs_volmeter_add_callback(obs_volmeter, OBSVolumeLevel, this);

	obs_source_release(s);
}

AudioControl::~AudioControl()
{
	obs_source_t *s = obs_weak_source_get_source(source);
	if (s) {
		signal_handler_disconnect(obs_source_get_signal_handler(s),
					  "mute", OBSMute, this);
		signal_handler_disconnect(obs_source_get_signal_handler(s),
					  "volume", OBSVolume, this);
		int columns = mainLayout->columnCount();
		for (int column = 2; column < columns; column++) {
			QLayoutItem *item =
				mainLayout->itemAtPosition(1, column);
			if (item) {
				QString filterName =
					item->widget()->objectName();
				obs_source_t *filter =
					obs_source_get_filter_by_name(
						s, QT_TO_UTF8(filterName));
				if (filter) {
					signal_handler_disconnect(
						obs_source_get_signal_handler(
							filter),
						"rename", OBSFilterRename,
						this);
					signal_handler_disconnect(
						obs_source_get_signal_handler(
							filter),
						"updated", OBSFilterUpdated,
						this);
					signal_handler_disconnect(
						obs_source_get_signal_handler(
							filter),
						"enable", OBSFilterEnable,
						this);
					obs_source_release(filter);
				}
			}
		}
		obs_source_release(s);
	}
	obs_volmeter_remove_callback(obs_volmeter, OBSVolumeLevel, this);
	obs_volmeter_destroy(obs_volmeter);
	obs_weak_source_release(source);
}

void AudioControl::OBSVolumeLevel(void *data,
				  const float magnitude[MAX_AUDIO_CHANNELS],
				  const float peak[MAX_AUDIO_CHANNELS],
				  const float inputPeak[MAX_AUDIO_CHANNELS])
{
	AudioControl *audioControl = static_cast<AudioControl *>(data);

	audioControl->volMeter->setLevels(magnitude, peak, inputPeak);
}

void AudioControl::LockVolumeControl(bool lock)
{
	QCheckBox *checkbox = reinterpret_cast<QCheckBox *>(sender());
	int columns = mainLayout->columnCount();
	for (int column = 1; column < columns; column++) {
		QLayoutItem *item = mainLayout->itemAtPosition(0, column);
		if (!item)
			continue;
		if (item->widget() == checkbox) {
			item = mainLayout->itemAtPosition(1, column);
			item->widget()->setEnabled(!lock);
			item = mainLayout->itemAtPosition(2, column);
			item->widget()->setEnabled(!lock);
			obs_source_t *s = obs_weak_source_get_source(source);
			if (!s)
				return;
			if (column == 1) {
				obs_data_t *settings =
					obs_source_get_private_settings(s);
				obs_data_set_bool(settings, "volume_locked",
						  lock);
				obs_data_release(settings);
			} else {
				item = mainLayout->itemAtPosition(1, column);
				QString filterName =
					item->widget()->objectName();
				obs_source_t *f = obs_source_get_filter_by_name(
					s, QT_TO_UTF8(filterName));
				if (f) {
					obs_data_t *settings =
						obs_source_get_settings(f);
					obs_data_set_bool(settings, "locked",
							  lock);
					obs_data_release(settings);
					obs_source_release(f);
				}
			}
			obs_source_release(s);
			return;
		}
	}
}

void AudioControl::MuteVolumeControl(bool mute)
{
	QCheckBox *checkbox = reinterpret_cast<QCheckBox *>(sender());
	int columns = mainLayout->columnCount();
	for (int column = 1; column < columns; column++) {
		QLayoutItem *item = mainLayout->itemAtPosition(muteRow, column);
		if (!item)
			continue;
		if (item->widget() == checkbox) {
			item = mainLayout->itemAtPosition(sliderRow, column);
			obs_source_t *s = obs_weak_source_get_source(source);
			if (!s)
				return;
			if (column == 1) {
				if (obs_source_muted(s) != mute)
					obs_source_set_muted(s, mute);
				obs_source_release(s);
				return;
			}
			obs_source_t *f = obs_source_get_filter_by_name(
				s, QT_TO_UTF8(item->widget()->objectName()));
			obs_source_release(s);
			if (!f)
				return;
			if (obs_source_enabled(f) == mute)
				obs_source_set_enabled(f, !mute);
			obs_source_release(f);
			return;
		}
	}
}

void AudioControl::ShowOutputMeter(bool output)
{
	volMeter->ShowOutputMeter(output);
}

void AudioControl::ShowSliderNames(bool show)
{
	showSliderNames = show;
	int columns = mainLayout->columnCount();
	if (show) {
		for (int column = 1; column < columns; column++) {

			QLayoutItem *item =
				mainLayout->itemAtPosition(sliderRow, column);
			if (!item)
				continue;
			QString name = item->widget()->objectName();
			if (column == 1) {
				name = QT_UTF8(obs_module_text("OutputShort"));
			}
			item = mainLayout->itemAtPosition(nameRow, column);
			if (!item) {
				auto *nameLabel = new QLabel();
				QFont font = nameLabel->font();
				font.setPointSize(font.pointSize() - 1);
				nameLabel->setWordWrap(true);

				nameLabel->setText(name);
				nameLabel->setFont(font);
				nameLabel->setAlignment(Qt::AlignCenter);

				mainLayout->addWidget(nameLabel, nameRow,
						      column);
			}
		}
	} else {
		for (int column = 1; column < columns; column++) {

			QLayoutItem *item =
				mainLayout->itemAtPosition(nameRow, column);
			if (!item)
				continue;
			auto *w = item->widget();
			mainLayout->removeItem(item);
			delete w;
		}
	}
}

#define LOG_OFFSET_DB 6.0f
#define LOG_RANGE_DB 96.0f
/* equals -log10f(LOG_OFFSET_DB) */
#define LOG_OFFSET_VAL -0.77815125038364363f
/* equals -log10f(-LOG_RANGE_DB + LOG_OFFSET_DB) */
#define LOG_RANGE_VAL -2.00860017176191756f

void AudioControl::ShowOutputSlider(bool output)
{
	if (output) {
		auto *item = mainLayout->itemAtPosition(1, 1);
		if (item)
			return;
		obs_source_t *s = obs_weak_source_get_source(source);
		if (!s)
			return;

		obs_data_t *settings = obs_source_get_private_settings(s);
		bool lock = obs_data_get_bool(settings, "volume_locked");
		obs_data_release(settings);

		auto *locked = new LockedCheckBox();
		locked->setSizePolicy(QSizePolicy::Maximum,
				      QSizePolicy::Maximum);
		locked->setFixedSize(16, 16);
		locked->setChecked(lock);
		locked->setStyleSheet("background: none");

		connect(locked, &QCheckBox::stateChanged, this,
			&AudioControl::LockVolumeControl, Qt::DirectConnection);

		auto *slider = new SliderIgnoreScroll();
		slider->setSizePolicy(QSizePolicy::Preferred,
				      QSizePolicy::Expanding);
		slider->setEnabled(!lock);
		slider->setMinimum(0);
		slider->setMaximum(10000);
		slider->setToolTip(QT_UTF8(obs_module_text("VolumeOutput")));
		float mul = obs_source_get_volume(s);
		float db = obs_mul_to_db(mul);
		float def;
		if (db >= 0.0f)
			def = 1.0f;
		else if (db <= -96.0f)
			def = 0.0f;
		else
			def = (-log10f(-db + LOG_OFFSET_DB) - LOG_RANGE_VAL) /
			      (LOG_OFFSET_VAL - LOG_RANGE_VAL);
		slider->setValue(def * 10000.0f);

		connect(slider, SIGNAL(valueChanged(int)), this,
			SLOT(SliderChanged(int)));

		auto *mute = new MuteCheckBox();
		mute->setEnabled(!lock);
		mute->setChecked(obs_source_muted(s));

		connect(mute, &QCheckBox::stateChanged, this,
			&AudioControl::MuteVolumeControl, Qt::DirectConnection);

		signal_handler_connect(obs_source_get_signal_handler(s), "mute",
				       OBSMute, this);
		signal_handler_connect(obs_source_get_signal_handler(s),
				       "volume", OBSVolume, this);
		obs_source_release(s);

		mainLayout->addWidget(locked, 0, 1, Qt::AlignHCenter);
		mainLayout->addWidget(slider, 1, 1, Qt::AlignHCenter);
		mainLayout->addWidget(mute, 2, 1, Qt::AlignHCenter);
		if (showSliderNames) {
			auto *nameLabel = new QLabel();
			QFont font = nameLabel->font();
			font.setPointSize(font.pointSize() - 1);
			nameLabel->setWordWrap(true);

			nameLabel->setText(
				QT_UTF8(obs_module_text("OutputShort")));
			nameLabel->setFont(font);
			nameLabel->setAlignment(Qt::AlignCenter);

			mainLayout->addWidget(nameLabel, nameRow, 1,
					      Qt::AlignHCenter);
		}
	} else {
		int rows = mainLayout->rowCount();
		for (int row = 0; row < rows; row++) {
			auto *item = mainLayout->itemAtPosition(row, 1);
			if (item) {
				auto *w = item->widget();
				mainLayout->removeItem(item);
				delete w;
			}
		}
		obs_source_t *s = obs_weak_source_get_source(source);
		if (!s)
			return;
		signal_handler_disconnect(obs_source_get_signal_handler(s),
					  "mute", OBSMute, this);
		signal_handler_disconnect(obs_source_get_signal_handler(s),
					  "volume", OBSVolume, this);
		obs_source_release(s);
	}
}

void AudioControl::OBSVolume(void *data, calldata_t *call_data)
{
	obs_source_t *source;
	calldata_get_ptr(call_data, "source", &source);
	double volume;
	calldata_get_float(call_data, "volume", &volume);
	AudioControl *audioControl = static_cast<AudioControl *>(data);
	QMetaObject::invokeMethod(audioControl, "SetOutputVolume",
				  Qt::QueuedConnection, Q_ARG(double, volume));
}

void AudioControl::SetOutputVolume(double volume)
{
	QLayoutItem *item = mainLayout->itemAtPosition(1, 1);
	if (!item)
		return;
	auto *slider = static_cast<QSlider *>(item->widget());
	float db = obs_mul_to_db(volume);
	float def;
	if (db >= 0.0f)
		def = 1.0f;
	else if (db <= -96.0f)
		def = 0.0f;
	else
		def = (-log10f(-db + LOG_OFFSET_DB) - LOG_RANGE_VAL) /
		      (LOG_OFFSET_VAL - LOG_RANGE_VAL);

	int val = def * 10000.0;
	changing_output_volume = true;
	slider->setValue(val);
	changing_output_volume = false;
}

void AudioControl::OBSMute(void *data, calldata_t *call_data)
{
	obs_source_t *source;
	calldata_get_ptr(call_data, "source", &source);
	bool muted = calldata_bool(call_data, "muted");
	AudioControl *audioControl = static_cast<AudioControl *>(data);
	QMetaObject::invokeMethod(audioControl, "SetMute", Qt::QueuedConnection,
				  Q_ARG(bool, muted));
}

void AudioControl::SetMute(bool muted)
{
	volMeter->muted = muted;
	QLayoutItem *item = mainLayout->itemAtPosition(2, 1);
	if (!item)
		return;
	auto *mute = static_cast<QCheckBox *>(item->widget());
	if (mute->isChecked() != muted)
		mute->setChecked(muted);
}

void AudioControl::OBSFilterRename(void *data, calldata_t *call_data)
{
	const char *prev_name = calldata_string(call_data, "prev_name");
	const char *new_name = calldata_string(call_data, "new_name");
	AudioControl *audioControl = static_cast<AudioControl *>(data);
	QMetaObject::invokeMethod(audioControl, "RenameFilter",
				  Qt::QueuedConnection,
				  Q_ARG(QString, QT_UTF8(prev_name)),
				  Q_ARG(QString, QT_UTF8(new_name)));
}

void AudioControl::OBSFilterEnable(void *data, calldata_t *call_data)
{
	obs_source_t *filter;
	calldata_get_ptr(call_data, "source", &filter);
	bool enabled = calldata_bool(call_data, "enabled");
	QString filterName = QT_UTF8(obs_source_get_name(filter));
	AudioControl *audioControl = static_cast<AudioControl *>(data);
	QMetaObject::invokeMethod(audioControl, "FilterEnable",
				  Qt::QueuedConnection,
				  Q_ARG(QString, filterName),
				  Q_ARG(bool, enabled));
}

void AudioControl::FilterEnable(QString name, bool enabled)
{
	int columns = mainLayout->columnCount();
	for (int column = 2; column < columns; column++) {
		QLayoutItem *item =
			mainLayout->itemAtPosition(sliderRow, column);
		if (!item)
			continue;
		auto *l = static_cast<QLabel *>(item->widget());
		if (l->objectName() == name) {
			item = mainLayout->itemAtPosition(muteRow, column);
			QCheckBox *checkbox =
				reinterpret_cast<QCheckBox *>(item->widget());
			if (checkbox->isChecked() == enabled)
				checkbox->setChecked(!enabled);
		}
	}
}

void AudioControl::OBSFilterUpdated(void *data, calldata_t *call_data)
{
	obs_source_t *filter;
	calldata_get_ptr(call_data, "source", &filter);
	QString filterName = QT_UTF8(obs_source_get_name(filter));
	obs_data_t *settings = obs_source_get_settings(filter);
	double volume = obs_data_get_double(settings, "volume");
	bool locked = obs_data_get_bool(settings, "locked");
	bool custom_color = obs_data_get_bool(settings, "custom_color");
	auto color = obs_data_get_int(settings, "color");
	QColor c =
		QColor(color & 0xff, (color >> 8) & 0xff, (color >> 16) & 0xff);
	obs_data_release(settings);
	AudioControl *audioControl = static_cast<AudioControl *>(data);
	QMetaObject::invokeMethod(audioControl, "FilterUpdated",
				  Qt::QueuedConnection,
				  Q_ARG(QString, filterName),
				  Q_ARG(double, volume), Q_ARG(bool, locked),
				  Q_ARG(bool, custom_color), Q_ARG(QColor, c));
}

void AudioControl::FilterUpdated(QString name, double volume, bool locked,
				 bool custom_color, QColor color)
{
	int columns = mainLayout->columnCount();
	for (int column = 2; column < columns; column++) {
		QLayoutItem *item =
			mainLayout->itemAtPosition(sliderRow, column);
		if (!item)
			continue;
		auto *l = static_cast<QLabel *>(item->widget());
		if (l->objectName() == name) {
			auto *slider = static_cast<QSlider *>(item->widget());
			int def = volume * 100.0;
			if (slider->value() != def) {
				changing_monitor_volume = true;
				slider->setValue(def);
				changing_monitor_volume = false;
			}
			slider->setStyleSheet(
				custom_color
					? QString("QSlider::handle {background-color: %1;}")
						  .arg(color.name())
					: "");

			item = mainLayout->itemAtPosition(lockRow, column);
			QCheckBox *checkbox =
				reinterpret_cast<QCheckBox *>(item->widget());
			if (checkbox->isChecked() != locked)
				checkbox->setChecked(locked);
		}
	}
}

void AudioControl::RenameFilter(QString prev_name, QString new_name)
{
	int columns = mainLayout->columnCount();
	for (int column = 2; column < columns; column++) {
		QLayoutItem *item =
			mainLayout->itemAtPosition(sliderRow, column);
		if (!item)
			continue;
		auto *w = item->widget();
		if (w->objectName() == prev_name) {
			w->setObjectName(new_name);
			QString toolTip = QT_UTF8(obs_module_text("Volume"));
			toolTip += " ";
			w->setToolTip(toolTip + new_name);

			item = mainLayout->itemAtPosition(nameRow, column);
			if (item) {
				auto *l =
					dynamic_cast<QLabel *>(item->widget());
				l->setText(new_name);
			}
		}
	}
}

void AudioControl::RemoveFilter(QString filterName)
{
	obs_source_t *s = obs_weak_source_get_source(source);
	if (s) {
		obs_source_t *filter = obs_source_get_filter_by_name(
			s, QT_TO_UTF8(filterName));
		if (filter) {
			signal_handler_disconnect(
				obs_source_get_signal_handler(filter), "rename",
				OBSFilterRename, this);
			signal_handler_disconnect(
				obs_source_get_signal_handler(filter),
				"updated", OBSFilterUpdated, this);
			signal_handler_disconnect(
				obs_source_get_signal_handler(filter), "enable",
				OBSFilterEnable, this);
			obs_source_release(filter);
		}
		obs_source_release(s);
	}
	int columns = mainLayout->columnCount();
	bool found = true;
	for (int column = 2; column < columns; column++) {
		QLayoutItem *item = mainLayout->itemAtPosition(1, column);
		if (!item)
			continue;
		QWidget *w = item->widget();
		if (filterName.localeAwareCompare(w->objectName()) == 0) {
			found = true;
			int rows = mainLayout->rowCount();
			for (int row = 0; row < rows; row++) {
				auto *item =
					mainLayout->itemAtPosition(row, column);
				if (item) {
					auto *w = item->widget();
					mainLayout->removeItem(item);
					delete w;
				}
			}
		} else if (found) {
			int rows = mainLayout->rowCount();
			for (int row = 0; row < rows; row++) {
				auto *item =
					mainLayout->itemAtPosition(row, column);
				if (item) {
					mainLayout->removeItem(item);
					mainLayout->addItem(item, row,
							    column - 1);
				}
			}
		}
	}
}

bool AudioControl::HasSliders()
{
	int columns = mainLayout->columnCount();
	for (int column = 1; column < columns; column++) {
		QLayoutItem *item = mainLayout->itemAtPosition(1, column);
		if (item && item->widget())
			return true;
	}
	return false;
}

void AudioControl::AddFilter(obs_source_t *filter)
{
	QString filterName = QT_UTF8(obs_source_get_name(filter));
	int columns = mainLayout->columnCount();
	for (int column = 2; column < columns; column++) {
		QLayoutItem *item = mainLayout->itemAtPosition(1, column);
		if (!item)
			continue;
		QWidget *w = item->widget();
		if (filterName.localeAwareCompare(w->objectName()) == 0) {
			return;
		}
	}
	if (columns > 2) {
		for (int i = columns - 1; i >= 2; i--) {
			QLayoutItem *item = mainLayout->itemAtPosition(1, i);
			if (!item) {
				addFilterColumn(i, filter);
				return;
			}
		}
		addFilterColumn(columns, filter);
	} else {
		addFilterColumn(2, filter);
	}
}

void AudioControl::addFilterColumn(int column, obs_source_t *filter)
{
	signal_handler_connect(obs_source_get_signal_handler(filter), "rename",
			       OBSFilterRename, this);
	signal_handler_connect(obs_source_get_signal_handler(filter), "updated",
			       OBSFilterUpdated, this);
	signal_handler_connect(obs_source_get_signal_handler(filter), "enable",
			       OBSFilterEnable, this);

	obs_data_t *settings = obs_source_get_settings(filter);
	bool lock = obs_data_get_bool(settings, "locked");
	auto *locked = new LockedCheckBox();
	locked->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);
	locked->setFixedSize(16, 16);
	locked->setChecked(lock);
	locked->setStyleSheet("background: none");

	connect(locked, &QCheckBox::stateChanged, this,
		&AudioControl::LockVolumeControl, Qt::DirectConnection);

	QString filterName = QT_UTF8(obs_source_get_name(filter));
	auto *slider = new SliderIgnoreScroll();
	slider->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
	slider->setEnabled(!lock);
	slider->setMinimum(0);
	slider->setMaximum(10000);
	slider->setObjectName(filterName);
	QString toolTip = QT_UTF8(obs_module_text("Volume"));
	toolTip += " ";
	toolTip += filterName;
	slider->setToolTip(toolTip);
	slider->setValue(obs_data_get_double(settings, "volume") * 100.0);

	if (obs_data_get_bool(settings, "custom_color")) {
		auto color = obs_data_get_int(settings, "color");
		QColor c = QColor(color & 0xff, (color >> 8) & 0xff,
				  (color >> 16) & 0xff);
		slider->setStyleSheet(
			QString("QSlider::handle {background-color: %1;}")
				.arg(c.name()));
	}
	connect(slider, SIGNAL(valueChanged(int)), this,
		SLOT(SliderChanged(int)));

	auto *mute = new MuteCheckBox();
	mute->setEnabled(!lock);
	mute->setChecked(!obs_source_enabled(filter));

	connect(mute, &QCheckBox::stateChanged, this,
		&AudioControl::MuteVolumeControl, Qt::DirectConnection);

	obs_data_release(settings);

	mainLayout->addWidget(locked, lockRow, column, Qt::AlignHCenter);
	mainLayout->addWidget(slider, sliderRow, column, Qt::AlignHCenter);
	mainLayout->addWidget(mute, muteRow, column, Qt::AlignHCenter);
	if (showSliderNames) {
		auto *nameLabel = new QLabel();
		QFont font = nameLabel->font();
		font.setPointSize(font.pointSize() - 1);
		nameLabel->setWordWrap(true);

		nameLabel->setText(filterName);
		nameLabel->setFont(font);
		nameLabel->setAlignment(Qt::AlignCenter);

		mainLayout->addWidget(nameLabel, nameRow, column,
				      Qt::AlignHCenter);
	}
}

void AudioControl::SliderChanged(int vol)
{
	QWidget *w = reinterpret_cast<QWidget *>(sender());
	obs_source_t *s = obs_weak_source_get_source(source);
	if (!s)
		return;
	QLayoutItem *i = mainLayout->itemAtPosition(1, 1);
	if (i && i->widget() == w && !changing_output_volume) {
		float def = (float)vol / 10000.0f;
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
		obs_source_set_volume(s, mul);
		obs_source_release(s);
		return;
	}
	if (changing_monitor_volume) {
		obs_source_release(s);
		return;
	}
	obs_source_t *f = obs_source_get_filter_by_name(
		s, w->objectName().toUtf8().constData());
	obs_source_release(s);
	if (!f)
		return;
	obs_data_t *settings = obs_source_get_settings(f);
	obs_data_set_double(settings, "volume", (double)vol / 100.0);
	obs_data_release(settings);
	obs_source_update(f, nullptr);
	obs_source_release(f);
}
