/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/sections/settings_fork.h"

#include "api/api_authorizations.h"
#include "apiwrap.h"
#include "base/timer.h"
#include "calls/calls_call.h"
#include "calls/calls_instance.h"
#include "calls/calls_video_bubble.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "platform/platform_specific.h"
#include "settings/sections/settings_main.h"
#include "settings/settings_builder.h"
#include "settings/settings_common_session.h"
#include "tgcalls/VideoCaptureInterface.h"
#include "ui/boxes/confirm_box.h"
#include "ui/boxes/single_choice_box.h"
#include "ui/effects/animations.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/continuous_sliders.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/level_meter.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "webrtc/webrtc_audio_input_tester.h"
#include "webrtc/webrtc_create_adm.h"
#include "webrtc/webrtc_environment.h"
#include "webrtc/webrtc_video_track.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

#include "base/qthelp_url.h"
#include "base/weak_ptr.h"
#include "boxes/abstract_box.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/file_utilities.h"
#include "lang/lang_keys.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "settings/settings_common.h"
#include "storage/localstorage.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "ui/boxes/confirm_box.h"
#include "ui/text/text_utilities.h"
#include "ui/vertical_list.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/fields/password_input.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_menu_icons.h"

#include <QtCore/QUrl>
#include <QtCore/QPointer>

#include <algorithm>

namespace Settings {

namespace {
using langString = tr::phrase<>;
using SessionController = not_null<Window::SessionController*>;

class SettingBox : public Ui::BoxContent, public base::has_weak_ptr  {
public:
	explicit SettingBox(
		QWidget*,
		Fn<void(bool)> callback,
		langString title,
		langString info);

	void setInnerFocus() override;

protected:
	void prepare() override;

	virtual QString getOrSetGlobal(QString value) = 0;
	virtual bool isInvalidUrl(QString linkUrl) = 0;

	Fn<void(bool)> _callback;
	Fn<void()> _setInnerFocus;
	langString _info;
	langString _title;
};

SettingBox::SettingBox(
	QWidget*,
	Fn<void(bool)> callback,
	langString title,
	langString info)
: _callback(std::move(callback))
, _info(info)
, _title(title) {
	Expects(_callback != nullptr);
}

void SettingBox::setInnerFocus() {
	Expects(_setInnerFocus != nullptr);

	_setInnerFocus();
}

void SettingBox::prepare() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);

	const auto url = content->add(
		object_ptr<Ui::InputField>(
			content,
			st::defaultInputField,
			_info(),
			getOrSetGlobal(QString())),
		st::markdownLinkFieldPadding);

	const auto submit = [=] {
		const auto linkUrl = url->getLastText();
		const auto isInvalid = isInvalidUrl(linkUrl);
		if (isInvalid) {
			url->showError();
			return;
		}
		const auto weak = base::make_weak(this);
		getOrSetGlobal(linkUrl);
		Core::App().saveSettings();
		_callback(!isInvalid);
		if (weak) {
			closeBox();
		}
	};

	url->submits(
	) | rpl::on_next([=] {
		submit();
	}, lifetime());

	setTitle(_title());

	addButton(tr::lng_box_ok(), submit);
	addButton(tr::lng_cancel(), [=] {
		_callback(!getOrSetGlobal(QString()).isEmpty());
		closeBox();
	});

	content->resizeToWidth(st::boxWidth);
	content->moveToLeft(0, 0);
	setDimensions(st::boxWidth, content->height());

	_setInnerFocus = [=] {
		url->setFocusFast();
	};
}

//////

class SearchEngineBox : public SettingBox {

	using SettingBox::SettingBox;

protected:
	QString getOrSetGlobal(QString value) override;
	bool isInvalidUrl(QString linkUrl) override;
};

QString SearchEngineBox::getOrSetGlobal(QString value) {
	if (value.isEmpty()) {
		return Core::App().settings().fork().searchEngineUrl();
	}
	Core::App().settings().fork().setSearchEngineUrl(value);
	return QString();
}

bool SearchEngineBox::isInvalidUrl(QString linkUrl) {
	linkUrl = qthelp::validate_url(linkUrl);
	return linkUrl.isEmpty() || linkUrl.indexOf("%q") == -1;
}


//////

class URISchemeBox : public SettingBox {

	using SettingBox::SettingBox;

protected:
	QString getOrSetGlobal(QString value) override;
	bool isInvalidUrl(QString linkUrl) override;
};

