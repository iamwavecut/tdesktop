/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/view/history_view_bottom_info.h"

#include "ui/chat/message_bubble.h"
#include "ui/chat/chat_style.h"
#include "ui/click_handler.h"
#include "ui/effects/reaction_fly_animation.h"
#include "ui/layers/generic_box.h"
#include "ui/text/custom_emoji_helper.h"
#include "ui/text/format_values.h"
#include "ui/text/text_options.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/painter.h"
#include "core/ui_integration.h"
#include "lang/lang_keys.h"
#include "history/history_item_components.h"
#include "history/history_item_helpers.h"
#include "history/history_item.h"
#include "history/history.h"
#include "history/view/media/history_view_media.h"
#include "history/view/history_view_message.h"
#include "history/view/history_view_cursor_state.h"
#include "base/unixtime.h"
#include "chat_helpers/emoji_interactions.h"
#include "core/click_handler_types.h"
#include "main/main_session.h"
#include "lottie/lottie_icon.h"
#include "data/data_channel.h"
#include "data/data_session.h"
#include "data/data_message_reactions.h"
#include "window/window_session_controller.h"
#include "styles/style_chat.h"
#include "styles/style_credits.h"
#include "styles/style_dialogs.h"
#include "styles/style_layers.h"

#include <QtGui/QTextCharFormat>
#include <QtGui/QTextLayout>
#include <QtGui/QTextOption>

#include <algorithm>
#include <cmath>

