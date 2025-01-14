#include "audio-output-control.hpp"

#include <QCheckBox>
#include <QVBoxLayout>
#include "utils.hpp"
#include "obs-module.h"
#include "media-io/audio-math.h"

/* msb(h, g, f, e) lsb(d, c, b, a)   -->  msb(h, h, g, f) lsb(e, d, c, b)
 */
#define SHIFT_RIGHT_2PS(msb, lsb)                                               \
	{                                                                       \
		__m128 tmp = _mm_shuffle_ps(lsb, msb, _MM_SHUFFLE(0, 0, 3, 3)); \
		lsb = _mm_shuffle_ps(lsb, tmp, _MM_SHUFFLE(2, 1, 2, 1));        \
		msb = _mm_shuffle_ps(msb, msb, _MM_SHUFFLE(3, 3, 2, 1));        \
	}

/* x4(d, c, b, a)  -->  max(a, b, c, d)
 */
#define hmax_ps(r, x4)                     \
	do {                               \
		float x4_mem[4];           \
		_mm_storeu_ps(x4_mem, x4); \
		r = x4_mem[0];             \
		r = fmaxf(r, x4_mem[1]);   \
		r = fmaxf(r, x4_mem[2]);   \
		r = fmaxf(r, x4_mem[3]);   \
	} while (false)

/* x(d, c, b, a) --> (|d|, |c|, |b|, |a|)
 */
#define abs_ps(v) _mm_andnot_ps(_mm_set1_ps(-0.f), v)

/* Take cross product of a vector with a matrix resulting in vector.
 */
#define VECTOR_MATRIX_CROSS_PS(out, v, m0, m1, m2, m3)    \
	{                                                 \
		out = _mm_mul_ps(v, m0);                  \
		__m128 mul1 = _mm_mul_ps(v, m1);          \
		__m128 mul2 = _mm_mul_ps(v, m2);          \
		__m128 mul3 = _mm_mul_ps(v, m3);          \
                                                          \
		_MM_TRANSPOSE4_PS(out, mul1, mul2, mul3); \
                                                          \
		out = _mm_add_ps(out, mul1);              \
		out = _mm_add_ps(out, mul2);              \
		out = _mm_add_ps(out, mul3);              \
	}

AudioOutputControl::AudioOutputControl(int track, obs_data_t *settings) : track(track)
{
	int audio_channels = 2;
	struct obs_audio_info audio_info;
	if (obs_get_audio_info(&audio_info)) {
		audio_channels = get_audio_channels(audio_info.speakers);
	}
	volMeter = new VolumeMeter(audio_channels);
	volMeter->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

	mainLayout = new QGridLayout;
	mainLayout->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
	mainLayout->setContentsMargins(0, 0, 0, 0);
	mainLayout->setSpacing(2);
	mainLayout->addWidget(volMeter, 0, 0, -1, 1, Qt::AlignHCenter);

	if (settings) {
		obs_data_array_t *devices = obs_data_get_array(settings, "devices");
		if (devices) {
			const size_t device_count = obs_data_array_count(devices);
			for (size_t i = 0; i < device_count; i++) {
				auto *device = obs_data_array_item(devices, i);
				if (!device)
					continue;
				QString device_id = QT_UTF8(obs_data_get_string(device, "id"));
				auto it = audioDevices.find(device_id);
				if (it == audioDevices.end()) {
					audio_monitor *monitor = audio_monitor_create(QT_TO_UTF8(device_id),
										      obs_data_get_string(device, "deviceName"), 0);
					audio_monitor_set_volume(monitor, 1.0f);
					audio_monitor_start(monitor);
					audioDevices[device_id] = monitor;
				}
				addDeviceColumn((int)i + 1, device_id, QT_UTF8(obs_data_get_string(device, "name")),
						(float)obs_data_get_double(device, "volume"), obs_data_get_bool(device, "muted"),
						obs_data_get_bool(device, "locked"));
				obs_data_release(device);
			}
			obs_data_array_release(devices);
		}
	}

	setLayout(mainLayout);
	audio_output_connect(obs_get_audio(), track, nullptr, OBSOutputAudio, this);
}

AudioOutputControl::~AudioOutputControl()
{
	audio_output_disconnect(obs_get_audio(), track, OBSOutputAudio, this);
}

