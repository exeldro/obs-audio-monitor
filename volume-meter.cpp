#include "volume-meter.hpp"

#include "../../../libobs/util/platform.h"

#define CLAMP(x, min, max) ((x) < (min) ? (min) : ((x) > (max) ? (max) : (x)))

QWeakPointer<VolumeMeterTimer> VolumeMeter::updateTimer;

QColor VolumeMeter::getBackgroundNominalColor() const
{
	return backgroundNominalColor;
}

void VolumeMeter::setBackgroundNominalColor(QColor c)
{
	backgroundNominalColor = std::move(c);
}

QColor VolumeMeter::getBackgroundWarningColor() const
{
	return backgroundWarningColor;
}

void VolumeMeter::setBackgroundWarningColor(QColor c)
{
	backgroundWarningColor = std::move(c);
}

QColor VolumeMeter::getBackgroundErrorColor() const
{
	return backgroundErrorColor;
}

void VolumeMeter::setBackgroundErrorColor(QColor c)
{
	backgroundErrorColor = std::move(c);
}

QColor VolumeMeter::getForegroundNominalColor() const
{
	return foregroundNominalColor;
}

void VolumeMeter::setForegroundNominalColor(QColor c)
{
	foregroundNominalColor = std::move(c);
}

QColor VolumeMeter::getForegroundWarningColor() const
{
	return foregroundWarningColor;
}

void VolumeMeter::setForegroundWarningColor(QColor c)
{
	foregroundWarningColor = std::move(c);
}

QColor VolumeMeter::getForegroundErrorColor() const
{
	return foregroundErrorColor;
}

void VolumeMeter::setForegroundErrorColor(QColor c)
{
	foregroundErrorColor = std::move(c);
}

QColor VolumeMeter::getClipColor() const
{
	return clipColor;
}

void VolumeMeter::setClipColor(QColor c)
{
	clipColor = std::move(c);
}

QColor VolumeMeter::getMagnitudeColor() const
{
	return magnitudeColor;
}

void VolumeMeter::setMagnitudeColor(QColor c)
{
	magnitudeColor = std::move(c);
}

QColor VolumeMeter::getMajorTickColor() const
{
	return majorTickColor;
}

void VolumeMeter::setMajorTickColor(QColor c)
{
	majorTickColor = std::move(c);
}

QColor VolumeMeter::getMinorTickColor() const
{
	return minorTickColor;
}

void VolumeMeter::setMinorTickColor(QColor c)
{
	minorTickColor = std::move(c);
}

qreal VolumeMeter::getMinimumLevel() const
{
	return minimumLevel;
}

void VolumeMeter::setMinimumLevel(qreal v)
{
	minimumLevel = v;
}

qreal VolumeMeter::getWarningLevel() const
{
	return warningLevel;
}

void VolumeMeter::setWarningLevel(qreal v)
{
	warningLevel = v;
}

qreal VolumeMeter::getErrorLevel() const
{
	return errorLevel;
}

void VolumeMeter::setErrorLevel(qreal v)
{
	errorLevel = v;
}

qreal VolumeMeter::getClipLevel() const
{
	return clipLevel;
}

void VolumeMeter::setClipLevel(qreal v)
{
	clipLevel = v;
}

qreal VolumeMeter::getMinimumInputLevel() const
{
	return minimumInputLevel;
}

void VolumeMeter::setMinimumInputLevel(qreal v)
{
	minimumInputLevel = v;
}

qreal VolumeMeter::getPeakDecayRate() const
{
	return peakDecayRate;
}

void VolumeMeter::setPeakDecayRate(qreal v)
{
	peakDecayRate = v;
}

qreal VolumeMeter::getMagnitudeIntegrationTime() const
{
	return magnitudeIntegrationTime;
}

void VolumeMeter::setMagnitudeIntegrationTime(qreal v)
{
	magnitudeIntegrationTime = v;
}