namespace HistoryView {
namespace {

constexpr auto kDeletedMessageOpacity = 0.275;

[[nodiscard]] QString SchedulePeriodText(TimeId period) {
	struct Entry {
		TimeId period = 0;
		QString text;
	};
	const auto map = std::vector<Entry>{
		{ 60, u"minutely"_q },
		{ 300, u"5-minutely"_q },
		{ 24 * 60 * 60, tr::lng_repeated_daily(tr::now) },
		{ 7 * 24 * 60 * 60, tr::lng_repeated_weekly(tr::now) },
		{ 14 * 24 * 60 * 60, tr::lng_repeated_biweekly(tr::now) },
		{ 30 * 24 * 60 * 60, tr::lng_repeated_monthly(tr::now) },
		{
			91 * 24 * 60 * 60,
			tr::lng_repeated_every_month(tr::now, lt_count, 3)
		},
		{
			182 * 24 * 60 * 60,
			tr::lng_repeated_every_month(tr::now, lt_count, 6)
		},
		{ 365 * 24 * 60 * 60, tr::lng_repeated_yearly(tr::now) },
	};
	for (const auto &entry : map) {
		if (entry.period >= period) {
			return entry.text;
		}
	}
	return map.back().text;
}

[[nodiscard]] QString RevisionDate(TimeId date) {
	return date
		? QLocale().toString(base::unixtime::parse(date), QLocale::ShortFormat)
		: QString();
}

[[nodiscard]] QString RevisionDate(
		const HistoryMessageRevisionSnapshot &snapshot) {
	return RevisionDate(snapshot.editDate ? snapshot.editDate : snapshot.date);
}

[[nodiscard]] QString RevisionTitle(int index) {
	return index
		? tr::lng_message_versions_version(
			tr::now,
			lt_version,
			QString::number(index + 1))
		: tr::lng_message_versions_original(tr::now);
}

[[nodiscard]] QString RevisionPreview(
		const HistoryMessageRevisionSnapshot &snapshot) {
	auto result = snapshot.text.simplified();
	if (result.isEmpty()) {
		result = snapshot.media.isEmpty()
			? QString()
			: (u"["_q + snapshot.media + u"]"_q);
	}
	const auto limit = 80;
	return (result.size() > limit)
		? (result.left(limit - 3) + "...")
		: result;
}

[[nodiscard]] QString RevisionContent(
		const HistoryMessageRevisionSnapshot &snapshot) {
	if (!snapshot.text.isEmpty()) {
		return snapshot.text;
	} else if (!snapshot.media.isEmpty()) {
		return u"["_q + snapshot.media + u"]"_q;
	}
	return QString();
}

enum class RevisionDiffKind {
	Plain,
	Added,
	Removed,
};

struct RevisionDiffFragment {
	QString text;
	RevisionDiffKind kind = RevisionDiffKind::Plain;
};

struct RevisionDiffBlock {
	QString title;
	std::vector<RevisionDiffFragment> fragments;
};

void AppendDiffFragment(
		std::vector<RevisionDiffFragment> &fragments,
		QString text,
		RevisionDiffKind kind) {
	if (text.isEmpty()) {
		return;
	}
	if (!fragments.empty() && fragments.back().kind == kind) {
		fragments.back().text += std::move(text);
	} else {
		fragments.push_back({ .text = std::move(text), .kind = kind });
	}
}

[[nodiscard]] std::vector<QString> RevisionDiffTokens(const QString &text) {
	auto result = std::vector<QString>();
	for (auto start = 0; start != text.size();) {
		const auto spaces = text[start].isSpace();
		auto end = start + 1;
		while (end != text.size()
			&& text[end].isSpace() == spaces) {
			++end;
		}
		result.push_back(text.mid(start, end - start));
		start = end;
	}
	return result;
}

[[nodiscard]] std::vector<RevisionDiffFragment> RevisionPrefixSuffixDiff(
		const QString &before,
		const QString &after) {
	auto result = std::vector<RevisionDiffFragment>();
	auto prefix = 0;
	while (prefix != before.size()
		&& prefix != after.size()
		&& before[prefix] == after[prefix]) {
		++prefix;
	}
	auto suffix = 0;
	while (suffix != before.size() - prefix
		&& suffix != after.size() - prefix
		&& before[before.size() - suffix - 1]
			== after[after.size() - suffix - 1]) {
		++suffix;
	}
	AppendDiffFragment(
		result,
		after.left(prefix),
		RevisionDiffKind::Plain);
	AppendDiffFragment(
		result,
		before.mid(prefix, before.size() - prefix - suffix),
		RevisionDiffKind::Removed);
	AppendDiffFragment(
		result,
		after.mid(prefix, after.size() - prefix - suffix),
		RevisionDiffKind::Added);
	AppendDiffFragment(
		result,
		after.right(suffix),
		RevisionDiffKind::Plain);
	return result;
}

[[nodiscard]] std::vector<RevisionDiffFragment> RevisionTextDiff(
		const QString &before,
		const QString &after) {
	constexpr auto kMaxCells = 40000;

	const auto oldTokens = RevisionDiffTokens(before);
	const auto newTokens = RevisionDiffTokens(after);
	if (int64(oldTokens.size()) * int64(newTokens.size()) > kMaxCells) {
		return RevisionPrefixSuffixDiff(before, after);
	}
	const auto oldCount = int(oldTokens.size());
	const auto newCount = int(newTokens.size());
	const auto columns = newCount + 1;
	auto lcs = std::vector<int>((oldCount + 1) * columns);
	const auto at = [&](int oldIndex, int newIndex) -> int& {
		return lcs[oldIndex * columns + newIndex];
	};
	for (auto oldIndex = oldCount; oldIndex-- > 0;) {
		for (auto newIndex = newCount; newIndex-- > 0;) {
			at(oldIndex, newIndex) = (oldTokens[oldIndex] == newTokens[newIndex])
				? (at(oldIndex + 1, newIndex + 1) + 1)
				: std::max(
					at(oldIndex + 1, newIndex),
					at(oldIndex, newIndex + 1));
		}
	}
	auto result = std::vector<RevisionDiffFragment>();
	auto oldIndex = 0;
	auto newIndex = 0;
	while (oldIndex != oldCount || newIndex != newCount) {
		if (oldIndex != oldCount
			&& newIndex != newCount
			&& oldTokens[oldIndex] == newTokens[newIndex]) {
			AppendDiffFragment(
				result,
				oldTokens[oldIndex++],
				RevisionDiffKind::Plain);
			++newIndex;
		} else if (oldIndex != oldCount
			&& (newIndex == newCount
				|| at(oldIndex + 1, newIndex)
					>= at(oldIndex, newIndex + 1))) {
			AppendDiffFragment(
				result,
				oldTokens[oldIndex++],
				RevisionDiffKind::Removed);
		} else {
			AppendDiffFragment(
				result,
				newTokens[newIndex++],
				RevisionDiffKind::Added);
		}
	}
	return result;
}

[[nodiscard]] std::vector<RevisionDiffFragment> RevisionValueDiff(
		const QString &before,
		const QString &after) {
	auto result = std::vector<RevisionDiffFragment>();
	AppendDiffFragment(
		result,
		u"- "_q + (before.isEmpty() ? QString("-") : before),
		RevisionDiffKind::Removed);
	AppendDiffFragment(result, "\n", RevisionDiffKind::Plain);
	AppendDiffFragment(
		result,
		u"+ "_q + (after.isEmpty() ? QString("-") : after),
		RevisionDiffKind::Added);
	return result;
}

[[nodiscard]] std::vector<RevisionDiffBlock> RevisionDiffBlocks(
		const HistoryMessageRevisionSnapshot &previous,
		const HistoryMessageRevisionSnapshot &current) {
	auto result = std::vector<RevisionDiffBlock>();
	if (previous.text != current.text) {
		result.push_back({
			.title = tr::lng_message_versions_text(tr::now),
			.fragments = RevisionTextDiff(previous.text, current.text),
		});
	}
	if (previous.media != current.media) {
		result.push_back({
			.title = tr::lng_message_versions_media(tr::now),
			.fragments = RevisionValueDiff(previous.media, current.media),
		});
	}
	if (previous.entitiesCount != current.entitiesCount) {
		result.push_back({
			.title = tr::lng_message_versions_entities(tr::now),
			.fragments = RevisionValueDiff(
				QString::number(previous.entitiesCount),
				QString::number(current.entitiesCount)),
		});
	}
	return result;
}

[[nodiscard]] QTextLayout::FormatRange RevisionDiffFormat(
		int start,
		int length,
		RevisionDiffKind kind) {
	auto result = QTextLayout::FormatRange();
	result.start = start;
	result.length = length;
	auto background = (kind == RevisionDiffKind::Added)
		? st::boxTextFgGood->c
		: st::boxTextFgError->c;
	background.setAlphaF(0.18);
	auto foreground = (kind == RevisionDiffKind::Added)
		? st::boxTextFgGood->c
		: st::boxTextFgError->c;
	result.format.setBackground(QBrush(background));
	result.format.setForeground(QBrush(foreground));
	if (kind == RevisionDiffKind::Removed) {
		result.format.setFontStrikeOut(true);
	}
	return result;
}

[[nodiscard]] QString RevisionDiffText(
		const std::vector<RevisionDiffFragment> &fragments) {
	auto result = QString();
	for (const auto &fragment : fragments) {
		result += fragment.text;
	}
	return result;
}

[[nodiscard]] QVector<QTextLayout::FormatRange> RevisionDiffFormats(
		const std::vector<RevisionDiffFragment> &fragments) {
	auto result = QVector<QTextLayout::FormatRange>();
	auto position = 0;
	for (const auto &fragment : fragments) {
		const auto length = fragment.text.size();
		if (fragment.kind != RevisionDiffKind::Plain && length > 0) {
			result.push_back(
				RevisionDiffFormat(position, length, fragment.kind));
		}
		position += length;
	}
	return result;
}

[[nodiscard]] int RevisionDiffLayoutHeight(
		const QString &text,
		int availableWidth) {
	auto layout = QTextLayout(text, st::boxLabel.style.font->f);
	auto option = QTextOption();
	option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
	layout.setTextOption(option);
	layout.beginLayout();
	auto height = qreal(0);
	while (true) {
		auto line = layout.createLine();
		if (!line.isValid()) {
			break;
		}
		line.setLineWidth(availableWidth);
		line.setPosition(QPointF(0, height));
		height += line.height();
	}
	layout.endLayout();
	return int(std::ceil(height));
}

void PaintRevisionDiffText(
		Painter &p,
		const QString &text,
		const QVector<QTextLayout::FormatRange> &formats,
		QPoint position,
		int availableWidth) {
	auto layout = QTextLayout(text, st::boxLabel.style.font->f);
	auto option = QTextOption();
	option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
	layout.setTextOption(option);
	layout.setFormats(formats);
	layout.beginLayout();
	auto height = qreal(0);
	while (true) {
		auto line = layout.createLine();
		if (!line.isValid()) {
			break;
		}
		line.setLineWidth(availableWidth);
		line.setPosition(QPointF(0, height));
		height += line.height();
	}
	layout.endLayout();
	p.setPen(st::boxTextFg);
	for (auto i = 0; i != layout.lineCount(); ++i) {
		layout.lineAt(i).draw(&p, position);
	}
}

class RevisionDiffView final : public Ui::RpWidget {
public:
	RevisionDiffView(
		QWidget *parent,
		std::vector<RevisionDiffBlock> blocks)
	: RpWidget(parent)
	, _blocks(std::move(blocks)) {
	}

protected:
	void paintEvent(QPaintEvent*) override {
		auto p = Painter(this);
		const auto &padding = st::defaultSettingsButton.padding;
		const auto titleFont = st::defaultSettingsButton.style.font;
		const auto left = padding.left();
		const auto availableWidth = std::max(
			width() - padding.left() - padding.right(),
			0);
		auto top = padding.top();
		for (const auto &block : _blocks) {
			p.setFont(titleFont);
			p.setPen(st::boxTitleFg);
			p.drawTextLeft(left, top, width(), block.title);
			top += titleFont->height + padding.bottom();
			const auto text = RevisionDiffText(block.fragments);
			const auto formats = RevisionDiffFormats(block.fragments);
			PaintRevisionDiffText(
				p,
				text,
				formats,
				QPoint(left, top),
				availableWidth);
			top += RevisionDiffLayoutHeight(text, availableWidth)
				+ padding.bottom();
		}
	}

