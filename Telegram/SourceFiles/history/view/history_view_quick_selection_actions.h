/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/object_ptr.h"
#include "base/timer.h"
#include "ui/rp_widget.h"
#include "ui/effects/animations.h"
#include "ui/widgets/shadow.h"

namespace HistoryView {

extern const char kOptionQuickSelectionActions[];

class QuickSelectionActions final : public Ui::RpWidget {
public:
	struct Descriptor {
		Fn<bool()> validate;
		Fn<void()> copyRequested;
		Fn<void()> quoteRequested;
	};
	QuickSelectionActions(not_null<QWidget*> parent, Descriptor descriptor);
	~QuickSelectionActions();

	void requestShow(Qt::MouseButton button, QPoint anchor);
	void handleMouseMove(QPoint point);

	void hideActions();

	[[nodiscard]] bool shown() const;

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	class Button;

	[[nodiscard]] QRect innerRect() const;
	void moveAbove(QPoint point);
	void toggle(bool shown);
	void showNow();
	void paintBubble(QPainter &p);
	void grabForAnimation();
	void animationCallback();

	const Ui::BoxShadow _shadow;
	object_ptr<Button> _copy;
	object_ptr<Button> _quote;
	const Fn<bool()> _validate;
	base::Timer _timer;
	QPoint _anchor;
	QPixmap _cache;
	Ui::Animations::Simple _shownAnimation;
	bool _shown = false;
	bool _grabbing = false;

};

} // namespace HistoryView