QString URISchemeBox::getOrSetGlobal(QString value) {
	if (value.isEmpty()) {
		return Core::App().settings().fork().uriScheme();
	}
	Core::App().settings().fork().setUriScheme(value);
	return QString();
}

bool URISchemeBox::isInvalidUrl(QString linkUrl) {
	return linkUrl.indexOf("://") < 2;
}

//////

class StickerSizeBox : public SettingBox {
	using SettingBox::SettingBox;

protected:
	QString getOrSetGlobal(QString value) override;
	bool isInvalidUrl(QString linkUrl) override;

private:
	int _startSize = 0;
};

QString StickerSizeBox::getOrSetGlobal(QString value) {
	if (value.isEmpty()) {
		if (!_startSize) {
			_startSize = Core::App().settings().fork().customStickerSize();
		} else if (_startSize
				== Core::App().settings().fork().customStickerSize()) {
			return QString();
		}
		return QString::number(
			Core::App().settings().fork().customStickerSize());
	}
	if (const auto number = value.toInt()) {
		Core::App().settings().fork().setCustomStickerSize(number);
	}
	return QString();
}

bool StickerSizeBox::isInvalidUrl(QString linkUrl) {
	const auto number = linkUrl.toInt();
	return !number || number < 50 || number > 256;
}

class TextValueBox final : public Ui::BoxContent, public base::has_weak_ptr {
public:
	TextValueBox(
		QWidget*,
		QString title,
		QString placeholder,
		Fn<QString()> current,
		Fn<void(QString)> save,
		Fn<bool(QString)> invalid,
		bool masked = false);

	void setInnerFocus() override;

protected:
	void prepare() override;

private:
	const QString _title;
	const QString _placeholder;
	const Fn<QString()> _current;
	const Fn<void(QString)> _save;
	const Fn<bool(QString)> _invalid;
	const bool _masked = false;
	Fn<void()> _setInnerFocus;
};

TextValueBox::TextValueBox(
	QWidget*,
	QString title,
	QString placeholder,
	Fn<QString()> current,
	Fn<void(QString)> save,
	Fn<bool(QString)> invalid,
	bool masked)
: _title(std::move(title))
, _placeholder(std::move(placeholder))
, _current(std::move(current))
, _save(std::move(save))
, _invalid(std::move(invalid))
, _masked(masked) {
}

void TextValueBox::setInnerFocus() {
	Expects(_setInnerFocus != nullptr);

	_setInnerFocus();
}

void TextValueBox::prepare() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);

	Fn<QString()> readText;
	Fn<void()> showError;
	Fn<void(Fn<void()>)> bindSubmit;
	if (_masked) {
		auto wrap = object_ptr<Ui::RpWidget>(content);
		const auto raw = wrap.data();
		const auto password = Ui::CreateChild<Ui::PasswordInput>(
			raw,
			st::defaultInputField,
			rpl::single(_placeholder),
			_current());
		raw->resize(raw->width(), password->height());
		raw->geometryValue(
		) | rpl::on_next([=](const QRect &geometry) {
			password->resize(geometry.width(), password->height());
			password->moveToLeft(0, 0);
			raw->resize(geometry.width(), password->height());
		}, raw->lifetime());
		content->add(std::move(wrap), st::markdownLinkFieldPadding);
		readText = [=]() -> QString {
			return password->getLastText();
		};
		showError = [=] {
			password->showError();
		};
		bindSubmit = [=](Fn<void()> submit) {
			QObject::connect(
				password,
				&Ui::MaskedInputField::submitted,
				[=](Qt::KeyboardModifiers) {
					submit();
				});
		};
		_setInnerFocus = [=] {
			password->setFocusFast();
		};
	} else {
		const auto input = content->add(
			object_ptr<Ui::InputField>(
				content,
				st::defaultInputField,
				rpl::single(_placeholder),
				_current()),
			st::markdownLinkFieldPadding);
		readText = [=]() -> QString {
			return input->getLastText();
		};
		showError = [=] {
			input->showError();
		};
		bindSubmit = [=](Fn<void()> submit) {
			input->submits(
			) | rpl::on_next([=](Qt::KeyboardModifiers) {
				submit();
			}, input->lifetime());
		};
		_setInnerFocus = [=] {
			input->setFocusFast();
		};
	}

	const auto submit = [=] {
		const auto value = readText().trimmed();
		if (_invalid(value)) {
			showError();
			return;
		}
		const auto weak = base::make_weak(this);
		_save(value);
		Core::App().saveSettings();
		if (weak) {
			closeBox();
		}
	};
	bindSubmit(submit);

	setTitle(_title);

	addButton(tr::lng_box_ok(), submit);
	addButton(tr::lng_cancel(), [=] {
		closeBox();
	});

	content->resizeToWidth(st::boxWidth);
	content->moveToLeft(0, 0);
	setDimensions(st::boxWidth, content->height());
}

