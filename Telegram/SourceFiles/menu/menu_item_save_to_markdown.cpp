/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "menu/menu_item_save_to_markdown.h"

#include "base/base_file_utilities.h"
#include "base/options.h"
#include "base/unixtime.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "history/view/history_view_list_widget.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_session_controller.h"

#include "styles/style_menu_icons.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QStandardPaths>
#include <QtCore/QUrl>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

namespace Menu {
namespace {

constexpr auto kMaxTotalFileSize = int64(200) * 1024 * 1024;

base::options::option<QString> OptionMarkdownClipboardText({
	.id = Menu::kOptionMarkdownClipboardText,
	.name = "Clipboard text for 'Save to markdown file'",
	.description = "Pattern copied to clipboard after saving a markdown "
		"file. '%q' is replaced with the absolute path to the file. "
		"Leave empty to copy the path itself.",
});

struct Entry {
	FullMsgId fullId;
	QString fromName;
	TimeId date = 0;
	QString text;
	PhotoData *photo = nullptr;
	DocumentData *document = nullptr;
	QString filename;
	std::shared_ptr<Data::PhotoMedia> photoView;
};

[[nodiscard]] QString SanitizeStem(QString stem) {
	auto sanitized = base::FileNameFromUserString(std::move(stem));
	if (sanitized.isEmpty()) {
		sanitized = u"file"_q;
	}
	return sanitized;
}

[[nodiscard]] QString UniqueName(
		const QString &stem,
		const QString &extension,
		base::flat_set<QString> &used) {
	auto candidate = stem + extension;
	auto index = 1;
	while (used.contains(candidate.toLower())) {
		candidate = stem + u"_"_q + QString::number(++index) + extension;
	}
	used.emplace(candidate.toLower());
	return candidate;
}

[[nodiscard]] QString PickDocumentName(
		not_null<DocumentData*> document,
		FullMsgId itemId,
		base::flat_set<QString> &used) {
	const auto raw = document->filename();
	if (!raw.isEmpty()) {
		const auto dot = raw.lastIndexOf('.');
		const auto stem = SanitizeStem(
			(dot > 0) ? raw.left(dot) : raw);
		const auto extension = (dot > 0) ? raw.mid(dot) : QString();
		return UniqueName(stem, extension, used);
	}
	auto extension = u".bin"_q;
	if (document->isVideoFile() || document->isVideoMessage()) {
		extension = u".mp4"_q;
	} else if (document->isVoiceMessage()) {
		extension = u".ogg"_q;
	} else if (document->isAudioFile()) {
		extension = u".mp3"_q;
	} else if (document->isAnimation()) {
		extension = u".gif"_q;
	}
	const auto stem = u"document_"_q + QString::number(itemId.msg.bare);
	return UniqueName(stem, extension, used);
}

[[nodiscard]] QString PickPhotoName(
		FullMsgId itemId,
		base::flat_set<QString> &used) {
	const auto stem = u"photo_"_q + QString::number(itemId.msg.bare);
	return UniqueName(stem, u".jpg"_q, used);
}

[[nodiscard]] QString ResolveAuthorName(not_null<HistoryItem*> item) {
	if (const auto info = item->originalHiddenSenderInfo()) {
		if (!info->name.isEmpty()) {
			return info->name;
		}
	}
	if (const auto sender = item->originalSender()) {
		return sender->name();
	}
	return item->from()->name();
}

[[nodiscard]] QString EscapeMarkdownLinkTarget(const QString &filename) {
	return QString::fromUtf8(QUrl::toPercentEncoding(
		filename,
		QByteArray("/")));
}

[[nodiscard]] bool IsImageLikeDocument(const DocumentData *document) {
	return document && document->isImage() && !document->isAnimation();
}

[[nodiscard]] QString BuildMarkdown(const std::vector<Entry> &entries) {
	auto out = QStringList();
	out << u"# Saved messages"_q;
	out << QString();
	for (const auto &entry : entries) {
		const auto when = base::unixtime::parse(entry.date);
		const auto header = u"## %1 — %2"_q.arg(
			entry.fromName.isEmpty() ? u"Unknown"_q : entry.fromName,
			when.toString(u"yyyy-MM-dd HH:mm:ss"_q));
		out << header;
		out << QString();
		if (!entry.text.isEmpty()) {
			out << entry.text;
			out << QString();
		}
		if (!entry.filename.isEmpty()) {
			const auto escaped = EscapeMarkdownLinkTarget(entry.filename);
			const auto asImage = entry.photo
				|| IsImageLikeDocument(entry.document);
			out << (asImage
				? u"![%1](%2)"_q.arg(entry.filename, escaped)
				: u"[%1](%2)"_q.arg(entry.filename, escaped));
			out << QString();
		}
		out << u"---"_q;
		out << QString();
	}
	return out.join('\n');
}

[[nodiscard]] bool WithinSizeLimitForItems(
		const std::vector<not_null<HistoryItem*>> &items) {
	auto total = int64(0);
	for (const auto &item : items) {
		const auto media = item->media();
		if (!media) {
			continue;
		}
		if (const auto document = media->document()) {
			if (document->size > kMaxTotalFileSize) {
				return false;
			}
			total += document->size;
			if (total > kMaxTotalFileSize) {
				return false;
			}
		}
	}
	return true;
}

[[nodiscard]] std::vector<Entry> CollectEntriesFromItems(
		const std::vector<not_null<HistoryItem*>> &items,
		base::flat_set<QString> &usedNames) {
	auto result = std::vector<Entry>();
	result.reserve(items.size());
	for (const auto &item : items) {
		auto entry = Entry{
			.fullId = item->fullId(),
			.fromName = ResolveAuthorName(item),
			.date = item->date(),
			.text = item->originalText().text,
		};
		const auto media = item->media();
		if (media) {
			if (const auto photo = media->photo()) {
				entry.photo = photo;
				entry.filename = PickPhotoName(entry.fullId, usedNames);
			} else if (const auto document = media->document()) {
				entry.document = document;
				entry.filename = PickDocumentName(
					not_null<DocumentData*>(document),
					entry.fullId,
					usedNames);
			}
		}
		result.push_back(std::move(entry));
	}
	return result;
}

void RunSaveToMarkdown(
		not_null<Window::SessionController*> controller,
		std::vector<Entry> entries) {
	if (entries.empty()) {
		return;
	}
	const auto tempRoot = QStandardPaths::writableLocation(
		QStandardPaths::TempLocation);
	if (tempRoot.isEmpty()) {
		controller->showToast(u"No temp directory available."_q);
		return;
	}
	const auto stamp = QDateTime::currentDateTime().toString(
		u"yyyyMMdd-HHmmsszzz"_q);
	const auto folder = u"%1/tdesktop-markdown-%2"_q.arg(tempRoot, stamp);
	if (!QDir().mkpath(folder)) {
		controller->showToast(u"Failed to create directory."_q);
		return;
	}

	for (auto &entry : entries) {
		if (entry.photo) {
			entry.photoView = entry.photo->createMediaView();
			entry.photoView->wanted(Data::PhotoSize::Large, entry.fullId);
		} else if (entry.document) {
			entry.document->save(
				entry.fullId,
				folder + '/' + entry.filename);
		}
	}

	const auto session = &controller->session();
	const auto weak = base::make_weak(controller);
	auto lifetime = std::make_shared<rpl::lifetime>();
	const auto shared = std::make_shared<std::vector<Entry>>(
		std::move(entries));

	const auto allReady = [shared] {
		for (const auto &entry : *shared) {
			if (entry.photo && entry.photo->loading()) {
				return false;
			}
			if (entry.document && entry.document->loading()) {
				return false;
			}
		}
		return true;
	};

	const auto finalize = [weak, folder, shared] {
		for (const auto &entry : *shared) {
			if (entry.photoView) {
				entry.photoView->saveToFile(folder + '/' + entry.filename);
			}
		}
		const auto markdown = BuildMarkdown(*shared);
		const auto mdPath = folder + u"/messages.md"_q;
		auto file = QFile(mdPath);
		if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			if (const auto strong = weak.get()) {
				strong->showToast(u"Failed to write markdown file."_q);
			}
			return;
		}
		file.write(markdown.toUtf8());
		file.close();

		const auto absolute = QFileInfo(mdPath).absoluteFilePath();
		const auto pattern = base::options::value<QString>(
			kOptionMarkdownClipboardText);
		const auto clipboardText = pattern.isEmpty()
			? absolute
			: QString(pattern).replace(u"%q"_q, absolute);
		QGuiApplication::clipboard()->setText(clipboardText);
		if (const auto strong = weak.get()) {
			strong->showToast(
				tr::lng_context_save_to_markdown_done(tr::now));
		}
	};