qreal VolumeMeter::getPeakHoldDuration() const
{
	return peakHoldDuration;
}

void VolumeMeter::setPeakHoldDuration(qreal v)
{
	peakHoldDuration = v;
}

qreal VolumeMeter::getInputPeakHoldDuration() const
{
	return inputPeakHoldDuration;
}

void VolumeMeter::setInputPeakHoldDuration(qreal v)
{
	inputPeakHoldDuration = v;
}

void VolumeMeter::setPeakMeterType(enum obs_peak_meter_type peakMeterType)
{
	if (obs_volmeter)
		obs_volmeter_set_peak_meter_type(obs_volmeter, peakMeterType);
	switch (peakMeterType) {
	case TRUE_PEAK_METER:
		// For true-peak meters EBU has defined the Permitted Maximum,
		// taking into account the accuracy of the meter and further
		// processing required by lossy audio compression.
		//
		// The alignment level was not specified, but I've adjusted
		// it compared to a sample-peak meter. Incidentally Youtube
		// uses this new Alignment Level as the maximum integrated
		// loudness of a video.
		//
		//  * Permitted Maximum Level (PML) = -2.0 dBTP
		//  * Alignment Level (AL) = -13 dBTP
		setErrorLevel(-2.0);
		setWarningLevel(-13.0);
		break;

	case SAMPLE_PEAK_METER:
	default:
		// For a sample Peak Meter EBU has the following level
		// definitions, taking into account inaccuracies of this meter:
		//
		//  * Permitted Maximum Level (PML) = -9.0 dBFS
		//  * Alignment Level (AL) = -20.0 dBFS
		setErrorLevel(-9.0);
		setWarningLevel(-20.0);
		break;
	}
}

void VolumeMeter::mousePressEvent(QMouseEvent *event)
{
	setFocus(Qt::MouseFocusReason);
	event->accept();
}

void VolumeMeter::wheelEvent(QWheelEvent *event)
{
	auto *proxy = focusProxy();
	if (proxy)
		QApplication::sendEvent(proxy, event);
}

VolumeMeter::VolumeMeter(QWidget *parent, obs_volmeter_t *obs_volmeter)
	: QWidget(parent), obs_volmeter(obs_volmeter)
{
	setAttribute(Qt::WA_OpaquePaintEvent, true);

	// Use a font that can be rendered small.
	tickFont = QFont("Arial");
	tickFont.setPixelSize(7);
	// Default meter color settings, they only show if
	// there is no stylesheet, do not remove.
	backgroundNominalColor.setRgb(0x26, 0x7f, 0x26); // Dark green
	backgroundWarningColor.setRgb(0x7f, 0x7f, 0x26); // Dark yellow
	backgroundErrorColor.setRgb(0x7f, 0x26, 0x26);   // Dark red
	foregroundNominalColor.setRgb(0x4c, 0xff, 0x4c); // Bright green
	foregroundWarningColor.setRgb(0xff, 0xff, 0x4c); // Bright yellow
	foregroundErrorColor.setRgb(0xff, 0x4c, 0x4c);   // Bright red
	clipColor.setRgb(0xff, 0xff, 0xff);              // Bright white
	magnitudeColor.setRgb(0x00, 0x00, 0x00);         // Black
	majorTickColor.setRgb(0xff, 0xff, 0xff);         // Black
	minorTickColor.setRgb(0xcc, 0xcc, 0xcc);         // Black
	minimumLevel = -60.0;                            // -60 dB
	warningLevel = -20.0;                            // -20 dB
	errorLevel = -9.0;                               //  -9 dB
	clipLevel = -0.5;                                //  -0.5 dB
	minimumInputLevel = -50.0;                       // -50 dB
	peakDecayRate = 11.76;                           //  20 dB / 1.7 sec
	magnitudeIntegrationTime = 0.3;                  //  99% in 300 ms
	peakHoldDuration = 20.0;                         //  20 seconds
	inputPeakHoldDuration = 1.0;                     //  1 second

	channels = (int)audio_output_get_channels(obs_get_audio());

	handleChannelCofigurationChange();
	updateTimerRef = updateTimer.toStrongRef();
	if (!updateTimerRef) {
		updateTimerRef = QSharedPointer<VolumeMeterTimer>::create();
		updateTimerRef->setTimerType(Qt::PreciseTimer);
		updateTimerRef->start(16);
		updateTimer = updateTimerRef;
	}

	updateTimerRef->AddVolControl(this);
}