class LinkRewriteRulesBox final : public Ui::BoxContent, public base::has_weak_ptr {
public:
	LinkRewriteRulesBox(QWidget*, Fn<void()> saved);

	void setInnerFocus() override;

protected:
	void prepare() override;

private:
	struct RowState {
		QPointer<Ui::InputField> source;
		QPointer<Ui::InputField> target;
	};

	[[nodiscard]] std::vector<Core::LinkRewriteRule> collectRules() const;
	void rebuildRows(std::vector<Core::LinkRewriteRule> rules, int focusIndex);
	void save();

	QPointer<Ui::VerticalLayout> _rowsWrap;
	std::vector<RowState> _rows;
	Fn<void()> _saved;
	Fn<void()> _setInnerFocus;
};

LinkRewriteRulesBox::LinkRewriteRulesBox(QWidget*, Fn<void()> saved)
: _saved(std::move(saved)) {
}

void LinkRewriteRulesBox::setInnerFocus() {
	if (_setInnerFocus) {
		_setInnerFocus();
	}
}

std::vector<Core::LinkRewriteRule> LinkRewriteRulesBox::collectRules() const {
	auto result = std::vector<Core::LinkRewriteRule>();
	result.reserve(_rows.size());
	for (const auto &[source, target] : _rows) {
		result.push_back({
			source ? source->getLastText() : QString(),
			target ? target->getLastText() : QString(),
		});
	}
	return result;
}

void LinkRewriteRulesBox::rebuildRows(
		std::vector<Core::LinkRewriteRule> rules,
		int focusIndex) {
	_rows.clear();
	while (_rowsWrap->count()) {
		delete _rowsWrap->widgetAt(0);
	}
	for (auto i = 0, count = int(rules.size()); i != count; ++i) {
		const auto wrap = _rowsWrap->add(object_ptr<Ui::VerticalLayout>(
			_rowsWrap));
		const auto source = wrap->add(
			object_ptr<Ui::InputField>(
				wrap,
				st::defaultInputField,
				rpl::single(u"Original host"_q),
				rules[i].sourceHost),
			st::markdownLinkFieldPadding);
		const auto target = wrap->add(
			object_ptr<Ui::InputField>(
				wrap,
				st::defaultInputField,
				rpl::single(u"Replacement host"_q),
				rules[i].targetHost),
			st::markdownLinkFieldPadding);
		AddButtonWithIcon(
			wrap,
			rpl::single(u"Remove rule"_q),
			st::settingsButtonNoIcon,
			{ &st::menuIconDeleteAttention })->setClickedCallback([=] {
			auto current = collectRules();
			if (i >= 0 && i < int(current.size())) {
				current.erase(begin(current) + i);
			}
			rebuildRows(std::move(current), std::max(0, i - 1));
		});
		Ui::AddSkip(wrap);
		Ui::AddDivider(wrap);
		Ui::AddSkip(wrap);
		_rows.push_back({ source, target });
	}
	if (!_rows.empty()) {
		_setInnerFocus = [=] {
			const auto index = std::clamp(focusIndex, 0, int(_rows.size()) - 1);
			if (const auto input = _rows[index].source.data()) {
				input->setFocusFast();
			}
		};
	} else {
		_setInnerFocus = nullptr;
	}
	_rowsWrap->resizeToWidth(width());
}