	int resizeGetHeight(int newWidth) override {
		const auto &padding = st::defaultSettingsButton.padding;
		const auto titleFont = st::defaultSettingsButton.style.font;
		const auto availableWidth = std::max(
			newWidth - padding.left() - padding.right(),
			0);
		auto result = padding.top();
		for (const auto &block : _blocks) {
			const auto text = RevisionDiffText(block.fragments);
			result += titleFont->height
				+ padding.bottom()
				+ RevisionDiffLayoutHeight(text, availableWidth)
				+ padding.bottom();
		}
		return result;
	}

private:
	std::vector<RevisionDiffBlock> _blocks;

};

class RevisionRowButton final : public Ui::RippleButton {
public:
	RevisionRowButton(
		QWidget *parent,
		QString title,
		QString date,
		QString preview)
	: Ui::RippleButton(parent, st::defaultSettingsButton.ripple)
	, _title(std::move(title))
	, _date(std::move(date))
	, _preview(std::move(preview)) {
		setCursor(style::cur_pointer);
	}

	QString accessibilityName() override {
		return _date.isEmpty() ? _title : (_title + u", "_q + _date);
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		const auto over = (isOver() || isDown()) && !isDisabled();
		if (over) {
			p.fillRect(e->rect(), st::defaultSettingsButton.textBgOver);
		}
		paintRipple(p, 0, 0);

		const auto &padding = st::defaultSettingsButton.padding;
		const auto titleFont = st::defaultSettingsButton.style.font;
		const auto previewFont = st::boxLabel.style.font;
		const auto left = padding.left();
		const auto right = padding.right();
		const auto available = std::max(width() - left - right, 0);
		const auto dateWidth = _date.isEmpty()
			? 0
			: titleFont->width(_date);
		const auto titleAvailable = std::max(
			available - dateWidth - (dateWidth ? titleFont->spacew : 0),
			0);
		auto top = padding.top();
		p.setFont(titleFont);
		p.setPen(over ? st::boxTextFgGood : st::boxTitleFg);
		p.drawTextLeft(
			left,
			top,
			width(),
			titleFont->elided(_title, titleAvailable));
		if (dateWidth > 0) {
			p.setPen(st::windowSubTextFg);
			p.drawTextRight(right, top, width(), _date, dateWidth);
		}
		if (!_preview.isEmpty()) {
			top += titleFont->height;
			p.setFont(previewFont);
			p.setPen(st::windowSubTextFg);
			p.drawTextLeft(
				left,
				top,
				width(),
				previewFont->elided(_preview, available));
		}
	}

