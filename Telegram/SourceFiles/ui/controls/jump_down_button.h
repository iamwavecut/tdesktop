/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/widgets/buttons.h"

namespace Ui {

class InfiniteRadialAnimation;

class JumpDownButton : public RippleButton {
public:
	JumpDownButton(QWidget *parent, const style::TwoIconButton &st);
	~JumpDownButton() override;

	void setUnreadCount(int unreadCount);
	void setLoadingIcons(
		const style::icon *icon,
		const style::icon *iconOver);
	void setActive(bool active);
	void setLoading(bool loading);
	[[nodiscard]] int unreadCount() const {
		return _unreadCount;
	}
	[[nodiscard]] bool active() const {
		return _active;
	}
	[[nodiscard]] bool loading() const;

protected:
	void paintEvent(QPaintEvent *e) override;
	void onStateChanged(State was, StateChangeSource source) override;

	QImage prepareRippleMask() const override;
	QPoint prepareRippleStartPosition() const override;

private:
	void loadingAnimationCallback();

	const style::TwoIconButton &_st;

	int _unreadCount = 0;
	bool _active = false;
	bool _loadingActive = false;
	const style::icon *_loadingIcon = nullptr;
	const style::icon *_loadingIconOver = nullptr;
	std::unique_ptr<InfiniteRadialAnimation> _loading;

};

} // namespace Ui