void LinkRewriteRulesBox::save() {
	auto raw = collectRules();
	auto rules = std::vector<Core::LinkRewriteRule>();
	rules.reserve(raw.size());
	auto sourceErrors = std::vector<int>();
	auto targetErrors = std::vector<int>();
	for (auto i = 0, count = int(raw.size()); i != count; ++i) {
		auto source = Core::ForkSettings::NormalizeLinkRewriteHost(
			raw[i].sourceHost);
		auto target = Core::ForkSettings::NormalizeLinkRewriteHost(
			raw[i].targetHost);
		const auto emptySource = raw[i].sourceHost.trimmed().isEmpty();
		const auto emptyTarget = raw[i].targetHost.trimmed().isEmpty();
		if (emptySource && emptyTarget) {
			continue;
		}
		if (source.isEmpty()) {
			sourceErrors.push_back(i);
		}
		if (target.isEmpty()) {
			targetErrors.push_back(i);
		}
		if (!source.isEmpty() && !target.isEmpty()) {
			rules.push_back({ std::move(source), std::move(target) });
		}
	}
	if (!sourceErrors.empty() || !targetErrors.empty()) {
		for (const auto index : sourceErrors) {
			if (index >= 0 && index < int(_rows.size())) {
				if (const auto input = _rows[index].source.data()) {
					input->showError();
				}
			}
		}
		for (const auto index : targetErrors) {
			if (index >= 0 && index < int(_rows.size())) {
				if (const auto input = _rows[index].target.data()) {
					input->showError();
				}
			}
		}
		return;
	}
	Core::App().settings().fork().setLinkRewrites(std::move(rules));
	Core::App().saveSettings();
	if (_saved) {
		_saved();
	}
	closeBox();
}

void LinkRewriteRulesBox::prepare() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	_rowsWrap = content->add(object_ptr<Ui::VerticalLayout>(content));
	rebuildRows(Core::App().settings().fork().linkRewrites(), 0);
	AddButtonWithIcon(
		content,
		rpl::single(u"Add rule"_q),
		st::settingsButtonNoIcon,
		{ &st::settingsIconAdd, IconType::Round, &st::windowBgActive }
	)->setClickedCallback([=] {
		auto rules = collectRules();
		rules.push_back({});
		rebuildRows(std::move(rules), _rows.size());
		setInnerFocus();
	});

	setTitle(u"Link rewrites"_q);
	addButton(tr::lng_settings_save(), [=] {
		save();
	});
	addButton(tr::lng_cancel(), [=] {
		closeBox();
	});
	setDimensionsToContent(st::boxWideWidth, content);
}

[[nodiscard]] bool InvalidSummaryApiBaseUrl(QString value) {
	const auto validated = qthelp::validate_url(value.trimmed());
	if (validated.isEmpty()) {
		return true;
	}
	const auto url = QUrl(validated);
	return !url.isValid()
		|| url.scheme().isEmpty()
		|| !url.path().contains(u"/v1"_q);
}

[[nodiscard]] bool InvalidSummaryModel(QString value) {
	return value.trimmed().isEmpty();
}

[[nodiscard]] QString SummaryApiBaseUrlLabel() {
	const auto value = Core::App().settings().fork().summaryApiBaseUrl();
	return value.isEmpty() ? u"Not set"_q : value;
}

[[nodiscard]] QString SummaryApiKeyLabel() {
	return Core::App().settings().fork().summaryApiKey().isEmpty()
		? u"Optional"_q
		: u"Configured"_q;
}

[[nodiscard]] QString SummaryModelLabel() {
	const auto value = Core::App().settings().fork().summaryModel();
	return value.isEmpty() ? u"Not set"_q : value;
}

[[nodiscard]] QString LinkRewriteRulesLabel() {
	const auto count = Core::App().settings().fork().linkRewrites().size();
	if (!count) {
		return u"Disabled"_q;
	}
	return (count == 1)
		? u"1 rule"_q
		: u"%1 rules"_q.arg(count);
}

//////

using namespace Builder;

