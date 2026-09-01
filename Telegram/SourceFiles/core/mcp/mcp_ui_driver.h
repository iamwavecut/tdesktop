/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "core/mcp/mcp_dispatcher.h"

#include <QtCore/QHash>
#include <QtCore/QMap>
#include <QtCore/QPointer>

class QWidget;

namespace Core {

class Application;

namespace Mcp {

class UiDriver final {
public:
	UiDriver(
		not_null<Application*> application,
		not_null<Dispatcher*> dispatcher);

private:
	struct WindowEntry {
		QPointer<QWidget> widget;
		quintptr raw = 0;
		quint64 generation = 0;
	};

	void registerTools();
	void refreshWindows();
	[[nodiscard]] QString handle(not_null<QWidget*> widget);
	[[nodiscard]] QWidget *resolve(const QString &handle);
	[[nodiscard]] ToolResult error(QString type, QString description) const;
	[[nodiscard]] QJsonObject windowJson(
		const QString &handle,
		not_null<QWidget*> widget) const;
	[[nodiscard]] QJsonObject widgetJson(
		not_null<QWidget*> window,
		not_null<QWidget*> widget,
		int id,
		int parentId) const;
	void snapshotChildren(
		not_null<QWidget*> window,
		not_null<QWidget*> parent,
		int parentId,
		int depth,
		int maxDepth,
		int maxNodes,
		QJsonArray &result) const;
	void completeLater(ToolCompletion done, QJsonObject result) const;

	const not_null<Application*> _application;
	const not_null<Dispatcher*> _dispatcher;
	QMap<quint64, WindowEntry> _windows;
	QHash<quintptr, quint64> _idByRaw;
	quint64 _nextId = 0;

};

} // namespace Mcp
} // namespace Core