	if (allReady()) {
		finalize();
		return;
	}
	session->downloaderTaskFinished(
	) | rpl::on_next([=]() mutable {
		if (!allReady()) {
			return;
		}
		finalize();
		base::take(lifetime)->destroy();
	}, *lifetime);
}

} // namespace

namespace {

void AddSaveToMarkdownFileActionForItems(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> controller,
		std::vector<not_null<HistoryItem*>> items) {
	if (items.empty() || !WithinSizeLimitForItems(items)) {
		return;
	}
	ranges::sort(items, {}, &HistoryItem::fullId);
	auto usedNames = base::flat_set<QString>();
	auto entries = CollectEntriesFromItems(items, usedNames);
	if (entries.empty()) {
		return;
	}
	const auto weak = base::make_weak(controller);
	menu->addAction(
		tr::lng_context_save_to_markdown_selected(tr::now),
		[weak, entries = std::move(entries)]() mutable {
			const auto strong = weak.get();
			if (!strong) {
				return;
			}
			RunSaveToMarkdown(strong, std::move(entries));
		},
		&st::menuIconExport);
}

} // namespace

const char kOptionMarkdownClipboardText[] = "markdown-clipboard-text";

void AddSaveToMarkdownFileAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> controller,
		const base::flat_map<
			HistoryItem*,
			TextSelection,
			std::less<>> &items) {
	if (items.empty()) {
		return;
	}
	auto historyItems = std::vector<not_null<HistoryItem*>>();
	historyItems.reserve(items.size());
	for (const auto &[item, _] : items) {
		if (item) {
			historyItems.emplace_back(item);
		}
	}
	AddSaveToMarkdownFileActionForItems(
		menu,
		controller,
		std::move(historyItems));
}

void AddSaveToMarkdownFileAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> controller,
		const std::vector<HistoryView::SelectedItem> &selectedItems) {
	if (selectedItems.empty()) {
		return;
	}
	const auto session = &controller->session();
	auto historyItems = std::vector<not_null<HistoryItem*>>();
	historyItems.reserve(selectedItems.size());
	for (const auto &selected : selectedItems) {
		if (const auto item = session->data().message(selected.msgId)) {
			historyItems.emplace_back(item);
		}
	}
	AddSaveToMarkdownFileActionForItems(
		menu,
		controller,
		std::move(historyItems));
}

} // namespace Menu