void BuildForkSectionContent(SectionBuilder &builder) {
	const auto controller = builder.controller();
	if (!controller) {
		return;
	}
	struct State {
		rpl::variable<bool> checked;
	};
	struct SummaryLabels {
		rpl::variable<QString> baseUrl = SummaryApiBaseUrlLabel();
		rpl::variable<QString> apiKey = SummaryApiKeyLabel();
		rpl::variable<QString> model = SummaryModelLabel();
		rpl::variable<QString> linkRewrites = LinkRewriteRulesLabel();
	};
	const auto summaryLabels = std::make_shared<SummaryLabels>();

	const auto add = [&](
			auto id,
			QStringList keywords,
			auto title,
			auto checkedCallback,
			auto ok) {
		const auto checkbox = builder.addButton({
			.id = std::move(id),
			.title = std::move(title),
			.st = &st::settingsButtonNoIcon,
			.toggled = rpl::single(checkedCallback()),
			.keywords = std::move(keywords),
		});
		checkbox->toggledValue(
		) | rpl::filter([=](bool checked) {
			return (checked != checkedCallback());
		}) | rpl::on_next([=](bool checked) {
			ok(checked);
			Core::App().saveSettings();
		}, checkbox->lifetime());
	};

	const auto restartBox = [=](Fn<void()> ok, Fn<void()> cancel) {
		controller->show(
			Ui::MakeConfirmBox({
				.text = tr::lng_settings_need_restart(tr::now),
				.confirmed = [=] {
					ok();
					Core::App().saveSettings();
					Core::Restart();
				},
				.cancelled = [=](Fn<void()> &&close) {
					cancel();
					close();
				},
				.confirmText = tr::lng_settings_restart_now(tr::now)
			}),
			Ui::LayerOption::KeepOther);
	};
	const auto addWithBox = [&](
			auto id,
			QStringList keywords,
			auto title,
			auto checkedCallback,
			auto ok,
			auto customBox) {
		const auto state = std::make_shared<State>();
		const auto checkbox = builder.addButton({
			.id = std::move(id),
			.title = std::move(title),
			.st = &st::settingsButtonNoIcon,
			.toggled = rpl::single(
				checkedCallback()
			) | rpl::then(state->checked.changes()),
			.keywords = std::move(keywords),
		});
		checkbox->toggledValue(
		) | rpl::filter([=](bool checked) {
			return (checked != checkedCallback());
		}) | rpl::on_next([=](bool checked) {
			customBox(checked, state.get());
		}, checkbox->lifetime());
	};
	const auto addRestart = [&](
			auto id,
			QStringList keywords,
			auto title,
			auto checkedCallback,
			auto ok) {
		addWithBox(
			std::move(id),
			std::move(keywords),
			std::move(title),
			std::move(checkedCallback),
			std::move(ok),
			[=](bool checked, State *state) {
				restartBox(
					[=] { ok(checked); },
					[=] { state->checked.force_assign(!checked); });
			});
	};

	//
	addRestart(
		u"fork/square_avatars"_q,
		{ u"square"_q, u"avatars"_q, u"userpic"_q, u"circle"_q },
		tr::lng_settings_square_avatats(),
		[] { return Core::App().settings().fork().squareUserpics(); },
		[=](bool checked) {
			Core::App().settings().fork().setSquareUserpics(checked);
		});

	//
	add(
		u"fork/audio_fade"_q,
		{ u"audio"_q, u"fade"_q },
		tr::lng_settings_audio_fade(),
		[] { return Core::App().settings().fork().audioFade(); },
		[=](bool checked) {
			Core::App().settings().fork().setAudioFade(checked);
		});

	//
	addWithBox(
		u"fork/uri_scheme"_q,
		{ u"URI"_q, u"scheme"_q, u"custom link"_q },
		tr::lng_settings_uri_scheme(),
		[] { return Core::App().settings().fork().askUriScheme(); },
		[=](bool checked) {
			Core::App().settings().fork().setAskUriScheme(checked);
		},
		[=](bool checked, State *state) {
			const auto callback = [=](bool isSuccess) {
				if (isSuccess) {
					Core::App().settings().fork().setAskUriScheme(isSuccess);
					Core::App().saveSettings();
				} else {
					state->checked.force_assign(false);
				}
			};
			if (!checked) {
				Core::App().settings().fork().setAskUriScheme(false);
				Core::App().saveSettings();
				return;
			}
			controller->show(
				Box<URISchemeBox>(
					std::move(callback),
					tr::lng_settings_uri_scheme_box_title,
					tr::lng_settings_uri_scheme_field_label),
				Ui::LayerOption::KeepOther);
		});

	//
	add(
		u"fork/last_seen_in_dialogs"_q,
		{ u"last"_q, u"seen"_q, u"dialogs"_q, u"online"_q },
		tr::lng_settings_last_seen_in_dialogs(),
		[] { return Core::App().settings().fork().lastSeenInDialogs(); },
		[=](bool checked) {
			Core::App().settings().fork().setLastSeenInDialogs(checked);
		});

	//
	addWithBox(
		u"fork/custom_search"_q,
		{ u"custom"_q, u"search"_q, u"engine"_q },
		tr::lng_settings_search_engine(),
		[] { return Core::App().settings().fork().searchEngine(); },
		[=](bool checked) {
			Core::App().settings().fork().setSearchEngine(checked);
		},
		[=](bool checked, State *state) {
			const auto callback = [=](bool isSuccess) {
				if (isSuccess) {
					Core::App().settings().fork().setSearchEngine(isSuccess);
					Core::App().saveSettings();
				} else {
					state->checked.force_assign(false);
				}
			};
			if (!checked) {
				Core::App().settings().fork().setSearchEngine(false);
				Core::App().saveSettings();
				return;
			}
			controller->show(
				Box<SearchEngineBox>(
					std::move(callback),
					tr::lng_settings_search_engine_box_title,
					tr::lng_settings_search_engine_field_label),
				Ui::LayerOption::KeepOther);
		});

	//
	add(
		u"fork/mention_by_name"_q,
		{ u"mention"_q, u"by"_q, u"name"_q },
		tr::lng_settings_mention_by_name(),
		[] { return Core::App().settings().fork().mentionByNameDisabled(); },
		[=](bool checked) {
			Core::App().settings().fork().setMentionByNameDisabled(checked);
		});

	//
	add(
		u"fork/all_recent_stickers"_q,
		{ u"all"_q, u"recent"_q, u"stickers"_q },
		tr::lng_settings_show_all_recent_stickers(),
		[] { return Core::App().settings().fork().allRecentStickers(); },
		[=](bool checked) {
			Core::App().settings().fork().setAllRecentStickers(checked);
		});

#ifndef Q_OS_LINUX
#ifdef Q_OS_WIN
	builder.addSkip();
	builder.addDivider();
	builder.addSkip();
	add(
		u"fork/use_black_tray_icon"_q,
		{ u"icon"_q, u"black"_q, u"tray"_q },
		tr::lng_settings_use_black_tray_icon(),
		[] { return Core::App().settings().fork().useBlackTrayIcon(); },
		[](bool checked) {
			Core::App().settings().fork().setUseBlackTrayIcon(checked);
			Core::App().saveSettings();
			Core::App().domain().notifyUnreadBadgeChanged();
		});
#else // !Q_OS_WIN
	builder.addSkip();
	builder.addDivider();
	builder.addSkip();
	addRestart(
		u"fork/use_black_tray_icon"_q,
		{ u"icon"_q, u"black"_q, u"tray"_q },
		tr::lng_settings_use_black_tray_icon(),
		[] { return Core::App().settings().fork().useBlackTrayIcon(); },
		[](bool checked) {
			Core::App().settings().fork().setUseBlackTrayIcon(checked);
		});
#endif // Q_OS_WIN

	addRestart(
		u"fork/use_original_tray_icon"_q,
		{ u"icon"_q, u"original"_q, u"tray"_q },
		tr::lng_settings_use_original_tray_icon(),
		[] { return Core::App().settings().fork().useOriginalTrayIcon(); },
		[](bool checked) {
			Core::App().settings().fork().setUseOriginalTrayIcon(checked);
		});
#endif // !Q_OS_LINUX
	builder.addSkip();
	builder.addDivider();
	builder.addSkip();

	//
	builder.addButton({
		.id = u"fork/custom_sticker_size"_q,
		.title = tr::lng_settings_custom_sticker_size(),
		.st = &st::settingsButton,
		.icon = { &st::menuIconStickers },
		.label = rpl::single(QString::number(Core::App().settings().fork().customStickerSize())),
		.onClick = [=] {
			controller->show(
				Box<StickerSizeBox>(
					[=](bool isSuccess) {
						if (isSuccess) {
							restartBox([] {}, [] {});
						}
					},
					tr::lng_settings_custom_sticker_size,
					tr::lng_settings_sticker_size_label));
		},
		.keywords = { u"custom"_q, u"sticker"_q, u"size"_q },
	});

	//
	add(
		u"fork/auto_submit_passcode"_q,
		{ u"auto"_q, u"submit"_q, u"passcode"_q },
		tr::lng_settings_auto_submit_passcode(),
		[] { return Core::App().settings().fork().autoSubmitPasscode(); },
		[](bool checked) {
			Core::App().settings().fork().setAutoSubmitPasscode(checked);
			Core::App().saveSettings();
		});

	//
	addRestart(
		u"fork/emoji_on_click"_q,
		{ u"emoji"_q, u"click"_q, u"panel"_q },
		tr::lng_settings_emoji_on_click(),
		[] { return Core::App().settings().fork().emojiPopupOnClick(); },
		[](bool checked) {
			Core::App().settings().fork().setEmojiPopupOnClick(checked);
		});

	//
	addRestart(
		u"fork/primary_unmuted"_q,
		{ u"primary"_q, u"unmuted"_q, u"dialogs"_q },
		tr::lng_settings_primary_unmuted(),
		[] { return Core::App().settings().fork().primaryUnmutedMessages(); },
		[](bool checked) {
			Core::App().settings().fork().setPrimaryUnmutedMessages(checked);
		});

	//
	add(
		u"fork/remember_media_menu"_q,
		{ u"remember"_q, u"media"_q, u"menu"_q },
		rpl::single(u"Add 'Remember' to menu for media"_q),
		[] { return Core::App().settings().fork().addToMenuRememberMedia(); },
		[](bool checked) {
			Core::App().settings().fork().setAddToMenuRememberMedia(checked);
		});

	//
	addRestart(
		u"fork/hide_all_chats_tab"_q,
		{ u"hide"_q, u"all_chats"_q, u"tab"_q },
		rpl::single(u"Hide 'All Chats' tab"_q),
		[] { return Core::App().settings().fork().hideAllChatsTab(); },
		[](bool checked) {
			Core::App().settings().fork().setHideAllChatsTab(checked);
		});

	//
	add(
		u"fork/disable_global_search"_q,
		{ u"disable"_q, u"global"_q, u"search"_q },
		rpl::single(u"Disable global search"_q),
		[] { return Core::App().settings().fork().globalSearchDisabled(); },
		[](bool checked) {
			Core::App().settings().fork().setGlobalSearchDisabled(checked);
		});

	//
	add(
		u"fork/forward_and_remove"_q,
		{ u"forward"_q, u"button"_q, u"remove"_q },
		rpl::single(u"Button to forward and remove"_q),
		[] { return Core::App().settings().fork().thirdButtonTopBar(); },
		[](bool checked) {
			Core::App().settings().fork().setThirdButtonTopBar(checked);
		});

	//
	add(
		u"fork/auto_copy_incoming_login_codes"_q,
		{ u"auto_copy"_q, u"login"_q, u"code"_q },
		rpl::single(u"Auto-copy incoming login codes"_q),
		[] { return Core::App().settings().fork().copyLoginCode(); },
		[](bool checked) {
			Core::App().settings().fork().setCopyLoginCode(checked);
		});

	//
	add(
		u"fork/hide_archived_stories"_q,
		{ u"hide"_q, u"archived"_q, u"stories"_q },
		rpl::single(u"Hide archived stories"_q),
		[] { return Core::App().settings().fork().archivedStoriesAreHidden(); },
		[](bool checked) {
			Core::App().settings().fork().setArchivedStoriesAreHidden(checked);
		});

	builder.addSkip();
	builder.addDivider();
	builder.addSkip();

	builder.addSubsectionTitle(rpl::single(u"Summarization"_q));
	builder.addButton({
		.id = u"fork/summarization/base_url"_q,
		.title = rpl::single(u"Provider base URL"_q),
		.st = &st::settingsButton,
		.icon = { &st::menuIconNetwork },
		.label = summaryLabels->baseUrl.value(),
		.onClick = [=] {
			controller->show(Box<TextValueBox>(
				u"Summarization provider"_q,
				u"Base URL including /v1"_q,
				[] { return Core::App().settings().fork().summaryApiBaseUrl(); },
				[=](QString value) {
					const auto validated = qthelp::validate_url(value.trimmed());
					Core::App().settings().fork().setSummaryApiBaseUrl(validated);
					summaryLabels->baseUrl = SummaryApiBaseUrlLabel();
				},
				[](QString value) { return InvalidSummaryApiBaseUrl(value); }));
		},
		.keywords = {
			u"summarization"_q,
			u"summary"_q,
			u"provider"_q,
			u"base"_q,
			u"url"_q,
			u"v1"_q,
		},
	});
	builder.addButton({
		.id = u"fork/summarization/api_key"_q,
		.title = rpl::single(u"API key"_q),
		.st = &st::settingsButton,
		.icon = { &st::menuIconLock },
		.label = summaryLabels->apiKey.value(),
		.onClick = [=] {
			controller->show(Box<TextValueBox>(
				u"Summarization API key"_q,
				u"Optional"_q,
				[] { return Core::App().settings().fork().summaryApiKey(); },
				[=](QString value) {
					Core::App().settings().fork().setSummaryApiKey(
						value.trimmed());
					summaryLabels->apiKey = SummaryApiKeyLabel();
				},
				[](QString) { return false; },
				true));
		},
		.keywords = {
			u"summarization"_q,
			u"summary"_q,
			u"api"_q,
			u"key"_q,
			u"token"_q,
		},
	});
	builder.addButton({
		.id = u"fork/summarization/model"_q,
		.title = rpl::single(u"Model"_q),
		.st = &st::settingsButton,
		.icon = { &st::menuIconSettings },
		.label = summaryLabels->model.value(),
		.onClick = [=] {
			controller->show(Box<TextValueBox>(
				u"Summarization model"_q,
				u"Model name"_q,
				[] { return Core::App().settings().fork().summaryModel(); },
				[=](QString value) {
					Core::App().settings().fork().setSummaryModel(
						value.trimmed());
					summaryLabels->model = SummaryModelLabel();
				},
				[](QString value) { return InvalidSummaryModel(value); }));
		},
		.keywords = {
			u"summarization"_q,
			u"summary"_q,
			u"model"_q,
			u"llm"_q,
			u"openai"_q,
		},
	});

	builder.addSkip();
	builder.addDivider();
	builder.addSkip();

	builder.addSubsectionTitle(rpl::single(u"Link rewrites"_q));
	builder.addButton({
		.id = u"fork/link_rewrites/manage"_q,
		.title = rpl::single(u"Manage rewrite rules"_q),
		.st = &st::settingsButton,
		.icon = { &st::menuIconAddress },
		.label = summaryLabels->linkRewrites.value(),
		.onClick = [=] {
			controller->show(Box<LinkRewriteRulesBox>([=] {
				summaryLabels->linkRewrites = LinkRewriteRulesLabel();
			}));
		},
		.keywords = {
			u"link"_q,
			u"rewrite"_q,
			u"rule"_q,
			u"domain"_q,
			u"host"_q,
			u"fixupx"_q,
			u"instagram"_q,
			u"x.com"_q,
		},
	});

	builder.addSkip();
	builder.addDivider();
	builder.addSkip();

	builder.addSubsectionTitle(tr::lng_filters_type_bots());
	//
	add(
		u"fork/skip_share_from_bot"_q,
		{ u"skip"_q, u"share"_q, u"bot"_q },
		rpl::single(u"Skip share box from app bots"_q),
		[] { return Core::App().settings().fork().skipShareFromBot(); },
		[](bool checked) {
			Core::App().settings().fork().setSkipShareFromBot(checked);
		});
	add(
		u"fork/additional_buttons_web_bot"_q,
		{ u"additional"_q, u"button"_q, u"web_bot"_q },
		rpl::single(u"Display additional buttons for app bots"_q),
		[] { return Core::App().settings().fork().additionalButtonsWebBot(); },
		[](bool checked) {
			Core::App().settings().fork().setAdditionalButtonsWebBot(checked);
		});

	builder.addSkip();
	builder.addDivider();
	builder.addSkip();
}