void AudioOutputControl::OBSOutputAudio(void *param, size_t mix_idx, struct audio_data *data)
{
	UNUSED_PARAMETER(mix_idx);
	if (!data)
		return;
	AudioOutputControl *control = static_cast<AudioOutputControl *>(param);

	audio_t *oa = obs_get_audio();
	if (!oa)
		return;
	size_t planes = audio_output_get_planes(oa);

	size_t nr_samples = data->frames;
	int channel_nr = 0;
	for (size_t plane_nr = 0; plane_nr < planes; plane_nr++) {
		float *samples = (float *)data->data[plane_nr];
		if (!samples) {
			continue;
		}

		/* volmeter->prev_samples may not be aligned to 16 bytes;
		 * use unaligned load. */
		__m128 previous_samples = _mm_loadu_ps(control->prev_samples[channel_nr]);

		/* These are normalized-sinc parameters for interpolating over sample
		* points which are located at x-coords: -1.5, -0.5, +0.5, +1.5.
		* And oversample points at x-coords: -0.3, -0.1, 0.1, 0.3. */
		const __m128 m3 = _mm_set_ps(-0.155915f, 0.935489f, 0.233872f, -0.103943f);
		const __m128 m1 = _mm_set_ps(-0.216236f, 0.756827f, 0.504551f, -0.189207f);
		const __m128 p1 = _mm_set_ps(-0.189207f, 0.504551f, 0.756827f, -0.216236f);
		const __m128 p3 = _mm_set_ps(-0.103943f, 0.233872f, 0.935489f, -0.155915f);

		__m128 work = previous_samples;
		__m128 peak = previous_samples;
		for (size_t i = 0; (i + 3) < nr_samples; i += 4) {
			__m128 new_work = _mm_load_ps(&samples[i]);
			__m128 intrp_samples;

			/* Include the actual sample values in the peak. */
			__m128 abs_new_work = abs_ps(new_work);
			peak = _mm_max_ps(peak, abs_new_work);

			/* Shift in the next point. */
			SHIFT_RIGHT_2PS(new_work, work);
			VECTOR_MATRIX_CROSS_PS(intrp_samples, work, m3, m1, p1, p3);
			peak = _mm_max_ps(peak, abs_ps(intrp_samples));

			SHIFT_RIGHT_2PS(new_work, work);
			VECTOR_MATRIX_CROSS_PS(intrp_samples, work, m3, m1, p1, p3);
			peak = _mm_max_ps(peak, abs_ps(intrp_samples));

			SHIFT_RIGHT_2PS(new_work, work);
			VECTOR_MATRIX_CROSS_PS(intrp_samples, work, m3, m1, p1, p3);
			peak = _mm_max_ps(peak, abs_ps(intrp_samples));

			SHIFT_RIGHT_2PS(new_work, work);
			VECTOR_MATRIX_CROSS_PS(intrp_samples, work, m3, m1, p1, p3);
			peak = _mm_max_ps(peak, abs_ps(intrp_samples));
		}

		float r;
		hmax_ps(r, peak);

		switch (nr_samples) {
		case 0:
			break;
		case 1:
			control->prev_samples[channel_nr][0] = control->prev_samples[channel_nr][1];
			control->prev_samples[channel_nr][1] = control->prev_samples[channel_nr][2];
			control->prev_samples[channel_nr][2] = control->prev_samples[channel_nr][3];
			control->prev_samples[channel_nr][3] = samples[nr_samples - 1];
			break;
		case 2:
			control->prev_samples[channel_nr][0] = control->prev_samples[channel_nr][2];
			control->prev_samples[channel_nr][1] = control->prev_samples[channel_nr][3];
			control->prev_samples[channel_nr][2] = samples[nr_samples - 2];
			control->prev_samples[channel_nr][3] = samples[nr_samples - 1];
			break;
		case 3:
			control->prev_samples[channel_nr][0] = control->prev_samples[channel_nr][3];
			control->prev_samples[channel_nr][1] = samples[nr_samples - 3];
			control->prev_samples[channel_nr][2] = samples[nr_samples - 2];
			control->prev_samples[channel_nr][3] = samples[nr_samples - 1];
			break;
		default:
			control->prev_samples[channel_nr][0] = samples[nr_samples - 4];
			control->prev_samples[channel_nr][1] = samples[nr_samples - 3];
			control->prev_samples[channel_nr][2] = samples[nr_samples - 2];
			control->prev_samples[channel_nr][3] = samples[nr_samples - 1];
		}

		control->peak[channel_nr] = r;

		channel_nr++;
	}

	/* Clear the peak of the channels that have not been handled. */
	for (; channel_nr < MAX_AUDIO_CHANNELS; channel_nr++) {
		control->peak[channel_nr] = 0.0;
	}

	channel_nr = 0;
	for (size_t plane_nr = 0; plane_nr < planes; plane_nr++) {
		float *samples = (float *)data->data[plane_nr];
		if (!samples) {
			continue;
		}

		float sum = 0.0;
		for (size_t i = 0; i < nr_samples; i++) {
			float sample = samples[i];
			sum += sample * sample;
		}
		control->magnitude[channel_nr] = sqrtf(sum / nr_samples);

		channel_nr++;
	}
	float magnitude[MAX_AUDIO_CHANNELS];
	float peak[MAX_AUDIO_CHANNELS];
	float input_peak[MAX_AUDIO_CHANNELS];

	for (channel_nr = 0; channel_nr < MAX_AUDIO_CHANNELS; channel_nr++) {
		magnitude[channel_nr] = mul_to_db(control->magnitude[channel_nr]);
		peak[channel_nr] = mul_to_db(control->peak[channel_nr]);

		/* The input-peak is NOT adjusted with volume, so that the user
		 * can check the input-gain. */
		input_peak[channel_nr] = mul_to_db(control->peak[channel_nr]);
	}
	control->volMeter->setLevels(magnitude, peak, input_peak);

	struct obs_audio_data audio;
	for (size_t i = 0; i < MAX_AV_PLANES; i++) {
		if (i < planes)
			audio.data[i] = data->data[i];
		else
			audio.data[i] = nullptr;
	}
	audio.frames = data->frames;
	audio.timestamp = data->timestamp;

	int columns = control->mainLayout->columnCount();
	auto d = control->audioDevices.begin();
	while (d != control->audioDevices.end()) {
		bool muted = false;
		for (int column = 1; column < columns; column++) {
			auto *item = control->mainLayout->itemAtPosition(control->sliderRow, column);
			if (!item)
				continue;
			if (item->widget()->objectName() == d.key()) {
				item = control->mainLayout->itemAtPosition(control->muteRow, column);
				if (!item)
					continue;
				auto *mute = reinterpret_cast<QCheckBox *>(item->widget());
				if (mute->isChecked())
					muted = true;
				break;
			}
		}
		if (!muted) {
			audio_monitor *monitor = d.value();
			audio_monitor_audio(monitor, &audio);
		}
		++d;
	}
}

