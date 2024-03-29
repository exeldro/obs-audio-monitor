#include "utils.hpp"

#include <qevent.h>

LockedCheckBox::LockedCheckBox()
{
	setProperty("lockCheckBox", true);
}

LockedCheckBox::LockedCheckBox(QWidget *parent) : QCheckBox(parent) {
	setProperty("lockCheckBox", true);
}

MuteCheckBox::MuteCheckBox()
{
	setProperty("muteCheckBox", true);
}

MuteCheckBox::MuteCheckBox(QWidget *parent) : QCheckBox(parent)
{
	setProperty("muteCheckBox", true);
}

SliderIgnoreScroll::SliderIgnoreScroll(QWidget *parent) : QSlider(parent)
{
	setFocusPolicy(Qt::StrongFocus);
}

SliderIgnoreScroll::SliderIgnoreScroll(Qt::Orientation orientation,
				       QWidget *parent)
	: QSlider(parent)
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