class Fork : public Section<Fork> {
public:
	Fork(QWidget *parent, not_null<Window::SessionController*> controller);
	~Fork();

	[[nodiscard]] rpl::producer<QString> title() override;
	void sectionSaveChanges(FnMut<void()> done) override;

private:
	void setupContent();

};

Fork::Fork(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	setupContent();
}

Fork::~Fork() = default;

rpl::producer<QString> Fork::title() {
	return tr::lng_settings_section_fork();
}

void Fork::sectionSaveChanges(FnMut<void()> done) {
	done();
}

void Fork::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);

	const SectionBuildMethod buildMethod = [](
			not_null<Ui::VerticalLayout*> container,
			not_null<Window::SessionController*> controller,
			Fn<void(Type)> showOther,
			rpl::producer<> showFinished) {
		const auto isPaused = Window::PausedIn(
			controller,
			Window::GifPauseReason::Layer);
		auto builder = SectionBuilder(WidgetContext{
			.container = container,
			.controller = controller,
			.showOther = std::move(showOther),
			.isPaused = isPaused,
		});
		BuildForkSectionContent(builder);
	};

	build(content, buildMethod);
	Ui::ResizeFitChild(this, content);
}

const auto kMeta = BuildHelper({
	.id = Fork::Id(),
	.parentId = MainId(),
	.title = &tr::lng_settings_section_fork,
	.icon = &st::menuIconForkSettings,
}, [](SectionBuilder &builder) {
	BuildForkSectionContent(builder);
});

} // namespace

Type ForkId() {
	return Fork::Id();
}

namespace Builder {

SectionBuildMethod ForkSection = kMeta.build;

} // namespace Builder
} // namespace Settings