VolumeMeter::~VolumeMeter()
{
	updateTimerRef->RemoveVolControl(this);
	delete tickPaintCache;
}

void VolumeMeter::setLevels(const float magnitude[MAX_AUDIO_CHANNELS],
			    const float peak[MAX_AUDIO_CHANNELS],
			    const float inputPeak[MAX_AUDIO_CHANNELS])
{
	uint64_t ts = os_gettime_ns();
	QMutexLocker locker(&dataMutex);

	currentLastUpdateTime = ts;
	for (int channelNr = 0; channelNr < MAX_AUDIO_CHANNELS; channelNr++) {
		currentMagnitude[channelNr] = magnitude[channelNr];
		currentPeak[channelNr] = peak[channelNr];
		currentInputPeak[channelNr] = inputPeak[channelNr];
	}

	// In case there are more updates then redraws we must make sure
	// that the ballistics of peak and hold are recalculated.
	locker.unlock();
	calculateBallistics(ts);
}

inline void VolumeMeter::resetLevels()
{
	currentLastUpdateTime = 0;
	for (int channelNr = 0; channelNr < MAX_AUDIO_CHANNELS; channelNr++) {
		currentMagnitude[channelNr] = -M_INFINITE;
		currentPeak[channelNr] = -M_INFINITE;
		currentInputPeak[channelNr] = -M_INFINITE;

		displayMagnitude[channelNr] = -M_INFINITE;
		displayPeak[channelNr] = -M_INFINITE;
		displayPeakHold[channelNr] = -M_INFINITE;
		displayPeakHoldLastUpdateTime[channelNr] = 0;
		displayInputPeakHold[channelNr] = -M_INFINITE;
		displayInputPeakHoldLastUpdateTime[channelNr] = 0;
	}
}

inline void VolumeMeter::handleChannelCofigurationChange()
{
	QMutexLocker locker(&dataMutex);

	int currentNrAudioChannels =
		obs_volmeter ? obs_volmeter_get_nr_channels(obs_volmeter)
			     : audio_output_get_info(obs_get_audio())->speakers;
	if (displayNrAudioChannels != currentNrAudioChannels) {
		displayNrAudioChannels = currentNrAudioChannels;

		// Make room for 3 pixels meter, with one pixel between each.
		// Then 9/13 pixels for ticks and numbers.
		setMinimumSize(displayNrAudioChannels * 4 + 14, 130);

		resetLevels();
	}
}

inline bool VolumeMeter::detectIdle(uint64_t ts)
{
	double timeSinceLastUpdate = (ts - currentLastUpdateTime) * 0.000000001;
	if (timeSinceLastUpdate > 0.5) {
		resetLevels();
		return true;
	} else {
		return false;
	}
}

