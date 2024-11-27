#include "utils.hpp"

#include <qevent.h>

LockedCheckBox::LockedCheckBox()
{
	setProperty("lockCheckBox", true);
	setProperty("class", "indicator-lock");
}

LockedCheckBox::LockedCheckBox(QWidget *parent) : QCheckBox(parent)
{
	setProperty("lockCheckBox", true);
	setProperty("class", "indicator-lock");
}

MuteCheckBox::MuteCheckBox()
{
	setProperty("muteCheckBox", true);
	setProperty("class", "indicator-mute");
}

MuteCheckBox::MuteCheckBox(QWidget *parent) : QCheckBox(parent)
{
	setProperty("muteCheckBox", true);
	setProperty("class", "indicator-mute");
}

SliderIgnoreScroll::SliderIgnoreScroll(QWidget *parent) : QSlider(parent)
{
	setFocusPolicy(Qt::StrongFocus);
}

SliderIgnoreScroll::SliderIgnoreScroll(Qt::Orientation orientation, QWidget *parent) : QSlider(parent)
{
	setFocusPolicy(Qt::StrongFocus);
	setOrientation(orientation);
}

void SliderIgnoreScroll::wheelEvent(QWheelEvent *event)
{
	if (!hasFocus())
		event->ignore();
	else
		QSlider::wheelEvent(event);
}
