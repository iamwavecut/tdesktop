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
#include "core/mcp/mcp_service.h"
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

#include "base/options.h"
#include "base/qthelp_url.h"
#include "base/weak_ptr.h"
#include "boxes/abstract_box.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/file_utilities.h"
#include "lang/lang_keys.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "menu/menu_item_save_to_markdown.h"
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
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

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
		Fn<bool(QString)> save,
		Fn<bool(QString)> invalid,
		bool masked = false);

	void setInnerFocus() override;

protected:
	void prepare() override;

private:
	const QString _title;
	const QString _placeholder;
	const Fn<QString()> _current;
	const Fn<bool(QString)> _save;
	const Fn<bool(QString)> _invalid;
	const bool _masked = false;
	Fn<void()> _setInnerFocus;
};

TextValueBox::TextValueBox(
	QWidget*,
	QString title,
	QString placeholder,
	Fn<QString()> current,
	Fn<bool(QString)> save,
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
		if (!_save(value)) {
			showError();
			return;
		}
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

struct LinkRewriteGroup {
	std::vector<QString> sources;
	QString target;
};

[[nodiscard]] QString LinkRewriteSourcesText(
		const std::vector<QString> &sources) {
	auto result = QStringList();
	result.reserve(sources.size());
	for (const auto &source : sources) {
		result.push_back(source);
	}
	return result.join(QChar(u'\n'));
}

[[nodiscard]] std::vector<LinkRewriteGroup> GroupLinkRewrites(
		const std::vector<Core::LinkRewriteRule> &rules) {
	auto result = std::vector<LinkRewriteGroup>();
	for (const auto &rule : rules) {
		const auto i = ranges::find(result, rule.targetHost, &LinkRewriteGroup::target);
		if (i == end(result)) {
			result.push_back({ { rule.sourceHost }, rule.targetHost });
		} else if (!ranges::contains(i->sources, rule.sourceHost)) {
			i->sources.push_back(rule.sourceHost);
		}
	}
	return result;
}

class LinkRewriteRulesBox final : public Ui::BoxContent, public base::has_weak_ptr {
public:
	LinkRewriteRulesBox(QWidget*, Fn<void()> saved);

	void setInnerFocus() override;

protected:
	void prepare() override;

private:
	struct GroupState {
		QPointer<Ui::InputField> sources;
		QPointer<Ui::InputField> target;
	};

	[[nodiscard]] std::vector<LinkRewriteGroup> collectGroups() const;
	void rebuildGroups(std::vector<LinkRewriteGroup> groups, int focusIndex);
	void save();

	QPointer<Ui::VerticalLayout> _groupsWrap;
	std::vector<GroupState> _groups;
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

std::vector<LinkRewriteGroup> LinkRewriteRulesBox::collectGroups() const {
	auto result = std::vector<LinkRewriteGroup>();
	result.reserve(_groups.size());
	for (const auto &[sources, target] : _groups) {
		auto values = std::vector<QString>();
		if (sources) {
			for (auto value : sources->getLastText().split(
					QChar(u'\n'),
					Qt::SkipEmptyParts)) {
				values.push_back(value.trimmed());
			}
		}
		result.push_back({
			.sources = std::move(values),
			.target = target ? target->getLastText() : QString(),
		});
	}
	return result;
}

void LinkRewriteRulesBox::rebuildGroups(
		std::vector<LinkRewriteGroup> groups,
		int focusIndex) {
	_groups.clear();
	while (_groupsWrap->count()) {
		delete _groupsWrap->widgetAt(0);
	}
	for (auto i = 0, count = int(groups.size()); i != count; ++i) {
		const auto wrap = _groupsWrap->add(object_ptr<Ui::VerticalLayout>(
			_groupsWrap));
		Ui::AddSubsectionTitle(
			wrap,
			rpl::single(u"Rewrite %1"_q.arg(i + 1)));
		const auto sources = wrap->add(
			object_ptr<Ui::InputField>(
				wrap,
				st::defaultInputField,
				Ui::InputField::Mode::MultiLine,
				rpl::single(u"Source URL prefixes — one per line"_q),
				LinkRewriteSourcesText(groups[i].sources)),
			st::markdownLinkFieldPadding);
		sources->setMinHeight(st::defaultInputField.heightMin * 2);
		sources->setMaxHeight(st::defaultInputField.heightMin * 4);
		const auto target = wrap->add(
			object_ptr<Ui::InputField>(
				wrap,
				st::defaultInputField,
				rpl::single(u"Replacement URL prefix"_q),
				groups[i].target),
			st::markdownLinkFieldPadding);
		AddButtonWithIcon(
			wrap,
			rpl::single(u"Remove rewrite"_q),
			st::settingsAttentionButtonWithIcon,
			{ &st::menuIconDeleteAttention })->setClickedCallback([=] {
			auto current = collectGroups();
			if (i >= 0 && i < int(current.size())) {
				current.erase(begin(current) + i);
			}
			rebuildGroups(std::move(current), std::max(0, i - 1));
		});
		Ui::AddSkip(wrap);
		Ui::AddDivider(wrap);
		Ui::AddSkip(wrap);
		_groups.push_back({ sources, target });
	}
	if (!_groups.empty()) {
		_setInnerFocus = [=] {
			const auto index = std::clamp(focusIndex, 0, int(_groups.size()) - 1);
			if (const auto input = _groups[index].sources.data()) {
				input->setFocusFast();
			}
		};
	} else {
		_setInnerFocus = nullptr;
	}
	_groupsWrap->resizeToWidth(width());
}

void LinkRewriteRulesBox::save() {
	auto raw = collectGroups();
	auto rules = std::vector<Core::LinkRewriteRule>();
	auto sourceErrors = std::vector<int>();
	auto targetErrors = std::vector<int>();
	auto sourceTargets = QHash<QString, QString>();
	for (auto i = 0, count = int(raw.size()); i != count; ++i) {
		auto target = Core::NormalizeLinkRewritePrefix(
			raw[i].target);
		if (raw[i].sources.empty() && raw[i].target.trimmed().isEmpty()) {
			continue;
		}
		if (raw[i].sources.empty()) {
			sourceErrors.push_back(i);
		}
		if (target.isEmpty()) {
			targetErrors.push_back(i);
		}
		for (const auto &value : raw[i].sources) {
			auto source = Core::NormalizeLinkRewritePrefix(value);
			if (source.isEmpty()) {
				sourceErrors.push_back(i);
				continue;
			}
			const auto existing = sourceTargets.constFind(source);
			if (existing != sourceTargets.cend()) {
				if (*existing != target) {
					sourceErrors.push_back(i);
				}
				continue;
			}
			sourceTargets.insert(source, target);
			if (!target.isEmpty()) {
				rules.push_back({ std::move(source), target });
			}
		}
	}
	if (!sourceErrors.empty() || !targetErrors.empty()) {
		for (const auto index : sourceErrors) {
			if (index >= 0 && index < int(_groups.size())) {
				if (const auto input = _groups[index].sources.data()) {
					input->showError();
				}
			}
		}
		for (const auto index : targetErrors) {
			if (index >= 0 && index < int(_groups.size())) {
				if (const auto input = _groups[index].target.data()) {
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
	auto contentOwned = object_ptr<Ui::VerticalLayout>(this);
	const auto content = contentOwned.data();
	Ui::AddDividerText(
		content,
		rpl::single(
			u"Enter hosts or host/path prefixes without http://. "
			u"The unmatched path, query and fragment are preserved."_q));
	_groupsWrap = content->add(object_ptr<Ui::VerticalLayout>(content));
	rebuildGroups(GroupLinkRewrites(
		Core::App().settings().fork().linkRewrites()), 0);
	AddButtonWithIcon(
		content,
		rpl::single(u"Add rewrite"_q),
		st::settingsButtonActive,
		{ &st::settingsIconAdd, IconType::Round, &st::windowBgActive }
	)->setClickedCallback([=] {
		auto groups = collectGroups();
		groups.push_back({});
		rebuildGroups(std::move(groups), _groups.size());
		setInnerFocus();
	});

	setTitle(u"Link rewrites"_q);
	addButton(tr::lng_settings_save(), [=] {
		save();
	});
	addButton(tr::lng_cancel(), [=] {
		closeBox();
	});
	content->resizeToWidth(st::boxWideWidth);
	setInnerWidget(std::move(contentOwned));
	setDimensions(st::boxWideWidth, st::boxMaxListHeight, true);
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

[[nodiscard]] QString SummaryOverviewText() {
	const auto &settings = Core::App().settings().fork();
	return settings.summaryApiBaseUrl().isEmpty()
		|| settings.summaryModel().isEmpty()
		? u"Incomplete"_q
		: u"Configured"_q;
}

class SummarizationSettingsBox final
	: public Ui::BoxContent
	, public base::has_weak_ptr {
public:
	SummarizationSettingsBox(QWidget*, Fn<void()> saved)
	: _saved(std::move(saved)) {
	}

	void setInnerFocus() override {
		if (_baseUrl) {
			_baseUrl->setFocusFast();
		}
	}

protected:
	void prepare() override {
		const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
		const auto &settings = Core::App().settings().fork();
		_baseUrl = content->add(
			object_ptr<Ui::InputField>(
				content,
				st::defaultInputField,
				rpl::single(u"Provider base URL including /v1"_q),
				settings.summaryApiBaseUrl()),
			st::markdownLinkFieldPadding);

		auto passwordWrap = object_ptr<Ui::RpWidget>(content);
		const auto raw = passwordWrap.data();
		_apiKey = Ui::CreateChild<Ui::PasswordInput>(
			raw,
			st::defaultInputField,
			rpl::single(u"API key — optional"_q),
			settings.summaryApiKey());
		raw->resize(raw->width(), _apiKey->height());
		raw->geometryValue(
		) | rpl::on_next([=](const QRect &geometry) {
			_apiKey->resize(geometry.width(), _apiKey->height());
			_apiKey->moveToLeft(0, 0);
			raw->resize(geometry.width(), _apiKey->height());
		}, raw->lifetime());
		content->add(std::move(passwordWrap), st::markdownLinkFieldPadding);

		_model = content->add(
			object_ptr<Ui::InputField>(
				content,
				st::defaultInputField,
				rpl::single(u"Model name"_q),
				settings.summaryModel()),
			st::markdownLinkFieldPadding);
		Ui::AddDividerText(
			content,
			rpl::single(
				u"The API key stays in local Forkgram settings."_q));

		const auto save = [=] {
			const auto baseUrl = _baseUrl->getLastText().trimmed();
			const auto model = _model->getLastText().trimmed();
			if (InvalidSummaryApiBaseUrl(baseUrl)) {
				_baseUrl->showError();
				return;
			} else if (InvalidSummaryModel(model)) {
				_model->showError();
				return;
			}
			const auto weak = base::make_weak(this);
			auto &settings = Core::App().settings().fork();
			settings.setSummaryApiBaseUrl(qthelp::validate_url(baseUrl));
			settings.setSummaryApiKey(_apiKey->getLastText().trimmed());
			settings.setSummaryModel(model);
			Core::App().saveSettings();
			if (_saved) {
				_saved();
			}
			if (weak) {
				closeBox();
			}
		};
		_baseUrl->submits(
		) | rpl::on_next([=] { _apiKey->setFocusFast(); }, _baseUrl->lifetime());
		QObject::connect(
			_apiKey,
			&Ui::MaskedInputField::submitted,
			[=] { _model->setFocusFast(); });
		_model->submits(
		) | rpl::on_next([=] { save(); }, _model->lifetime());

		setTitle(rpl::single(u"Summarization"_q));
		addButton(tr::lng_settings_save(), save);
		addButton(tr::lng_cancel(), [=] { closeBox(); });
		content->moveToLeft(0, 0);
		setDimensionsToContent(st::boxWidth, content);
	}

private:
	QPointer<Ui::InputField> _baseUrl;
	QPointer<Ui::PasswordInput> _apiKey;
	QPointer<Ui::InputField> _model;
	Fn<void()> _saved;
};

[[nodiscard]] QString LinkRewriteRulesLabel() {
	const auto count = GroupLinkRewrites(
		Core::App().settings().fork().linkRewrites()).size();
	if (!count) {
		return u"Disabled"_q;
	}
	return (count == 1)
		? u"1 rewrite"_q
		: u"%1 rewrites"_q.arg(count);
}

//////

class MarkdownClipboardTextBox : public Ui::BoxContent {
public:
	MarkdownClipboardTextBox(QWidget*) {
	}

	void setInnerFocus() override {
		Expects(_setInnerFocus != nullptr);

		_setInnerFocus();
	}

protected:
	void prepare() override;

private:
	Fn<void()> _setInnerFocus;

};

void MarkdownClipboardTextBox::prepare() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);

	content->add(
		object_ptr<Ui::FlatLabel>(
			content,
			tr::lng_settings_markdown_clipboard_text_label(),
			st::boxDividerLabel),
		st::defaultBoxDividerLabelPadding);

	auto &option = base::options::lookup<QString>(
		Menu::kOptionMarkdownClipboardText);

	const auto field = content->add(
		object_ptr<Ui::InputField>(
			content,
			st::defaultInputField,
			Ui::InputField::Mode::MultiLine,
			tr::lng_settings_markdown_clipboard_text_placeholder(),
			option.value()),
		st::markdownLinkFieldPadding);

	const auto submit = [=, &option] {
		option.set(field->getLastText());
		closeBox();
	};

	setTitle(tr::lng_settings_markdown_clipboard_text_box_title());

	addButton(tr::lng_box_ok(), submit);
	addButton(tr::lng_cancel(), [=] { closeBox(); });

	content->moveToLeft(0, 0);
	setDimensionsToContent(st::boxWidth, content);

	_setInnerFocus = [=] {
		field->setFocusFast();
	};
}

//////

[[nodiscard]] rpl::producer<QString> McpTextValue(Fn<QString()> value) {
	return rpl::single(rpl::empty) | rpl::then(
		Core::App().mcp().configurationChanges()
	) | rpl::map([value = std::move(value)] {
		return value();
	});
}

[[nodiscard]] rpl::producer<bool> McpBoolValue(Fn<bool()> value) {
	return rpl::single(rpl::empty) | rpl::then(
		Core::App().mcp().configurationChanges()
	) | rpl::map([value = std::move(value)] {
		return value();
	});
}

[[nodiscard]] QString McpStatusText() {
	const auto &service = Core::App().mcp();
	return !service.enabled()
		? tr::lng_settings_mcp_status_disabled(tr::now)
		: service.available()
		? tr::lng_settings_mcp_status_listening(tr::now)
		: tr::lng_settings_mcp_unavailable(tr::now);
}

[[nodiscard]] QString McpEndpointText() {
	const auto endpoint = Core::App().mcp().endpoint();
	return endpoint.isEmpty()
		? tr::lng_settings_mcp_endpoint_unassigned(tr::now)
		: endpoint;
}

[[nodiscard]] QString McpTokenText() {
	const auto &service = Core::App().mcp();
	if (!service.authenticationEnabled()) {
		return tr::lng_settings_mcp_token_disabled(tr::now);
	}
	const auto token = service.bearerTokenForCopy();
	return token.isEmpty() ? QString() : (u"••••••••"_q + token.right(4));
}

[[nodiscard]] QString McpToolsCountText() {
	const auto &service = Core::App().mcp();
	return u"%1 / %2"_q.arg(
		service.enabledToolCount()
	).arg(
		service.totalToolCount());
}

[[nodiscard]] QString McpCategoryTitle(const QString &category) {
	if (category == u"client"_q) {
		return tr::lng_settings_mcp_category_client(tr::now);
	} else if (category == u"peers"_q) {
		return tr::lng_settings_mcp_category_peers(tr::now);
	} else if (category == u"chats"_q) {
		return tr::lng_settings_mcp_category_chats(tr::now);
	} else if (category == u"messages"_q) {
		return tr::lng_settings_mcp_category_messages(tr::now);
	} else if (category == u"files"_q) {
		return tr::lng_settings_mcp_category_files(tr::now);
	} else if (category == u"contacts"_q) {
		return tr::lng_settings_mcp_category_contacts(tr::now);
	} else if (category == u"members"_q) {
		return tr::lng_settings_mcp_category_members(tr::now);
	} else if (category == u"rights"_q) {
		return tr::lng_settings_mcp_category_rights(tr::now);
	} else if (category == u"settings"_q) {
		return tr::lng_settings_mcp_category_settings(tr::now);
	} else if (category == u"updates"_q) {
		return tr::lng_settings_mcp_category_updates(tr::now);
	} else if (category == u"raw"_q) {
		return tr::lng_settings_mcp_category_raw(tr::now);
	} else if (category == u"ui"_q) {
		return tr::lng_settings_mcp_category_ui(tr::now);
	}
	return category;
}

[[nodiscard]] QString McpCategoryCountText(const QString &category) {
	const auto &service = Core::App().mcp();
	auto enabled = 0;
	auto total = 0;
	for (const auto &tool : service.tools()) {
		if (tool.category == category) {
			++total;
			if (service.toolEnabled(tool.name)) {
				++enabled;
			}
		}
	}
	return u"%1 / %2"_q.arg(enabled).arg(total);
}

class McpToolCategoryBox final : public Ui::BoxContent {
public:
	McpToolCategoryBox(QWidget*, QString category)
	: _category(std::move(category)) {
	}

protected:
	void prepare() override {
		setTitle(McpCategoryTitle(_category));
		addButton(tr::lng_close(), [=] { closeBox(); });

		auto contentOwned = object_ptr<Ui::VerticalLayout>(this);
		const auto content = contentOwned.data();
		const auto service = &Core::App().mcp();
		Ui::AddDividerText(
			content,
			McpTextValue([=] {
				return McpCategoryCountText(_category) + u" tools enabled"_q;
			}));
		const auto all = content->add(object_ptr<Ui::SettingsButton>(
			content,
			tr::lng_settings_mcp_category_all(),
			st::settingsButtonNoIcon));
		all->toggleOn(McpBoolValue([=] {
			return service->categoryState(_category)
				== Core::Mcp::ToolCategoryState::All;
		}));
		all->toggledChanges(
		) | rpl::filter([=](bool enabled) {
			return enabled != (service->categoryState(_category)
				== Core::Mcp::ToolCategoryState::All);
		}) | rpl::on_next([=](bool enabled) {
			service->setCategoryEnabled(_category, enabled);
		}, all->lifetime());
		Ui::AddDivider(content);

		for (const auto &tool : service->tools()) {
			if (tool.category != _category) {
				continue;
			}
			const auto name = tool.name;
			const auto button = content->add(object_ptr<Ui::SettingsButton>(
				content,
				rpl::single(name),
				st::settingsButtonNoIcon));
			button->toggleOn(McpBoolValue([=] {
				return service->toolEnabled(name);
			}));
			button->toggledChanges(
			) | rpl::filter([=](bool enabled) {
				return enabled != service->toolEnabled(name);
			}) | rpl::on_next([=](bool enabled) {
				service->setToolEnabled(name, enabled);
			}, button->lifetime());
		}
		content->resizeToWidth(st::boxWideWidth);
		setInnerWidget(std::move(contentOwned));
		setDimensions(st::boxWideWidth, st::boxMaxListHeight, true);
	}

private:
	const QString _category;
};

class McpToolsBox final : public Ui::BoxContent {
public:
	explicit McpToolsBox(QWidget*) {
	}

protected:
	void prepare() override {
		setTitle(tr::lng_settings_mcp_tools());
		addButton(tr::lng_close(), [=] { closeBox(); });

		auto contentOwned = object_ptr<Ui::VerticalLayout>(this);
		const auto content = contentOwned.data();
		Ui::AddDividerText(
			content,
			rpl::single(
				u"Choose a category to manage its tools. Disable Raw API "
				u"and UI automation to block fallback access."_q));
		for (const auto &category : {
			u"client"_q,
			u"peers"_q,
			u"chats"_q,
			u"messages"_q,
			u"files"_q,
			u"contacts"_q,
			u"members"_q,
			u"rights"_q,
			u"settings"_q,
			u"updates"_q,
			u"raw"_q,
			u"ui"_q,
		}) {
			const auto header = AddButtonWithLabel(
				content,
				rpl::single(McpCategoryTitle(category)),
				McpTextValue([=] {
					return McpCategoryCountText(category) + u"  ›"_q;
				}),
				st::settingsButtonNoIcon);
			header->setClickedCallback([=] {
				uiShow()->showBox(Box<McpToolCategoryBox>(category));
			});
		}
		content->resizeToWidth(st::boxWidth);
		setInnerWidget(std::move(contentOwned));
		setDimensions(st::boxWidth, st::boxMaxListHeight, true);
	}
};

//////

using namespace Builder;

[[nodiscard]] std::optional<quint16> ParseMcpPort(QString value) {
	auto ok = false;
	const auto port = value.trimmed().toInt(&ok);
	return (ok && port >= 1024 && port <= 65535)
		? std::make_optional(quint16(port))
		: std::nullopt;
}

[[nodiscard]] QString McpOverviewText() {
	return McpStatusText() + u" · "_q + McpToolsCountText();
}

[[nodiscard]] QString McpStatusDetails() {
	return tr::lng_settings_mcp_status(tr::now)
		+ u": "_q
		+ McpStatusText()
		+ u"\n"_q
		+ tr::lng_settings_mcp_endpoint(tr::now)
		+ u": "_q
		+ McpEndpointText();
}

class McpSettingsBox final : public Ui::BoxContent {
public:
	explicit McpSettingsBox(QWidget*) {
	}

protected:
	void prepare() override {
		setTitle(tr::lng_settings_mcp_title());
		addButton(tr::lng_close(), [=] { closeBox(); });

		auto contentOwned = object_ptr<Ui::VerticalLayout>(this);
		const auto content = contentOwned.data();
		const auto service = &Core::App().mcp();
		const auto enabledState = std::make_shared<rpl::variable<bool>>(
			service->enabled());
		const auto enabled = content->add(object_ptr<Ui::SettingsButton>(
			content,
			tr::lng_settings_mcp_enable(),
			st::settingsButtonNoIcon));
		enabled->toggleOn(enabledState->value());
		enabled->toggledChanges(
		) | rpl::filter([=](bool value) {
			return value != service->enabled();
		}) | rpl::on_next([=](bool value) {
			if (!service->setEnabled(value)) {
				enabledState->force_assign(service->enabled());
				uiShow()->showBox(Ui::MakeInformBox(
					tr::lng_settings_mcp_bind_failed(
						tr::now,
						lt_error,
						service->errorString())));
			}
		}, enabled->lifetime());
		service->configurationChanges(
		) | rpl::on_next([=] {
			enabledState->force_assign(service->enabled());
		}, enabled->lifetime());

		Ui::AddDividerText(
			content,
			McpTextValue([] { return McpStatusDetails(); }));
		const auto port = AddButtonWithLabel(
			content,
			tr::lng_settings_mcp_port(),
			McpTextValue([] {
				const auto value = Core::App().mcp().configuredPort();
				return value ? QString::number(value) + u"  ›"_q : u"›"_q;
			}),
			st::settingsButton,
			{ &st::menuIconNetwork });
		port->setClickedCallback([=] {
			uiShow()->showBox(Box<TextValueBox>(
				tr::lng_settings_mcp_port(tr::now),
				tr::lng_settings_mcp_port_placeholder(tr::now),
				[] {
					return QString::number(
						Core::App().mcp().configuredPort());
				},
				[](QString value) {
					const auto port = ParseMcpPort(std::move(value));
					return port && Core::App().mcp().rebind(*port);
				},
				[](QString value) {
					return !ParseMcpPort(std::move(value));
				}));
		});
		const auto endpoint = AddButtonWithLabel(
			content,
			tr::lng_settings_mcp_endpoint(),
			McpTextValue([] { return McpEndpointText(); }),
			st::settingsButton,
			{ &st::menuIconCopy });
		endpoint->setClickedCallback([=] {
			if (!service->endpoint().isEmpty()) {
				QGuiApplication::clipboard()->setText(service->endpoint());
			}
		});

		Ui::AddDivider(content);
		const auto auth = content->add(object_ptr<Ui::SettingsButton>(
			content,
			tr::lng_settings_mcp_auth(),
			st::settingsButtonNoIcon));
		auth->toggleOn(McpBoolValue([=] {
			return service->authenticationEnabled();
		}));
		auth->toggledChanges(
		) | rpl::filter([=](bool value) {
			return value != service->authenticationEnabled();
		}) | rpl::on_next([=](bool value) {
			service->setAuthenticationEnabled(value);
		}, auth->lifetime());
		auto authDetailsOwned = object_ptr<Ui::VerticalLayout>(content);
		const auto authDetails = authDetailsOwned.data();
		const auto token = AddButtonWithLabel(
			authDetails,
			tr::lng_settings_mcp_token(),
			McpTextValue([] { return McpTokenText(); }),
			st::settingsButton,
			{ &st::menuIconCopy });
		token->setClickedCallback([=] {
			const auto value = service->bearerTokenForCopy();
			if (service->authenticationEnabled() && !value.isEmpty()) {
				QGuiApplication::clipboard()->setText(value);
			}
		});
		AddButtonWithIcon(
			authDetails,
			tr::lng_settings_mcp_regenerate_token(),
			st::settingsButton,
			{ &st::menuIconSettings }
		)->setClickedCallback([=] {
			uiShow()->showBox(Ui::MakeConfirmBox({
				.text = tr::lng_settings_mcp_regenerate_confirm(tr::now),
				.confirmed = [=] {
					QGuiApplication::clipboard()->setText(
						service->regenerateBearerToken());
				},
				.confirmText = tr::lng_settings_mcp_regenerate_token(tr::now),
			}));
		});
		const auto authDetailsWrap = content->add(
			object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
				content,
				std::move(authDetailsOwned)));
		authDetailsWrap->toggleOn(McpBoolValue([=] {
			return service->authenticationEnabled();
		}), anim::type::instant);

		Ui::AddDivider(content);
		const auto tools = AddButtonWithLabel(
			content,
			tr::lng_settings_mcp_tools(),
			McpTextValue([] { return McpToolsCountText() + u"  ›"_q; }),
			st::settingsButton,
			{ &st::menuIconShowInChat });
		tools->setClickedCallback([=] {
			uiShow()->showBox(Box<McpToolsBox>());
		});

		content->resizeToWidth(st::boxWideWidth);
		setInnerWidget(std::move(contentOwned));
		setDimensions(st::boxWideWidth, st::boxMaxListHeight, true);
	}
};

void BuildForkSectionContent(SectionBuilder &builder) {
	const auto controller = builder.controller();
	struct State {
		rpl::variable<bool> checked;
	};
	struct SummaryLabels {
		rpl::variable<QString> summarization = SummaryOverviewText();
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
		if (!checkbox) {
			return;
		}
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
		if (!checkbox) {
			return;
		}
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
	builder.addButton({
		.id = u"fork/markdown_clipboard_text"_q,
		.title = tr::lng_settings_markdown_clipboard_text(),
		.st = &st::settingsButton,
		.icon = { &st::menuIconExport },
		.onClick = [=] {
			controller->show(Box<MarkdownClipboardTextBox>());
		},
		.keywords = {
			u"markdown"_q,
			u"clipboard"_q,
			u"save"_q,
			u"text"_q,
		},
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
	add(
		u"fork/remember_media_menu"_q,
		{ u"remember"_q, u"media"_q, u"menu"_q },
		tr::lng_settings_remember_media_menu(),
		[] { return Core::App().settings().fork().addToMenuRememberMedia(); },
		[](bool checked) {
			Core::App().settings().fork().setAddToMenuRememberMedia(checked);
		});

	//
	addRestart(
		u"fork/hide_all_chats_tab"_q,
		{ u"hide"_q, u"all_chats"_q, u"tab"_q },
		tr::lng_settings_hide_all_chats_tab(),
		[] { return Core::App().settings().fork().hideAllChatsTab(); },
		[](bool checked) {
			Core::App().settings().fork().setHideAllChatsTab(checked);
		});

	//
	add(
		u"fork/disable_global_search"_q,
		{ u"disable"_q, u"global"_q, u"search"_q },
		tr::lng_settings_disable_global_search(),
		[] { return Core::App().settings().fork().globalSearchDisabled(); },
		[](bool checked) {
			Core::App().settings().fork().setGlobalSearchDisabled(checked);
		});

	//
	add(
		u"fork/forward_and_remove"_q,
		{ u"forward"_q, u"button"_q, u"remove"_q },
		tr::lng_settings_forward_and_remove(),
		[] { return Core::App().settings().fork().thirdButtonTopBar(); },
		[](bool checked) {
			Core::App().settings().fork().setThirdButtonTopBar(checked);
		});

	//
	add(
		u"fork/auto_copy_incoming_login_codes"_q,
		{ u"auto_copy"_q, u"login"_q, u"code"_q },
		tr::lng_settings_auto_copy_login_codes(),
		[] { return Core::App().settings().fork().copyLoginCode(); },
		[](bool checked) {
			Core::App().settings().fork().setCopyLoginCode(checked);
		});

	//
	add(
		u"fork/hide_archived_stories"_q,
		{ u"hide"_q, u"archived"_q, u"stories"_q },
		tr::lng_settings_hide_archived_stories(),
		[] { return Core::App().settings().fork().archivedStoriesAreHidden(); },
		[](bool checked) {
			Core::App().settings().fork().setArchivedStoriesAreHidden(checked);
		});

	//
	add(
		u"fork/hide_from_blocked_users"_q,
		{ u"hide"_q, u"blocked"_q, u"users"_q, u"messages"_q },
		tr::lng_settings_hide_from_blocked_users(),
		[] { return Core::App().settings().fork().hideFromBlockedUsers(); },
		[](bool checked) {
			Core::App().settings().fork().setHideFromBlockedUsers(checked);
		});

	builder.addSkip();
	builder.addDivider();
	builder.addSkip();

	builder.addSubsectionTitle(rpl::single(u"Integrations"_q));
	builder.addButton({
		.id = u"fork/mcp"_q,
		.title = tr::lng_settings_mcp_title(),
		.st = &st::settingsButton,
		.icon = { &st::menuIconNetwork },
		.label = McpTextValue([] { return McpOverviewText() + u"  ›"_q; }),
		.onClick = [=] { controller->show(Box<McpSettingsBox>()); },
		.keywords = {
			u"mcp"_q,
			u"server"_q,
			u"status"_q,
			u"port"_q,
			u"endpoint"_q,
			u"authentication"_q,
			u"token"_q,
			u"tools"_q,
			u"permissions"_q,
		},
	});

	builder.addButton({
		.id = u"fork/summarization"_q,
		.title = rpl::single(u"Summarization"_q),
		.st = &st::settingsButton,
		.icon = { &st::menuIconSettings },
		.label = summaryLabels->summarization.value(
		) | rpl::map([](QString value) { return value + u"  ›"_q; }),
		.onClick = [=] {
			controller->show(Box<SummarizationSettingsBox>([=] {
				summaryLabels->summarization = SummaryOverviewText();
			}));
		},
		.keywords = {
			u"summarization"_q,
			u"summary"_q,
			u"model"_q,
			u"llm"_q,
			u"openai"_q,
		},
	});

	builder.addButton({
		.id = u"fork/link_rewrites/manage"_q,
		.title = rpl::single(u"Link rewrites"_q),
		.st = &st::settingsButton,
		.icon = { &st::menuIconAddress },
		.label = summaryLabels->linkRewrites.value(
		) | rpl::map([](QString value) { return value + u"  ›"_q; }),
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
			u"path"_q,
			u"prefix"_q,
			u"www"_q,
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
		tr::lng_settings_skip_share_from_bot(),
		[] { return Core::App().settings().fork().skipShareFromBot(); },
		[](bool checked) {
			Core::App().settings().fork().setSkipShareFromBot(checked);
		});
	add(
		u"fork/additional_buttons_web_bot"_q,
		{ u"additional"_q, u"button"_q, u"web_bot"_q },
		tr::lng_settings_additional_buttons_web_bot(),
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

const auto kMeta = BuildHelper({
	.id = Fork::Id(),
	.parentId = MainId(),
	.title = &tr::lng_settings_section_fork,
	.icon = &st::menuIconForkSettings,
}, [](SectionBuilder &builder) {
	BuildForkSectionContent(builder);
});

const SectionBuildMethod kForkSection = kMeta.build;

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

	build(content, kForkSection);
	Ui::ResizeFitChild(this, content);
}

} // namespace

Type ForkId() {
	return Fork::Id();
}

namespace Builder {

SectionBuildMethod ForkSection = kForkSection;

} // namespace Builder
} // namespace Settings