inline void
VolumeMeter::calculateBallisticsForChannel(int channelNr, uint64_t ts,
					   qreal timeSinceLastRedraw)
{
	const float peak = showOutputMeter ? currentPeak[channelNr]
					   : currentInputPeak[channelNr];
	if (peak >= displayPeak[channelNr] || isnan(displayPeak[channelNr])) {
		// Attack of peak is immediate.
		displayPeak[channelNr] = peak;
	} else {
		// Decay of peak is 40 dB / 1.7 seconds for Fast Profile
		// 20 dB / 1.7 seconds for Medium Profile (Type I PPM)
		// 24 dB / 2.8 seconds for Slow Profile (Type II PPM)
		float decay = float(peakDecayRate * timeSinceLastRedraw);
		displayPeak[channelNr] =
			CLAMP(displayPeak[channelNr] - decay, peak, 0);
	}

	if (peak >= displayPeakHold[channelNr] ||
	    !isfinite(displayPeakHold[channelNr])) {
		// Attack of peak-hold is immediate, but keep track
		// when it was last updated.
		displayPeakHold[channelNr] = peak;
		displayPeakHoldLastUpdateTime[channelNr] = ts;
	} else {
		// The peak and hold falls back to peak
		// after 20 seconds.
		qreal timeSinceLastPeak =
			(uint64_t)(ts -
				   displayPeakHoldLastUpdateTime[channelNr]) *
			0.000000001;
		if (timeSinceLastPeak > peakHoldDuration) {
			displayPeakHold[channelNr] = peak;
			displayPeakHoldLastUpdateTime[channelNr] = ts;
		}
	}

	if (peak >= displayInputPeakHold[channelNr] ||
	    !isfinite(displayInputPeakHold[channelNr])) {
		// Attack of peak-hold is immediate, but keep track
		// when it was last updated.
		displayInputPeakHold[channelNr] = peak;
		displayInputPeakHoldLastUpdateTime[channelNr] = ts;
	} else {
		// The peak and hold falls back to peak after 1 second.
		qreal timeSinceLastPeak =
			(uint64_t)(
				ts -
				displayInputPeakHoldLastUpdateTime[channelNr]) *
			0.000000001;
		if (timeSinceLastPeak > inputPeakHoldDuration) {
			displayInputPeakHold[channelNr] = peak;
			displayInputPeakHoldLastUpdateTime[channelNr] = ts;
		}
	}

	if (!isfinite(displayMagnitude[channelNr])) {
		// The statements in the else-leg do not work with
		// NaN and infinite displayMagnitude.
		displayMagnitude[channelNr] = currentMagnitude[channelNr];
	} else {
		// A VU meter will integrate to the new value to 99% in 300 ms.
		// The calculation here is very simplified and is more accurate
		// with higher frame-rate.
		float attack =
			float((currentMagnitude[channelNr] -
			       displayMagnitude[channelNr]) *
			      (timeSinceLastRedraw / magnitudeIntegrationTime) *
			      0.99);
		displayMagnitude[channelNr] =
			CLAMP(displayMagnitude[channelNr] + attack,
			      (float)minimumLevel, 0);
	}
}

inline void VolumeMeter::calculateBallistics(uint64_t ts,
					     qreal timeSinceLastRedraw)
{
	QMutexLocker locker(&dataMutex);

	for (int channelNr = 0; channelNr < MAX_AUDIO_CHANNELS; channelNr++)
		calculateBallisticsForChannel(channelNr, ts,
					      timeSinceLastRedraw);
}

void VolumeMeter::paintInputMeter(QPainter &painter, int x, int y, int width,
				  int height, float peakHold)
{
	QMutexLocker locker(&dataMutex);
	QColor color;

	if (peakHold < minimumInputLevel)
		color = backgroundNominalColor;
	else if (peakHold < warningLevel)
		color = foregroundNominalColor;
	else if (peakHold < errorLevel)
		color = foregroundWarningColor;
	else if (peakHold <= clipLevel)
		color = foregroundErrorColor;
	else
		color = clipColor;

	painter.fillRect(x, y, width, height, color);
}

void VolumeMeter::paintHTicks(QPainter &painter, int x, int y, int width,
			      int height)
{
	qreal scale = width / minimumLevel;

	painter.setFont(tickFont);
	painter.setPen(majorTickColor);

	// Draw major tick lines and numeric indicators.
	for (int i = 0; i >= minimumLevel; i -= 5) {
		int position = int(x + width - (i * scale) - 1);
		QString str = QString::number(i);

		if (i == 0 || i == -5)
			painter.drawText(position - 3, height, str);
		else
			painter.drawText(position - 5, height, str);
		painter.drawLine(position, y, position, y + 2);
	}

	// Draw minor tick lines.
	painter.setPen(minorTickColor);
	for (int i = 0; i >= minimumLevel; i--) {
		int position = int(x + width - (i * scale) - 1);
		if (i % 5 != 0)
			painter.drawLine(position, y, position, y + 1);
	}
}

