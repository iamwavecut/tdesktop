/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class HistoryItem;

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace HistoryView {
struct SelectedItem;
} // namespace HistoryView

namespace Window {
class SessionController;
} // namespace Window

namespace Menu {

extern const char kOptionMarkdownClipboardText[];

void AddSaveToMarkdownFileAction(
	not_null<Ui::PopupMenu*> menu,
	not_null<Window::SessionController*> controller,
	const base::flat_map<HistoryItem*, TextSelection, std::less<>> &items);

void AddSaveToMarkdownFileAction(
	not_null<Ui::PopupMenu*> menu,
	not_null<Window::SessionController*> controller,
	const std::vector<HistoryView::SelectedItem> &selectedItems);

} // namespace Menu