	int resizeGetHeight(int) override {
		const auto &padding = st::defaultSettingsButton.padding;
		const auto titleFont = st::defaultSettingsButton.style.font;
		const auto previewFont = st::boxLabel.style.font;
		return padding.top()
			+ titleFont->height
			+ (_preview.isEmpty() ? 0 : previewFont->height)
			+ padding.bottom();
	}

private:
	QString _title;
	QString _date;
	QString _preview;

};

void AddRevisionLabel(
		not_null<Ui::GenericBox*> box,
		const QString &text,
		bool selectable = false) {
	const auto label = box->addRow(
		object_ptr<Ui::FlatLabel>(box, text, st::boxLabel));
	label->setSelectable(selectable);
	label->setBreakEverywhere(true);
}

void MessageRevisionsBox(
	not_null<Ui::GenericBox*> box,
	not_null<Data::Session*> owner,
	FullMsgId itemId);

void MessageRevisionDetailsBox(
	not_null<Ui::GenericBox*> box,
	not_null<Data::Session*> owner,
	FullMsgId itemId,
	int index);

void ReplaceRevisionBox(
		not_null<Ui::GenericBox*> box,
		object_ptr<Ui::GenericBox> next) {
	box->getDelegate()->show(
		std::move(next),
		Ui::LayerOption::CloseOther,
		anim::type::instant);
}

void AddRevisionCloseButton(not_null<Ui::GenericBox*> box) {
	box->addButton(tr::lng_close(), [=] {
		box->closeBox();
	});
}

void MessageRevisionDetailsBox(
		not_null<Ui::GenericBox*> box,
		not_null<Data::Session*> owner,
		FullMsgId itemId,
		int index) {
	box->setWidth(st::boxWidth);
	box->setTitle(RevisionTitle(index));
	const auto item = owner->message(itemId);
	const auto history = item ? item->revisionHistory() : nullptr;
	const auto count = history ? int(history->versions.size()) : 0;
	if (!history || index < 0 || index >= count) {
		AddRevisionLabel(
			box,
			tr::lng_message_versions_empty(tr::now),
			true);
		AddRevisionCloseButton(box);
		return;
	}
	const auto back = box->addRow(object_ptr<Ui::LinkButton>(
		box,
		tr::lng_message_versions_back(tr::now)));
	back->setClickedCallback([=] {
		ReplaceRevisionBox(box, Box(MessageRevisionsBox, owner, itemId));
	});
	const auto &snapshot = history->versions[index];
	const auto date = RevisionDate(snapshot);
	if (!date.isEmpty()) {
		AddRevisionLabel(box, date);
	}
	if (index) {
		auto blocks = RevisionDiffBlocks(history->versions[index - 1], snapshot);
		if (blocks.empty()) {
			AddRevisionLabel(
				box,
				tr::lng_message_versions_no_diff(tr::now),
				true);
		} else {
			box->addRow(object_ptr<RevisionDiffView>(
				box,
				std::move(blocks)));
		}
	}
	if (!index) {
		const auto snapshotContent = RevisionContent(snapshot);
		if (!snapshotContent.isEmpty()) {
			AddRevisionLabel(box, snapshotContent, true);
		}
	}
	AddRevisionCloseButton(box);
}

void MessageRevisionsBox(
		not_null<Ui::GenericBox*> box,
		not_null<Data::Session*> owner,
		FullMsgId itemId) {
	box->setWidth(st::boxWidth);
	box->setTitle(tr::lng_message_versions_title(tr::now));
	const auto item = owner->message(itemId);
	const auto history = item ? item->revisionHistory() : nullptr;
	const auto count = history ? int(history->versions.size()) : 0;
	if (count < 2) {
		AddRevisionLabel(
			box,
			tr::lng_message_versions_empty(tr::now),
			true);
		AddRevisionCloseButton(box);
		return;
	}
	for (auto i = 0; i != count; ++i) {
		const auto &snapshot = history->versions[i];
		const auto button = box->addRow(object_ptr<RevisionRowButton>(
			box,
			RevisionTitle(i),
			RevisionDate(snapshot),
			RevisionPreview(snapshot)));
		button->setClickedCallback([=] {
			ReplaceRevisionBox(
				box,
				Box(MessageRevisionDetailsBox, owner, itemId, i));
		});
	}
	AddRevisionCloseButton(box);
}

} // namespace

struct BottomInfo::Effect {
	mutable std::unique_ptr<Ui::ReactionFlyAnimation> animation;
	mutable QImage image;
	EffectId id = 0;
};

BottomInfo::BottomInfo(
	not_null<::Data::Reactions*> reactionsOwner,
	Data &&data)
: _reactionsOwner(reactionsOwner)
, _data(std::move(data)) {
	layout();
}

BottomInfo::~BottomInfo() = default;

void BottomInfo::update(Data &&data, int availableWidth) {
	_data = std::move(data);
	layout();
	if (width() > 0) {
		resizeGetHeight(std::min(maxWidth(), availableWidth));
	}
}

int BottomInfo::countEffectMaxWidth() const {
	auto result = 0;
	if (_effect) {
		result += st::reactionInfoSize;
		result += st::reactionInfoBetween;
	}
	if (result) {
		result += (st::reactionInfoSkip - st::reactionInfoBetween);
	}
	return result;
}

int BottomInfo::countEffectHeight(int newWidth) const {
	const auto left = 0;
	auto x = 0;
	auto y = 0;
	auto widthLeft = newWidth;
	if (_effect) {
		const auto add = st::reactionInfoBetween;
		const auto width = st::reactionInfoSize;
		if (x > left && widthLeft < width) {
			x = left;
			y += st::msgDateFont->height;
			widthLeft = newWidth;
		}
		x += width + add;
		widthLeft -= width + add;
	}
	if (x > left) {
		y += st::msgDateFont->height;
	}
	return y;
}

int BottomInfo::firstLineWidth() const {
	if (height() == minHeight()) {
		return width();
	}
	return maxWidth() - _effectMaxWidth;
}

bool BottomInfo::isWide() const {
	return (_data.flags & Data::Flag::Edited)
		|| _data.deletedDate
		|| _data.scheduleRepeatPeriod
		|| !_data.author.isEmpty()
		|| !_views.isEmpty()
		|| !_replies.isEmpty()
		|| _effect
		|| _data.tonStake;
}

TextState BottomInfo::textState(
		not_null<const Message*> view,
		QPoint position) const {
	const auto item = view->data();
	auto result = TextState(item);
	if (const auto link = replayEffectLink(view, position)) {
		result.link = link;
		return result;
	}
	const auto authorEditedWidth = _authorEditedDate.maxWidth();
	const auto deletedDateWidth = _deletedDateLabel.maxWidth();
	const auto textWidth = authorEditedWidth + deletedDateWidth;
	auto withTicksWidth = textWidth;
	if (_data.flags & (Data::Flag::OutLayout | Data::Flag::Sending)) {
		withTicksWidth += st::historySendStateSpace;
	}
	if (!_views.isEmpty()) {
		const auto viewsWidth = _views.maxWidth();
		const auto right = width()
			- withTicksWidth
			- ((_data.flags & Data::Flag::Pinned) ? st::historyPinWidth : 0)
			- st::historyViewsSpace
			- st::historyViewsWidth
			- viewsWidth;
		const auto inViews = QRect(
			right,
			0,
			withTicksWidth + st::historyViewsWidth,
			st::msgDateFont->height
		).contains(position);
		if (inViews) {
			result.customTooltip = true;
			const auto fullViews = tr::lng_views_tooltip(
				tr::now,
				lt_count_decimal,
				*_data.views);
			const auto fullForwards = _data.forwardsCount
				? ('\n' + tr::lng_forwards_tooltip(
					tr::now,
					lt_count_decimal,
					*_data.forwardsCount))
				: QString();
			result.customTooltipText = fullViews + fullForwards;
		}
	}
	const auto inTime = QRect(
		width() - withTicksWidth,
		0,
		withTicksWidth,
		st::msgDateFont->height
	).contains(position);
	if (_editedLabelWidth > 0) {
		const auto left = width() - withTicksWidth + _editedLabelLeft;
		const auto inEdited = QRect(
			left,
			0,
			_editedLabelWidth,
			st::msgDateFont->height
		).contains(position);
		if (inEdited) {
			result.link = editedLink(view);
			return result;
		}
	}
	if (inTime) {
		result.cursor = CursorState::Date;
	}
	return result;
}

ClickHandlerPtr BottomInfo::replayEffectLink(
		not_null<const Message*> view,
		QPoint position) const {
	if (!_effect) {
		return nullptr;
	}
	auto left = 0;
	auto top = 0;
	auto available = width();
	if (height() != minHeight()) {
		available = std::min(available, _effectMaxWidth);
		left += width() - available;
		top += st::msgDateFont->height;
	}
	if (_effect) {
		const auto image = QRect(
			left,
			top,
			st::reactionInfoSize,
			st::msgDateFont->height);
		if (image.contains(position)) {
			if (!_replayLink) {
				_replayLink = replayEffectLink(view);
			}
			return _replayLink;
		}
	}
	return nullptr;
}

ClickHandlerPtr BottomInfo::replayEffectLink(
		not_null<const Message*> view) const {
	const auto weak = base::make_weak(view);
	return std::make_shared<LambdaClickHandler>([=](ClickContext context) {
		const auto my = context.other.value<ClickHandlerContext>();
		if ([[maybe_unused]] const auto controller = my.sessionWindow.get()) {
			if (const auto strong = weak.get()) {
				strong->delegate()->elementStartEffect(strong, nullptr);
			}
		}
	});
}

ClickHandlerPtr BottomInfo::editedLink(not_null<const Message*> view) const {
	if (!(_data.flags & Data::Flag::Edited)) {
		return nullptr;
	}
	if (!_editedLink) {
		const auto weak = base::make_weak(view);
		_editedLink = std::make_shared<LambdaClickHandler>(
			[=](ClickContext context) {
				if (const auto controller = ExtractController(context)) {
					if (const auto strong = weak.get()) {
						controller->show(Box(
							MessageRevisionsBox,
							&controller->session().data(),
							strong->data()->fullId()));
					}
				}
			});
	}
	return _editedLink;
}

bool BottomInfo::isSignedAuthorElided() const {
	return _authorElided;
}

void BottomInfo::paint(
		Painter &p,
		QPoint position,
		int outerWidth,
		bool unread,
		bool inverted,
		const PaintContext &context) const {
	const auto st = context.st;
	const auto stm = context.messageStyle();

	auto right = position.x() + width();
	const auto firstLineBottom = position.y() + st::msgDateFont->height;
	if (_data.flags & Data::Flag::OutLayout) {
		const auto &icon = (_data.flags & Data::Flag::Sending)
			? (inverted
				? st->historySendingInvertedIcon()
				: st->historySendingIcon())
			: unread
			? (inverted
				? st->historySentInvertedIcon()
				: stm->historySentIcon)
			: (inverted
				? st->historyReceivedInvertedIcon()
				: stm->historyReceivedIcon);
		icon.paint(
			p,
			QPoint(right, firstLineBottom) + st::historySendStatePosition,
			outerWidth);
		right -= st::historySendStateSpace;
	}

	const auto authorEditedWidth = _authorEditedDate.maxWidth();
	const auto deletedLabelWidth = _deletedDateLabel.maxWidth();
	right -= authorEditedWidth + deletedLabelWidth;
	const auto editedActive = _editedLabelWidth > 0
		&& _editedLink
		&& ClickHandler::showAsActive(_editedLink);
	if (editedActive) {
		const auto &padding = st::msgTagBadgePadding;
		const auto pillHeight = padding.top()
			+ st::msgDateFont->height
			+ padding.bottom();
		const auto pillRect = QRect(
			right + _editedLabelLeft - padding.left(),
			position.y() + (st::msgDateFont->height - pillHeight) / 2,
			padding.left() + _editedLabelWidth + padding.right(),
			pillHeight);
		auto color = stm->msgDateFg->c;
		color.setAlphaF(0.10);
		const auto pen = p.pen();
		const auto brush = p.brush();
		p.setPen(Qt::NoPen);
		p.setBrush(color);
		{
			auto hq = PainterHighQualityEnabler(p);
			p.drawRoundedRect(
				pillRect,
				pillRect.height() / 2.,
				pillRect.height() / 2.);
		}
		p.setPen(pen);
		p.setBrush(brush);
	}
	p.setPen(stm->msgDateFg);
	_authorEditedDate.drawLeft(
		p,
		right,
		position.y(),
		authorEditedWidth,
		outerWidth);
	if (editedActive) {
		_editedLabelText.drawLeft(
			p,
			right + _editedLabelLeft,
			position.y(),
			_editedLabelWidth,
			outerWidth);
	}
	if (deletedLabelWidth > 0) {
		const auto opacity = p.opacity();
		p.setOpacity(std::min(1., opacity / kDeletedMessageOpacity));
		_deletedDateLabel.drawLeft(
			p,
			right + authorEditedWidth,
			position.y(),
			deletedLabelWidth,
			outerWidth);
		p.setOpacity(opacity);
	}

	if (_data.flags & Data::Flag::Silent) {
		const auto &icon = inverted
			? st->historySilentInvertedIcon()
			: stm->historySilentIcon;
		right -= st::historySilentWidth;
		icon.paint(
			p,
			right,
			firstLineBottom + st::historySilentTop,
			outerWidth);
	}
	if (_data.flags & Data::Flag::Ephemeral) {
		const auto &icon = inverted
			? st->historyEphemeralInvertedIcon()
			: stm->historyEphemeralIcon;
		right -= st::historyEphemeralStateWidth;
		icon.paint(
			p,
			right,
			firstLineBottom + st::historyEphemeralStateTop,
			outerWidth);
	}

	if (_data.flags & Data::Flag::Pinned) {
		const auto &icon = inverted
			? st->historyPinInvertedIcon()
			: stm->historyPinIcon;
		right -= st::historyPinWidth;
		icon.paint(
			p,
			right,
			firstLineBottom + st::historyPinTop,
			outerWidth);
	}
	if (!_views.isEmpty()) {
		const auto viewsWidth = _views.maxWidth();
		right -= st::historyViewsSpace + viewsWidth;
		_views.drawLeft(p, right, position.y(), viewsWidth, outerWidth);

		const auto &icon = inverted
			? st->historyViewsInvertedIcon()
			: stm->historyViewsIcon;
		right -= st::historyViewsWidth;
		icon.paint(
			p,
			right,
			firstLineBottom + st::historyViewsTop,
			outerWidth);
	}
	if (!_replies.isEmpty()) {
		const auto repliesWidth = _replies.maxWidth();
		right -= st::historyViewsSpace + repliesWidth;
		_replies.drawLeft(p, right, position.y(), repliesWidth, outerWidth);

		const auto &icon = inverted
			? st->historyRepliesInvertedIcon()
			: stm->historyRepliesIcon;
		right -= st::historyViewsWidth;
		icon.paint(
			p,
			right,
			firstLineBottom + st::historyViewsTop,
			outerWidth);
	}
	if ((_data.flags & Data::Flag::Sending)
		&& !(_data.flags & Data::Flag::OutLayout)) {
		right -= st::historySendStateSpace;
		const auto &icon = inverted
			? st->historyViewsSendingInvertedIcon()
			: st->historyViewsSendingIcon();
		icon.paint(
			p,
			right,
			firstLineBottom + st::historyViewsTop,
			outerWidth);
	}
	if (_effect) {
		auto left = position.x();
		auto top = position.y();
		auto available = width();
		if (height() != minHeight()) {
			available = std::min(available, _effectMaxWidth);
			left += width() - available;
			top += st::msgDateFont->height;
		}
		paintEffect(p, position, left, top, available, context);
	}
}

void BottomInfo::paintEffect(
		Painter &p,
		QPoint origin,
		int left,
		int top,
		int availableWidth,
		const PaintContext &context) const {
	struct SingleAnimation {
		not_null<Ui::ReactionFlyAnimation*> animation;
		QRect target;
	};
	std::vector<SingleAnimation> animations;

	auto x = left;
	auto y = top;
	auto widthLeft = availableWidth;
	if (_effect) {
		const auto animating = (_effect->animation != nullptr);
		const auto add = st::reactionInfoBetween;
		const auto width = st::reactionInfoSize;
		if (x > left && widthLeft < width) {
			x = left;
			y += st::msgDateFont->height;
			widthLeft = availableWidth;
		}
		if (_effect->image.isNull()) {
			_effect->image = _reactionsOwner->resolveEffectImageFor(
				_effect->id);
		}
		const auto image = QRect(
			x + (st::reactionInfoSize - st::effectInfoImage) / 2,
			y + (st::msgDateFont->height - st::effectInfoImage) / 2,
			st::effectInfoImage,
			st::effectInfoImage);
		if (!_effect->image.isNull()) {
			p.drawImage(image.topLeft(), _effect->image);
		}
		if (animating) {
			animations.push_back({
				.animation = _effect->animation.get(),
				.target = image,
			});
		}
		x += width + add;
		widthLeft -= width + add;
	}
	if (!animations.empty() && context.reactionInfo) {
		const auto now = context.now;
		context.reactionInfo->effectPaint = [
			now,
			origin,
			list = std::move(animations)
		](QPainter &p) {
			auto result = QRect();
			for (const auto &single : list) {
				const auto area = single.animation->paintGetArea(
					p,
					origin,
					single.target,
					QColor(255, 255, 255, 0), // Colored, for emoji status.
					QRect(), // Clip, for emoji status.
					now);
				result = result.isEmpty() ? area : result.united(area);
			}
			return result;
		};
	}
}

QSize BottomInfo::countCurrentSize(int newWidth) {
	if (newWidth >= maxWidth() || (_data.flags & Data::Flag::Shortcut)) {
		return optimalSize();
	}
	const auto dateHeight = (_data.flags & Data::Flag::Sponsored)
		? 0
		: st::msgDateFont->height;
	const auto noReactionsWidth = maxWidth() - _effectMaxWidth;
	accumulate_min(newWidth, std::max(noReactionsWidth, _effectMaxWidth));
	return QSize(
		newWidth,
		dateHeight + countEffectHeight(newWidth));
}

void BottomInfo::layout() {
	layoutDateText();
	layoutViewsText();
	layoutRepliesText();
	layoutEffectText();
	initDimensions();
}

void BottomInfo::layoutDateText() {
	if (!(_data.flags & Data::Flag::Edited)) {
		_editedLink = nullptr;
	}
	const auto editedLabel = (_data.flags & Data::Flag::Edited)
		? (tr::lng_edited(tr::now)
			+ (_data.editCount
				? (u" "_q + QString::number(_data.editCount))
				: QString()))
		: QString();
	const auto leading = !editedLabel.isEmpty()
		? editedLabel
		: (_data.flags & Data::Flag::EstimateDate)
		? tr::lng_approximate(tr::now)
		: _data.scheduleRepeatPeriod
		? SchedulePeriodText(_data.scheduleRepeatPeriod)
		: QString();
	const auto author = _data.author;
	const auto prefix = !author.isEmpty() ? u", "_q : QString();
	const auto published = (_data.flags & Data::Flag::ForwardedDate)
		? Ui::FormatDateTimeSavedFrom(_data.date)
		: QLocale().toString(_data.date.time(), QLocale::ShortFormat);
	const auto deletedLabel = _data.deletedDate
		? tr::lng_message_deleted(tr::now)
		: QString();
	const auto deleted = _data.deletedDate
		? (u" "_q
			+ deletedLabel
			+ u" "_q
			+ RevisionDate(_data.deletedDate))
		: QString();
	const auto date = (leading.isEmpty() ? QString() : (leading + ' '))
		+ published;
	const auto afterAuthor = prefix + date;
	const auto deletedWidth = st::msgDateFont->width(deleted);
	const auto afterAuthorWidth = st::msgDateFont->width(afterAuthor)
		+ deletedWidth;
	const auto authorWidth = st::msgDateFont->width(author);
	const auto maxWidth = st::maxSignatureSize;
	_authorElided = !author.isEmpty()
		&& (authorWidth + afterAuthorWidth > maxWidth);
	const auto name = _authorElided
		? st::msgDateFont->elided(author, maxWidth - afterAuthorWidth)
		: author;
	_editedLabelWidth = editedLabel.isEmpty()
		? 0
		: st::msgDateFont->width(editedLabel);
	if (_editedLabelWidth > 0) {
		_editedLabelText.setText(
			st::msgDateTextStyle,
			editedLabel,
			Ui::NameTextOptions());
	} else {
		_editedLabelText.clear();
	}
	_editedLabelLeft = _editedLabelWidth
		? ((_data.flags & Data::Flag::Imported)
			? 0
			: (name.isEmpty() ? 0 : st::msgDateFont->width(name + prefix)))
		: 0;
	const auto full = (_data.flags & Data::Flag::Sponsored)
		? QString()
		: (_data.flags & Data::Flag::Imported)
		? (date + ' ' + tr::lng_imported(tr::now))
		: name.isEmpty()
		? date
		: (name + afterAuthor);
	auto helper = Ui::Text::CustomEmojiHelper(
		Core::TextContext({ .session = &_reactionsOwner->session() }));
	auto marked = TextWithEntities();
	if (const auto count = _data.stars) {
		marked.append(
			Ui::Text::IconEmoji(&st::starIconEmojiSmall)
		).append(Lang::FormatCountToShort(count).string).append(u", "_q);
	}
	if (const auto stake = _data.tonStake) {
		marked.append(
			QString::number(stake / 1e9)
		).append(helper.image({
			.image = Ui::Emoji::SinglePixmap(
				Ui::Emoji::Find(QString::fromUtf8("\xf0\x9f\x92\x8e")),
				Ui::Emoji::GetSizeNormal()).toImage().scaledToHeight(
					st::stakeIconEmojiSize * style::DevicePixelRatio(),
					Qt::SmoothTransformation),
			.margin = QMargins(0, st::stakeIconEmojiTop, 0, 0),
			.textColor = false,
		})).append("  ");
	}
	marked.append(full);
	_authorEditedDate.setMarkedText(
		st::msgDateTextStyle,
		marked,
		Ui::NameTextOptions(),
		helper.context());
	_deletedDateLabel.setText(
		st::msgDateTextStyle,
		deleted,
		Ui::NameTextOptions());
}

void BottomInfo::layoutViewsText() {
	if (!_data.views || (_data.flags & Data::Flag::Sending)) {
		_views.clear();
		return;
	}
	_views.setText(
		st::msgDateTextStyle,
		Lang::FormatCountToShort(std::max(*_data.views, 1)).string,
		Ui::NameTextOptions());
}

void BottomInfo::layoutRepliesText() {
	if (!_data.replies
		|| !*_data.replies
		|| (_data.flags & Data::Flag::RepliesContext)
		|| (_data.flags & Data::Flag::Sending)
		|| (_data.flags & Data::Flag::Shortcut)) {
		_replies.clear();
		return;
	}
	_replies.setText(
		st::msgDateTextStyle,
		Lang::FormatCountToShort(*_data.replies).string,
		Ui::NameTextOptions());
}

void BottomInfo::layoutEffectText() {
	if (!_data.effectId) {
		_effect = nullptr;
		return;
	}
	_effect = std::make_unique<Effect>(prepareEffectWithId(_data.effectId));
}

QSize BottomInfo::countOptimalSize() {
	if (_data.flags & Data::Flag::Shortcut) {
		return { st::historyShortcutStateSpace, st::msgDateFont->height };
	}
	auto width = 0;
	if (_data.flags & (Data::Flag::OutLayout | Data::Flag::Sending)) {
		width += st::historySendStateSpace;
	}
	width += _authorEditedDate.maxWidth();
	width += _deletedDateLabel.maxWidth();
	if (!_views.isEmpty()) {
		width += st::historyViewsSpace
			+ _views.maxWidth()
			+ st::historyViewsWidth;
	}
	if (!_replies.isEmpty()) {
		width += st::historyViewsSpace
			+ _replies.maxWidth()
			+ st::historyViewsWidth;
	}
	if (_data.flags & Data::Flag::Pinned) {
		width += st::historyPinWidth;
	}
	if (_data.flags & Data::Flag::Silent) {
		width += st::historySilentWidth;
	}
	if (_data.flags & Data::Flag::Ephemeral) {
		width += st::historyEphemeralStateWidth;
	}
	_effectMaxWidth = countEffectMaxWidth();
	width += _effectMaxWidth;
	const auto dateHeight = (_data.flags & Data::Flag::Sponsored)
		? 0
		: st::msgDateFont->height;
	return QSize(width, dateHeight);
}

BottomInfo::Effect BottomInfo::prepareEffectWithId(EffectId id) {
	auto result = Effect{ .id = id };
	_reactionsOwner->preloadEffectImageFor(id);
	return result;
}

auto BottomInfo::takeEffectAnimation()
-> std::unique_ptr<Ui::ReactionFlyAnimation> {
	return _effect ? std::move(_effect->animation) : nullptr;
}

void BottomInfo::continueEffectAnimation(
		std::unique_ptr<Ui::ReactionFlyAnimation> animation) {
	if (_effect) {
		_effect->animation = std::move(animation);
	}
}

QRect BottomInfo::effectIconGeometry() const {
	if (!_effect) {
		return {};
	}
	auto left = 0;
	auto top = 0;
	auto available = width();
	if (height() != minHeight()) {
		available = std::min(available, _effectMaxWidth);
		left += width() - available;
		top += st::msgDateFont->height;
	}
	return QRect(
		left + (st::reactionInfoSize - st::effectInfoImage) / 2,
		top + (st::msgDateFont->height - st::effectInfoImage) / 2,
		st::effectInfoImage,
		st::effectInfoImage);
}

BottomInfo::Data BottomInfoDataFromMessage(not_null<Message*> message) {
	using Flag = BottomInfo::Data::Flag;
	const auto item = message->data();

	auto result = BottomInfo::Data();
	result.date = message->dateTime();
	result.effectId = item->effectId();
	if (message->hasOutLayout()) {
		result.flags |= Flag::OutLayout;
	}
	if (message->context() == Context::Replies) {
		result.flags |= Flag::RepliesContext;
	}
	if (item->isSponsored()) {
		result.flags |= Flag::Sponsored;
	}
	if (item->isPinned() && message->context() != Context::Pinned) {
		result.flags |= Flag::Pinned;
	}
	if (message->context() == Context::ShortcutMessages) {
		result.flags |= Flag::Shortcut;
	}
	if (!item->isPost()
		|| !item->hasRealFromId()
		|| !item->history()->peer->asChannel()->signatureProfiles()) {
		if (const auto msgsigned = item->Get<HistoryMessageSigned>()) {
			if (!msgsigned->isAnonymousRank) {
				result.author = msgsigned->author;
			}
		}
	}
	if (const auto editedDate = message->displayedEditDate()) {
		result.flags |= Flag::Edited;
		result.editCount = std::max(item->editCount(), 1);
	}
	result.deletedDate = item->deletedDate();
	if (const auto views = item->Get<HistoryMessageViews>()) {
		if (views->views.count >= 0) {
			result.views = views->views.count;
		}
		if (views->replies.count >= 0 && !views->commentsMegagroupId) {
			result.replies = views->replies.count;
		}
		if (views->forwardsCount > 0) {
			result.forwardsCount = views->forwardsCount;
		}
	}
	if (item->isSending() || item->hasFailed()) {
		result.flags |= Flag::Sending;
	}
	if (item->isEphemeral()
		&& !message->hasBubble()
		&& (!message->media()
			|| !message->media()->drawsOwnEphemeralBadge())) {
		result.flags |= Flag::Ephemeral;
	}
	if (!item->history()->peer->isUser()) {
		const auto mine = PaidInformation{
			.messages = 1,
			.stars = item->starsPaid(),
		};
		const auto media = message->media();
		auto info = media ? media->paidInformation().value_or(mine) : mine;
		if (const auto total = info.stars) {
			result.stars = total;
		}
	}
	if (const auto media = item->media()) {
		if (const auto outcome = media->diceGameOutcome()) {
			result.tonStake = outcome.stakeNanoTon;
		}
	}
	const auto forwarded = item->Get<HistoryMessageForwarded>();
	if (forwarded && forwarded->imported) {
		result.flags |= Flag::Imported;
	}
	if (item->awaitingVideoProcessing()) {
		result.flags |= Flag::EstimateDate;
	}
	if (item->isScheduled()) {
		result.scheduleRepeatPeriod = item->scheduleRepeatPeriod();
		if (item->isSilent()) {
			result.flags |= Flag::Silent;
		}
	}
	if (!forwarded) {
		return result;
	}
	if (forwarded->savedFromMsgId && forwarded->savedFromDate) {
		result.date = base::unixtime::parse(forwarded->savedFromDate);
		result.flags |= Flag::ForwardedDate;
	} else if (forwarded->originalDate
		&& (message->context() == Context::SavedSublist
			|| item->history()->peer->isSelf())
		&& !item->externalReply()) {
		result.date = base::unixtime::parse(forwarded->originalDate);
		result.flags |= Flag::ForwardedDate;
	}
	// We don't want to pass and update it in Data for now.
	//if (item->unread()) {
	//	result.flags |= Flag::Unread;
	//}
	return result;
}

} // namespace HistoryView