void VolumeMeter::paintVTicks(QPainter &painter, int x, int y, int height)
{
	qreal scale = height / minimumLevel;

	painter.setFont(tickFont);
	painter.setPen(majorTickColor);

	// Draw major tick lines and numeric indicators.
	for (int i = 0; i >= minimumLevel; i -= 5) {
		int position = y + int((i * scale) - 1);
		QString str = QString::number(i);

		if (i == 0)
			painter.drawText(x + 5, position + 4, str);
		else if (i == -60)
			painter.drawText(x + 4, position, str);
		else
			painter.drawText(x + 4, position + 2, str);
		painter.drawLine(x, position, x + 2, position);
	}

	// Draw minor tick lines.
	painter.setPen(minorTickColor);
	for (int i = 0; i >= minimumLevel; i--) {
		int position = y + int((i * scale) - 1);
		if (i % 5 != 0)
			painter.drawLine(x, position, x + 1, position);
	}
}

#define CLIP_FLASH_DURATION_MS 1000

void VolumeMeter::ClipEnding()
{
	clipping = false;
}

void VolumeMeter::paintHMeter(QPainter &painter, int x, int y, int width,
			      int height, float magnitude, float peak,
			      float peakHold)
{
	qreal scale = width / minimumLevel;

	QMutexLocker locker(&dataMutex);
	int minimumPosition = x + 0;
	int maximumPosition = x + width;
	int magnitudePosition = int(x + width - (magnitude * scale));
	int peakPosition = int(x + width - (peak * scale));
	int peakHoldPosition = int(x + width - (peakHold * scale));
	int warningPosition = int(x + width - (warningLevel * scale));
	int errorPosition = int(x + width - (errorLevel * scale));

	int nominalLength = warningPosition - minimumPosition;
	int warningLength = errorPosition - warningPosition;
	int errorLength = maximumPosition - errorPosition;
	locker.unlock();

	if (clipping) {
		peakPosition = maximumPosition;
	}

	if (peakPosition < minimumPosition) {
		painter.fillRect(minimumPosition, y, nominalLength, height,
				 backgroundNominalColor);
		painter.fillRect(warningPosition, y, warningLength, height,
				 backgroundWarningColor);
		painter.fillRect(errorPosition, y, errorLength, height,
				 backgroundErrorColor);
	} else if (peakPosition < warningPosition) {
		painter.fillRect(minimumPosition, y,
				 peakPosition - minimumPosition, height,
				 foregroundNominalColor);
		painter.fillRect(peakPosition, y,
				 warningPosition - peakPosition, height,
				 backgroundNominalColor);
		painter.fillRect(warningPosition, y, warningLength, height,
				 backgroundWarningColor);
		painter.fillRect(errorPosition, y, errorLength, height,
				 backgroundErrorColor);
	} else if (peakPosition < errorPosition) {
		painter.fillRect(minimumPosition, y, nominalLength, height,
				 foregroundNominalColor);
		painter.fillRect(warningPosition, y,
				 peakPosition - warningPosition, height,
				 foregroundWarningColor);
		painter.fillRect(peakPosition, y, errorPosition - peakPosition,
				 height, backgroundWarningColor);
		painter.fillRect(errorPosition, y, errorLength, height,
				 backgroundErrorColor);
	} else if (peakPosition < maximumPosition) {
		painter.fillRect(minimumPosition, y, nominalLength, height,
				 foregroundNominalColor);
		painter.fillRect(warningPosition, y, warningLength, height,
				 foregroundWarningColor);
		painter.fillRect(errorPosition, y, peakPosition - errorPosition,
				 height, foregroundErrorColor);
		painter.fillRect(peakPosition, y,
				 maximumPosition - peakPosition, height,
				 backgroundErrorColor);
	} else if (int(magnitude) != 0) {
		if (!clipping) {
			QTimer::singleShot(CLIP_FLASH_DURATION_MS, this,
					   SLOT(ClipEnding()));
			clipping = true;
		}

		int end = errorLength + warningLength + nominalLength;
		painter.fillRect(minimumPosition, y, end, height,
				 QBrush(foregroundErrorColor));
	}

	if (peakHoldPosition - 3 < minimumPosition)
		; // Peak-hold below minimum, no drawing.
	else if (peakHoldPosition < warningPosition)
		painter.fillRect(peakHoldPosition - 3, y, 3, height,
				 foregroundNominalColor);
	else if (peakHoldPosition < errorPosition)
		painter.fillRect(peakHoldPosition - 3, y, 3, height,
				 foregroundWarningColor);
	else
		painter.fillRect(peakHoldPosition - 3, y, 3, height,
				 foregroundErrorColor);

	if (magnitudePosition - 3 >= minimumPosition)
		painter.fillRect(magnitudePosition - 3, y, 3, height,
				 magnitudeColor);
}