void AudioOutputControl::LockVolumeControl(bool lock)
{
	QCheckBox *checkbox = reinterpret_cast<QCheckBox *>(sender());
	int columns = mainLayout->columnCount();
	for (int column = 1; column < columns; column++) {
		QLayoutItem *item = mainLayout->itemAtPosition(lockRow, column);
		if (!item)
			continue;
		if (item->widget() == checkbox) {
			item = mainLayout->itemAtPosition(sliderRow, column);
			item->widget()->setEnabled(!lock);
			item = mainLayout->itemAtPosition(muteRow, column);
			item->widget()->setEnabled(!lock);
			return;
		}
	}
}

void AudioOutputControl::SliderChanged(int vol)
{
	QWidget *w = reinterpret_cast<QWidget *>(sender());
	audio_monitor_set_volume(audioDevices[w->objectName()], (float)vol / 10000.0f);
}

obs_data_t *AudioOutputControl::GetSettings()
{
	auto *data = obs_data_create();
	obs_data_array_t *devices = obs_data_array_create();
	int columns = mainLayout->columnCount();
	for (int column = 1; column < columns; column++) {
		QLayoutItem *item = mainLayout->itemAtPosition(sliderRow, column);
		if (!item)
			continue;
		auto *device = obs_data_create();
		auto *w = reinterpret_cast<QSlider *>(item->widget());
		obs_data_set_string(device, "id", QT_TO_UTF8(w->objectName()));
		obs_data_set_bool(
			device, "locked",
			reinterpret_cast<QCheckBox *>(mainLayout->itemAtPosition(lockRow, column)->widget())->isChecked());
		obs_data_set_bool(
			device, "muted",
			reinterpret_cast<QCheckBox *>(mainLayout->itemAtPosition(muteRow, column)->widget())->isChecked());
		obs_data_set_double(device, "volume", (double)w->value() / 100.0);
		obs_data_set_string(device, "name", QT_TO_UTF8(w->toolTip()));
		obs_data_array_push_back(devices, device);
		obs_data_release(device);
	}
	obs_data_set_array(data, "devices", devices);
	obs_data_array_release(devices);
	return data;
}

