/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/view/history_view_quick_selection_actions.h"

#include "base/options.h"
#include "lang/lang_keys.h"
#include "ui/effects/ripple_animation.h"
#include "ui/painter.h"
#include "ui/rect.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/tooltip.h"
#include "styles/style_chat.h"
#include "styles/style_widgets.h"

namespace HistoryView {
namespace {

constexpr auto kFadeDuration = crl::time(120);
constexpr auto kTooltipDelay = crl::time(1000);

base::options::toggle OptionQuickSelectionActions({
	.id = kOptionQuickSelectionActions,
	.name = "Quick selection actions",
	.description = "Show a small bubble with copy and quote buttons "
		"above a selected message text.",
	.defaultValue = true,
});

} // namespace

class QuickSelectionActions::Button final
	: public Ui::RippleButton
	, public Ui::AbstractTooltipShower {
public:
	Button(
		QWidget *parent,
		const style::IconButton &st,
		const style::icon &icon,
		QString tooltip);

	QString tooltipText() const override;
	QPoint tooltipPos() const override;
	bool tooltipWindowActive() const override;

protected:
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

	QImage prepareRippleMask() const override;
	QPoint prepareRippleStartPosition() const override;

private:
	const style::IconButton &_st;
	const style::icon &_icon;
	const QString _tooltip;

};

QuickSelectionActions::Button::Button(
	QWidget *parent,
	const style::IconButton &st,
	const style::icon &icon,
	QString tooltip)
: RippleButton(parent, st.ripple)
, _st(st)
, _icon(icon)
, _tooltip(std::move(tooltip)) {
	resize(_st.width, _st.height);
	setMouseTracking(true);
	setCursor(style::cur_pointer);
}

void QuickSelectionActions::Button::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	paintRipple(p, _st.rippleAreaPosition, nullptr);
	_icon.paintInCenter(p, rect());
}

void QuickSelectionActions::Button::mouseMoveEvent(QMouseEvent *e) {
	RippleButton::mouseMoveEvent(e);
	if (rect().contains(e->pos())) {
		Ui::Tooltip::Show(kTooltipDelay, this);
	} else {
		Ui::Tooltip::Hide();
	}
}

void QuickSelectionActions::Button::leaveEventHook(QEvent *e) {
	RippleButton::leaveEventHook(e);
	Ui::Tooltip::Hide();
}

QString QuickSelectionActions::Button::tooltipText() const {
	return _tooltip;
}

QPoint QuickSelectionActions::Button::tooltipPos() const {
	return QCursor::pos();
}

bool QuickSelectionActions::Button::tooltipWindowActive() const {
	return Ui::AppInFocus() && Ui::InFocusChain(window());
}

QImage QuickSelectionActions::Button::prepareRippleMask() const {
	return Ui::RippleAnimation::EllipseMask(Size(_st.rippleAreaSize));
}

QPoint QuickSelectionActions::Button::prepareRippleStartPosition() const {
	const auto result = mapFromGlobal(QCursor::pos())
		- _st.rippleAreaPosition;
	const auto area = Rect(Size(_st.rippleAreaSize));
	return area.contains(result)
		? result
		: DisabledRippleStartPosition();
}

QuickSelectionActions::QuickSelectionActions(
	not_null<QWidget*> parent,
	Descriptor descriptor)
: RpWidget(parent)
, _shadow(st::defaultBoxShadow)
, _copy(
	this,
	st::historyQuickSelectionButton,
	st::historyQuickSelectionCopyIcon,
	tr::lng_context_copy_selected(tr::now))
, _quote(
	this,
	st::historyQuickSelectionButton,
	st::historyQuickSelectionQuoteIcon,
	tr::lng_context_quote_and_reply(tr::now))