void VolumeMeter::paintVMeter(QPainter &painter, int x, int y, int width,
			      int height, float magnitude, float peak,
			      float peakHold)
{
	qreal scale = height / minimumLevel;

	QMutexLocker locker(&dataMutex);
	int minimumPosition = y + 0;
	int maximumPosition = y + height;
	int magnitudePosition = int(y + height - (magnitude * scale));
	int peakPosition = int(y + height - (peak * scale));
	int peakHoldPosition = int(y + height - (peakHold * scale));
	int warningPosition = int(y + height - (warningLevel * scale));
	int errorPosition = int(y + height - (errorLevel * scale));

	int nominalLength = warningPosition - minimumPosition;
	int warningLength = errorPosition - warningPosition;
	int errorLength = maximumPosition - errorPosition;
	locker.unlock();

	if (clipping) {
		peakPosition = maximumPosition;
	}

	if (peakPosition < minimumPosition) {
		painter.fillRect(x, minimumPosition, width, nominalLength,
				 backgroundNominalColor);
		painter.fillRect(x, warningPosition, width, warningLength,
				 backgroundWarningColor);
		painter.fillRect(x, errorPosition, width, errorLength,
				 backgroundErrorColor);
	} else if (peakPosition < warningPosition) {
		painter.fillRect(x, minimumPosition, width,
				 peakPosition - minimumPosition,
				 foregroundNominalColor);
		painter.fillRect(x, peakPosition, width,
				 warningPosition - peakPosition,
				 backgroundNominalColor);
		painter.fillRect(x, warningPosition, width, warningLength,
				 backgroundWarningColor);
		painter.fillRect(x, errorPosition, width, errorLength,
				 backgroundErrorColor);
	} else if (peakPosition < errorPosition) {
		painter.fillRect(x, minimumPosition, width, nominalLength,
				 foregroundNominalColor);
		painter.fillRect(x, warningPosition, width,
				 peakPosition - warningPosition,
				 foregroundWarningColor);
		painter.fillRect(x, peakPosition, width,
				 errorPosition - peakPosition,
				 backgroundWarningColor);
		painter.fillRect(x, errorPosition, width, errorLength,
				 backgroundErrorColor);
	} else if (peakPosition < maximumPosition) {
		painter.fillRect(x, minimumPosition, width, nominalLength,
				 foregroundNominalColor);
		painter.fillRect(x, warningPosition, width, warningLength,
				 foregroundWarningColor);
		painter.fillRect(x, errorPosition, width,
				 peakPosition - errorPosition,
				 foregroundErrorColor);
		painter.fillRect(x, peakPosition, width,
				 maximumPosition - peakPosition,
				 backgroundErrorColor);
	} else {
		if (!clipping) {
			QTimer::singleShot(CLIP_FLASH_DURATION_MS, this,
					   SLOT(ClipEnding()));
			clipping = true;
		}

		int end = errorLength + warningLength + nominalLength;
		painter.fillRect(x, minimumPosition, width, end,
				 QBrush(foregroundErrorColor));
	}

	if (peakHoldPosition - 3 < minimumPosition)
		; // Peak-hold below minimum, no drawing.
	else if (peakHoldPosition < warningPosition)
		painter.fillRect(x, peakHoldPosition - 3, width, 3,
				 foregroundNominalColor);
	else if (peakHoldPosition < errorPosition)
		painter.fillRect(x, peakHoldPosition - 3, width, 3,
				 foregroundWarningColor);
	else
		painter.fillRect(x, peakHoldPosition - 3, width, 3,
				 foregroundErrorColor);

	if (magnitudePosition - 3 >= minimumPosition)
		painter.fillRect(x, magnitudePosition - 3, width, 3,
				 magnitudeColor);
}

