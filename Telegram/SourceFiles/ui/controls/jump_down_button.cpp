/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ui/controls/jump_down_button.h"

#include "ui/effects/radial_animation.h"
#include "ui/effects/ripple_animation.h"
#include "ui/unread_badge_paint.h"
#include "ui/painter.h"
#include "styles/style_chat_helpers.h"

namespace Ui {

JumpDownButton::JumpDownButton(
	QWidget *parent,
	const style::TwoIconButton &st)
: RippleButton(parent, st.ripple)
, _st(st) {
	resize(_st.width, _st.height);
	setCursor(style::cur_pointer);

	hide();
}

JumpDownButton::~JumpDownButton() = default;

void JumpDownButton::loadingAnimationCallback() {
	if (!anim::Disabled()) {
		update();
	}
}

QImage JumpDownButton::prepareRippleMask() const {
	return Ui::RippleAnimation::EllipseMask(
		QSize(_st.rippleAreaSize, _st.rippleAreaSize));
}

void JumpDownButton::setLoadingIcons(
		const style::icon *icon,
		const style::icon *iconOver) {
	_loadingIcon = icon;
	_loadingIconOver = iconOver;
}

void JumpDownButton::setActive(bool active) {
	if (_active == active) {
		return;
	}
	_active = active;
	update();
}

QPoint JumpDownButton::prepareRippleStartPosition() const {
	return mapFromGlobal(QCursor::pos()) - _st.rippleAreaPosition;
}

void JumpDownButton::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);

	const auto over = isOver();
	const auto down = isDown();
	const auto active = over || down || _active;
	const auto loadingState = _loading
		? _loading->computeState()
		: RadialState{ 0., 0, RadialState::kFull };
	const auto paintLoadingIcons = [&](float64 opacity) {
		if (!_loadingIcon || !_loadingIconOver || opacity <= 0.) {
			return false;
		}
		const auto wasOpacity = p.opacity();
		p.setOpacity(wasOpacity * opacity);
		const auto &icon = active ? *_loadingIconOver : *_loadingIcon;
		icon.paint(p, _st.iconPosition, width());
		p.setOpacity(wasOpacity);
		return true;
	};
	const auto paintIcons = [&](float64 opacity) {
		if (opacity <= 0.) {
			return;
		}
		const auto wasOpacity = p.opacity();
		p.setOpacity(wasOpacity * opacity);
		const auto &below = active ? _st.iconBelowOver : _st.iconBelow;
		const auto &above = active ? _st.iconAboveOver : _st.iconAbove;
		below.paint(p, _st.iconPosition, width());
		above.paint(p, _st.iconPosition, width());
		p.setOpacity(wasOpacity);
	};

	paintIcons(1. - loadingState.shown);
	paintRipple(p, _st.rippleAreaPosition.x(), _st.rippleAreaPosition.y());
	if (_unreadCount > 0) {
		auto unreadString = QString::number(_unreadCount);

		Ui::UnreadBadgeStyle st;
		st.align = style::al_center;
		st.font = st::historyToDownBadgeFont;
		st.size = st::historyToDownBadgeSize;
		st.sizeId = Ui::UnreadBadgeSize::HistoryToDown;
		Ui::PaintUnreadBadge(p, unreadString, width(), 0, st, 4);
	}
	if (loadingState.shown > 0.) {
		if (paintLoadingIcons(loadingState.shown)) {
			return;
		}
		const auto icon = (over || down) ? _st.iconAboveOver : _st.iconAbove;
		auto inner = QRect(
			_st.iconPosition,
			QSize(icon.width(), icon.height()));
		if (inner.width() <= 0 || inner.height() <= 0) {
			inner = rect().marginsRemoved(QMargins(14, 14, 14, 14));
		}
		const auto line = style::ConvertScaleExact(st::historyEmojiCircleLine);
		const auto color = active
			? st::historyEmojiCircleFgOver
			: st::historyEmojiCircleFg;
		if (anim::Disabled() && _loading && _loading->animating()) {
			anim::DrawStaticLoading(p, inner, line, color);
		} else {
			auto pen = color->p;
			pen.setWidthF(line);
			pen.setCapStyle(Qt::RoundCap);
			p.setPen(pen);
			p.setBrush(Qt::NoBrush);

			PainterHighQualityEnabler hq(p);
			if (loadingState.arcLength < RadialState::kFull) {
				p.drawArc(inner, loadingState.arcFrom, loadingState.arcLength);
			} else {
				p.drawEllipse(inner);
			}
		}
	}
}

void JumpDownButton::setUnreadCount(int unreadCount) {
	if (_unreadCount != unreadCount) {
		_unreadCount = unreadCount;
		update();
	}
}

void JumpDownButton::setLoading(bool loading) {
	if (_loadingActive == loading) {
		return;
	}
	_loadingActive = loading;
	if (loading) {
		_loading = std::make_unique<InfiniteRadialAnimation>(
			[=] { loadingAnimationCallback(); },
			st::defaultInfiniteRadialAnimation);
	}
	setEnabled(!loading);
	if (loading) {
		_loading->start(st::defaultInfiniteRadialAnimation.sineDuration);
		update();
	} else if (_loading) {
		_loading->stopWithFade();
		update();
	}
}

bool JumpDownButton::loading() const {
	return _loadingActive;
}

void JumpDownButton::onStateChanged(State was, StateChangeSource source) {
	RippleButton::onStateChanged(was, source);
	const auto wasOver = static_cast<bool>(was & StateFlag::Over);
	if (isOver() != wasOver) {
		update();
	}
}

} // namespace Ui