, _validate(std::move(descriptor.validate)) {
	const auto &st = st::historyQuickSelectionButton;
	const auto extend = _shadow.extend();
	const auto inner = QRect(0, 0, st.width * 2, st.height);
	resize(inner.marginsAdded(extend).size());
	_copy->move(extend.left(), extend.top());
	_quote->move(extend.left() + st.width, extend.top());

	_copy->setClickedCallback([=, action = std::move(descriptor.copyRequested)] {
		if (action) {
			action();
		}
		hideActions();
	});
	_quote->setClickedCallback([=, action = std::move(descriptor.quoteRequested)] {
		if (action) {
			action();
		}
		hideActions();
	});
	_timer.setCallback([=] { showNow(); });
	hide();
}

QuickSelectionActions::~QuickSelectionActions() = default;

void QuickSelectionActions::requestShow(Qt::MouseButton button, QPoint anchor) {
	if (button != Qt::LeftButton
		|| !OptionQuickSelectionActions.value()
		|| (_validate && !_validate())) {
		hideActions();
		return;
	}
	_anchor = anchor;
	_timer.callOnce(crl::time(150));
}

void QuickSelectionActions::showNow() {
	if (_validate && !_validate()) {
		return;
	}
	moveAbove(_anchor);
	raise();
	toggle(true);
}

void QuickSelectionActions::handleMouseMove(QPoint point) {
	if (!shown()) {
		return;
	}
	const auto reach = st::historyQuickSelectionButton.height * 2;
	if (!(geometry() + Margins(reach)).contains(point)) {
		hideActions();
	}
}

void QuickSelectionActions::hideActions() {
	_timer.cancel();
	toggle(false);
}

QRect QuickSelectionActions::innerRect() const {
	return QRect(QPoint(), size()).marginsRemoved(_shadow.extend());
}

void QuickSelectionActions::moveAbove(QPoint point) {
	const auto skip = st::historyQuickSelectionSkip;
	auto left = point.x() - width() / 2;
	auto top = point.y() - skip - height() + _shadow.extend().bottom();
	if (const auto parent = parentWidget()) {
		left = std::clamp(left, 0, std::max(parent->width() - width(), 0));
		top = std::max(top, 0);
	}
	move(left, top);
}

bool QuickSelectionActions::shown() const {
	return _shown;
}

void QuickSelectionActions::toggle(bool shown) {
	if (_shown == shown) {
		return;
	}
	_shown = shown;
	if (_shown) {
		show();
	}
	grabForAnimation();
	_shownAnimation.start(
		[=] { animationCallback(); },
		_shown ? 0. : 1.,
		_shown ? 1. : 0.,
		kFadeDuration);
}

void QuickSelectionActions::grabForAnimation() {
	_cache = QPixmap();
	_copy->show();
	_quote->show();
	_grabbing = true;
	_cache = Ui::GrabWidget(this);
	_grabbing = false;
	_copy->hide();
	_quote->hide();
}

void QuickSelectionActions::animationCallback() {
	update();
	if (_shownAnimation.animating()) {
		return;
	}
	_cache = QPixmap();
	if (_shown) {
		_copy->show();
		_quote->show();
		update();
	} else {
		hide();
	}
}

void QuickSelectionActions::paintBubble(QPainter &p) {
	const auto inner = innerRect();
	const auto radius = inner.height() / 2;
	_shadow.paint(p, inner, radius);

	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(st::windowBg);
	p.drawRoundedRect(inner, radius, radius);
}

void QuickSelectionActions::paintEvent(QPaintEvent *e) {
	Painter p(this);

	if (_grabbing) {
		paintBubble(p);
		return;
	}
	const auto opacity = _shownAnimation.value(_shown ? 1. : 0.);
	if (opacity <= 0.) {
		return;
	}
	p.setOpacity(opacity);
	if (!_cache.isNull()) {
		p.drawPixmap(0, 0, _cache);
		return;
	}
	paintBubble(p);
}

const char kOptionQuickSelectionActions[] = "quick-selection-actions";

} // namespace HistoryView