bool AudioOutputControl::HasDevice(QString device_id)
{
	if (device_id.isEmpty())
		return false;
	auto it = audioDevices.find(device_id);
	return it != audioDevices.end();
}

void AudioOutputControl::AddDevice(QString device_id, QString device_name)
{
	auto it = audioDevices.find(device_id);
	if (it == audioDevices.end()) {
		audio_monitor *monitor = audio_monitor_create(QT_TO_UTF8(device_id), QT_TO_UTF8(device_name), 0);
		audio_monitor_set_volume(monitor, 1.0f);
		audio_monitor_start(monitor);
		audioDevices[device_id] = monitor;
	}

	int columns = mainLayout->columnCount();
	for (int column = 1; column < columns; column++) {
		QLayoutItem *item = mainLayout->itemAtPosition(1, column);
		if (!item)
			continue;
		QWidget *w = item->widget();
		if (device_id.localeAwareCompare(w->objectName()) == 0) {
			return;
		}
	}
	if (columns > 1) {
		for (int i = columns - 1; i >= 1; i--) {
			QLayoutItem *item = mainLayout->itemAtPosition(1, i);
			if (!item) {
				addDeviceColumn(i, device_id, device_name);
				return;
			}
		}
		addDeviceColumn(columns, device_id, device_name);
	} else {
		addDeviceColumn(1, device_id, device_name);
	}
}

void AudioOutputControl::addDeviceColumn(int column, QString device_id, QString deviceName, float volume, bool muted, bool lock)
{

	auto *locked = new LockedCheckBox();
	locked->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);
	locked->setFixedSize(16, 16);
	locked->setStyleSheet("background: none");
	locked->setChecked(lock);
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
	connect(locked, &QCheckBox::checkStateChanged, this, &AudioOutputControl::LockVolumeControl, Qt::DirectConnection);
#else
	connect(locked, &QCheckBox::stateChanged, this, &AudioOutputControl::LockVolumeControl, Qt::DirectConnection);
#endif

	auto *slider = new SliderIgnoreScroll();
	slider->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
	slider->setMinimum(0);
	slider->setMaximum(10000);
	slider->setObjectName(device_id);
	slider->setToolTip(deviceName);
	slider->setValue(volume * 100.0f);
	slider->setEnabled(!lock);

	connect(slider, SIGNAL(valueChanged(int)), this, SLOT(SliderChanged(int)));

	auto *mute = new MuteCheckBox();
	mute->setChecked(muted);
	mute->setEnabled(!lock);

	mainLayout->addWidget(locked, lockRow, column, Qt::AlignHCenter);
	mainLayout->addWidget(slider, sliderRow, column, Qt::AlignHCenter);
	mainLayout->addWidget(mute, muteRow, column, Qt::AlignHCenter);
}

void AudioOutputControl::RemoveDevice(QString device_id)
{
	const auto it = audioDevices.find(device_id);
	if (it != audioDevices.end()) {
		auto *monitor = it.value();
		audio_monitor_destroy(monitor);
		audioDevices.remove(device_id);
	}
	const auto columns = mainLayout->columnCount();
	auto found = false;
	for (auto column = 1; column < columns; column++) {
		auto *item_slider = mainLayout->itemAtPosition(sliderRow, column);
		if (!item_slider)
			continue;
		auto *widget = item_slider->widget();
		if (device_id.localeAwareCompare(widget->objectName()) == 0) {
			found = true;
			const auto rows = mainLayout->rowCount();
			for (auto row = 0; row < rows; row++) {
				auto *item = mainLayout->itemAtPosition(row, column);
				if (item) {
					auto *w = item->widget();
					mainLayout->removeItem(item);
					delete w;
					delete item;
				}
			}
		} else if (found) {
			const auto rows = mainLayout->rowCount();
			for (auto row = 0; row < rows; row++) {
				auto *item = mainLayout->itemAtPosition(row, column);
				if (item) {
					mainLayout->removeItem(item);
					mainLayout->addItem(item, row, column - 1, 1, 1, Qt::AlignHCenter);
				}
			}
		}
	}
}

void AudioOutputControl::Reset() {
	for (auto d = audioDevices.begin(); d != audioDevices.end(); d++) {
		audio_monitor_stop(d.value());
		audio_monitor_start(d.value());
	}
}