void VolumeMeter::ShowOutputMeter(bool output)
{
	showOutputMeter = output;
}

void VolumeMeter::paintEvent(QPaintEvent *event)
{
	uint64_t ts = os_gettime_ns();
	qreal timeSinceLastRedraw = (ts - lastRedrawTime) * 0.000000001;

	const QRect rect = event->region().boundingRect();
	int width = rect.width();
	int height = rect.height();

	handleChannelCofigurationChange();
	calculateBallistics(ts, timeSinceLastRedraw);
	bool idle = detectIdle(ts);

	// Draw the ticks in a off-screen buffer when the widget changes size.
	QSize tickPaintCacheSize = QSize(14, height);

	if (tickPaintCache == nullptr ||
	    tickPaintCache->size() != tickPaintCacheSize) {
		delete tickPaintCache;
		tickPaintCache = new QPixmap(tickPaintCacheSize);

		QColor clearColor(0, 0, 0, 0);
		tickPaintCache->fill(clearColor);

		QPainter tickPainter(tickPaintCache);

		tickPainter.translate(0, height);
		tickPainter.scale(1, -1);
		paintVTicks(tickPainter, 0, 11,
			    tickPaintCacheSize.height() - 11);

		tickPainter.end();
	}

	// Actual painting of the widget starts here.
	QPainter painter(this);

	// Paint window background color (as widget is opaque)
	QColor background = palette().color(QPalette::ColorRole::Window);
	painter.fillRect(rect, background);

	// Invert the Y axis to ease the math
	painter.translate(0, height);
	painter.scale(1, -1);
	painter.drawPixmap(displayNrAudioChannels * 4 - 1, 7, *tickPaintCache);

	for (int channelNr = 0; channelNr < displayNrAudioChannels;
	     channelNr++) {

		int channelNrFixed =
			(displayNrAudioChannels == 1 && channels > 2)
				? 2
				: channelNr;

		paintVMeter(painter, channelNr * 4, 8, 3, height - 10,
			    displayMagnitude[channelNrFixed],
			    displayPeak[channelNrFixed],
			    displayPeakHold[channelNrFixed]);

		if (idle)
			continue;

		// By not drawing the input meter boxes the user can
		// see that the audio stream has been stopped, without
		// having too much visual impact.

		paintInputMeter(painter, channelNr * 4, 3, 3, 3,
				displayInputPeakHold[channelNrFixed]);
	}

	lastRedrawTime = ts;
}

void VolumeMeterTimer::AddVolControl(VolumeMeter *meter)
{
	volumeMeters.push_back(meter);
}

void VolumeMeterTimer::RemoveVolControl(VolumeMeter *meter)
{
	volumeMeters.removeOne(meter);
}

void VolumeMeterTimer::timerEvent(QTimerEvent *)
{
	for (VolumeMeter *meter : volumeMeters)
		meter->update();
}
