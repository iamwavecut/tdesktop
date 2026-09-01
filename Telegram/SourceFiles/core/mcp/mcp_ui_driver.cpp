/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/mcp/mcp_ui_driver.h"

#include "core/application.h"
#include "core/core_screenshot_protection.h"
#include "window/window_controller.h"

#include <QtCore/QBuffer>
#include <QtCore/QJsonArray>
#include <QtCore/QTimer>
#include <QtGui/QInputMethodEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

namespace Core::Mcp {
namespace {

[[nodiscard]] QJsonObject EmptySchema() {
	return {
		{ u"type"_q, u"object"_q },
		{ u"additionalProperties"_q, false },
	};
}

[[nodiscard]] QJsonObject ObjectSchema() {
	return { { u"type"_q, u"object"_q } };
}

[[nodiscard]] QJsonObject HandleSchema(
		QJsonObject extra = {},
		QJsonArray required = {}) {
	extra.insert(u"window_handle"_q, QJsonObject{
		{ u"type"_q, u"string"_q },
	});
	required.prepend(u"window_handle"_q);
	return {
		{ u"type"_q, u"object"_q },
		{ u"properties"_q, std::move(extra) },
		{ u"required"_q, std::move(required) },
		{ u"additionalProperties"_q, false },
	};
}

[[nodiscard]] QJsonObject PointProperties() {
	return {
		{ u"x"_q, QJsonObject{ { u"type"_q, u"integer"_q } } },
		{ u"y"_q, QJsonObject{ { u"type"_q, u"integer"_q } } },
	};
}

[[nodiscard]] Qt::KeyboardModifiers Modifiers(const QJsonArray &values) {
	auto result = Qt::KeyboardModifiers();
	for (const auto &value : values) {
		const auto name = value.toString().toLower();
		if (name == u"shift"_q) {
			result |= Qt::ShiftModifier;
		} else if (name == u"ctrl"_q || name == u"control"_q) {
			result |= Qt::ControlModifier;
		} else if (name == u"alt"_q) {
			result |= Qt::AltModifier;
		} else if (name == u"meta"_q || name == u"command"_q) {
			result |= Qt::MetaModifier;
		}
	}
	return result;
}

[[nodiscard]] int Key(const QString &name) {
	static const auto values = QMap<QString, int>{
		{ u"backspace"_q, Qt::Key_Backspace },
		{ u"delete"_q, Qt::Key_Delete },
		{ u"down"_q, Qt::Key_Down },
		{ u"enter"_q, Qt::Key_Return },
		{ u"escape"_q, Qt::Key_Escape },
		{ u"left"_q, Qt::Key_Left },
		{ u"return"_q, Qt::Key_Return },
		{ u"right"_q, Qt::Key_Right },
		{ u"space"_q, Qt::Key_Space },
		{ u"tab"_q, Qt::Key_Tab },
		{ u"up"_q, Qt::Key_Up },
	};
	const auto lowered = name.toLower();
	if (const auto i = values.find(lowered); i != values.end()) {
		return i.value();
	}
	return (name.size() == 1) ? name.front().toUpper().unicode() : 0;
}

} // namespace

UiDriver::UiDriver(
		not_null<Application*> application,
		not_null<Dispatcher*> dispatcher)
: _application(application)
, _dispatcher(dispatcher) {
	registerTools();
}

void UiDriver::refreshWindows() {
	for (auto i = _windows.begin(); i != _windows.end();) {
		if (!i->widget || !i->widget->isVisible()) {
			_idByRaw.remove(i->raw);
			i = _windows.erase(i);
		} else {
			++i;
		}
	}
	for (const auto widget : QApplication::topLevelWidgets()) {
		if (!widget->isVisible()) {
			continue;
		}
		const auto raw = quintptr(widget);
		if (!_idByRaw.contains(raw)) {
			const auto id = ++_nextId;
			_idByRaw.insert(raw, id);
			_windows.insert(id, {
				.widget = widget,
				.raw = raw,
				.generation = id,
			});
		}
	}
}

QString UiDriver::handle(not_null<QWidget*> widget) {
	refreshWindows();
	const auto id = _idByRaw.value(quintptr(widget.get()));
	const auto i = _windows.find(id);
	return (i != _windows.end())
		? u"%1:%2"_q.arg(id).arg(i->generation)
		: QString();
}

QWidget *UiDriver::resolve(const QString &handle) {
	refreshWindows();
	const auto parts = handle.split(':');
	if (parts.size() != 2) {
		return nullptr;
	}
	auto idOk = false;
	auto generationOk = false;
	const auto id = parts[0].toULongLong(&idOk);
	const auto generation = parts[1].toULongLong(&generationOk);
	const auto i = _windows.find(id);
	return (idOk
		&& generationOk
		&& i != _windows.end()
		&& i->generation == generation)
		? i->widget.data()
		: nullptr;
}

ToolResult UiDriver::error(QString type, QString description) const {
	return {
		.structuredContent = QJsonObject{
			{ u"error"_q, QJsonObject{
				{ u"type"_q, std::move(type) },
				{ u"description"_q, std::move(description) },
			} },
		},
		.isError = true,
	};
}

QJsonObject UiDriver::windowJson(
		const QString &handle,
		not_null<QWidget*> widget) const {
	const auto controller = _application->findWindow(widget);
	const auto geometry = widget->geometry();
	return {
		{ u"window_handle"_q, handle },
		{ u"title"_q, widget->windowTitle() },
		{ u"x"_q, geometry.x() },
		{ u"y"_q, geometry.y() },
		{ u"width"_q, geometry.width() },
		{ u"height"_q, geometry.height() },
		{ u"active"_q, widget->isActiveWindow() },
		{ u"primary"_q, controller ? controller->isPrimary() : false },
	};
}

QJsonObject UiDriver::widgetJson(
		not_null<QWidget*> window,
		not_null<QWidget*> widget,
		int id,
		int parentId) const {
	const auto topLeft = widget->mapTo(window, QPoint());
	const auto text = widget->property("text");
	auto result = QJsonObject{
		{ u"node_id"_q, id },
		{ u"class"_q, QString::fromLatin1(widget->metaObject()->className()) },
		{ u"object_name"_q, widget->objectName() },
		{ u"accessible_name"_q, widget->accessibleName() },
		{ u"accessible_description"_q, widget->accessibleDescription() },
		{ u"x"_q, topLeft.x() },
		{ u"y"_q, topLeft.y() },
		{ u"width"_q, widget->width() },
		{ u"height"_q, widget->height() },
		{ u"enabled"_q, widget->isEnabled() },
		{ u"focused"_q, widget->hasFocus() },
	};
	if (parentId >= 0) {
		result.insert(u"parent_id"_q, parentId);
	}
	if (text.canConvert<QString>()) {
		result.insert(u"text"_q, text.toString());
	}
	return result;
}

void UiDriver::snapshotChildren(
		not_null<QWidget*> window,
		not_null<QWidget*> parent,
		int parentId,
		int depth,
		int maxDepth,
		int maxNodes,
		QJsonArray &result) const {
	if (depth >= maxDepth || result.size() >= maxNodes) {
		return;
	}
	for (const auto child : parent->findChildren<QWidget*>(
			QString(),
			Qt::FindDirectChildrenOnly)) {
		if (!child->isVisible() || result.size() >= maxNodes) {
			continue;
		}
		const auto id = result.size();
		result.append(widgetJson(window, child, id, parentId));
		snapshotChildren(
			window,
			child,
			id,
			depth + 1,
			maxDepth,
			maxNodes,
			result);
	}
}

void UiDriver::completeLater(ToolCompletion done, QJsonObject result) const {
	QTimer::singleShot(0, _application, [
		done = std::move(done),
		result = std::move(result)
	]() mutable {
		done({ .structuredContent = std::move(result) });
	});
}

void UiDriver::registerTools() {
	_dispatcher->addTool({
		.name = u"telegram.ui.windows"_q,
		.description = u"List visible Forkgram windows."_q,
		.inputSchema = EmptySchema(),
		.outputSchema = ObjectSchema(),
		.handler = [=](const QJsonObject &, ToolCompletion done) {
			if (_application->passcodeLocked()) {
				done(error(u"APP_LOCKED"_q, u"Forkgram is locked."_q));
				return;
			}
			refreshWindows();
			auto windows = QJsonArray();
			for (auto i = _windows.begin(); i != _windows.end(); ++i) {
				if (i->widget) {
					windows.append(windowJson(
						u"%1:%2"_q.arg(i.key()).arg(i->generation),
						i->widget.data()));
				}
			}
			done({ .structuredContent = QJsonObject{
				{ u"windows"_q, std::move(windows) },
			} });
		},
	});
	_dispatcher->addTool({
		.name = u"telegram.ui.snapshot"_q,
		.description = u"Return the visible QWidget tree for a window."_q,
		.inputSchema = HandleSchema(),
		.outputSchema = ObjectSchema(),
		.handler = [=](const QJsonObject &arguments, ToolCompletion done) {
			const auto window = resolve(arguments.value(
				u"window_handle"_q).toString());
			if (_application->passcodeLocked()) {
				done(error(u"APP_LOCKED"_q, u"Forkgram is locked."_q));
			} else if (!window) {
				done(error(u"STALE_WINDOW"_q, u"Window handle is stale."_q));
			} else {
				auto nodes = QJsonArray{
					widgetJson(window, window, 0, -1),
				};
				snapshotChildren(window, window, 0, 0, 8, 1000, nodes);
				done({ .structuredContent = QJsonObject{
					{ u"window_handle"_q, handle(window) },
					{ u"nodes"_q, std::move(nodes) },
				} });
			}
		},
	});
	_dispatcher->addTool({
		.name = u"telegram.ui.screenshot"_q,
		.description = u"Capture a visible Forkgram window as PNG."_q,
		.inputSchema = HandleSchema(),
		.outputSchema = ObjectSchema(),
		.handler = [=](const QJsonObject &arguments, ToolCompletion done) {
			const auto window = resolve(arguments.value(
				u"window_handle"_q).toString());
			if (_application->passcodeLocked()) {
				done(error(u"APP_LOCKED"_q, u"Forkgram is locked."_q));
			} else if (_application->screenshotProtection().active()) {
				done(error(
					u"SCREENSHOT_PROTECTED"_q,
					u"The visible content is protected from screenshots."_q));
			} else if (!window) {
				done(error(u"STALE_WINDOW"_q, u"Window handle is stale."_q));
			} else {
				auto png = QByteArray();
				auto buffer = QBuffer(&png);
				buffer.open(QIODevice::WriteOnly);
				window->grab().save(&buffer, "PNG");
				done({
					.structuredContent = QJsonObject{
						{ u"window_handle"_q, handle(window) },
						{ u"width"_q, window->width() },
						{ u"height"_q, window->height() },
						{ u"device_pixel_ratio"_q, window->devicePixelRatioF() },
					},
					.content = QJsonArray{ QJsonObject{
						{ u"type"_q, u"image"_q },
						{ u"data"_q, QString::fromLatin1(png.toBase64()) },
						{ u"mimeType"_q, u"image/png"_q },
					} },
				});
			}
		},
	});
	const auto pointSchema = HandleSchema(
		PointProperties(),
		{ u"x"_q, u"y"_q });
	_dispatcher->addTool({
		.name = u"telegram.ui.hit_test"_q,
		.description = u"Describe the visible widget at window coordinates."_q,
		.inputSchema = pointSchema,
		.outputSchema = ObjectSchema(),
		.handler = [=](const QJsonObject &arguments, ToolCompletion done) {
			const auto window = resolve(arguments.value(
				u"window_handle"_q).toString());
			const auto point = QPoint(
				arguments.value(u"x"_q).toInt(),
				arguments.value(u"y"_q).toInt());
			if (_application->passcodeLocked()) {
				done(error(u"APP_LOCKED"_q, u"Forkgram is locked."_q));
			} else if (!window) {
				done(error(u"STALE_WINDOW"_q, u"Window handle is stale."_q));
			} else if (!window->rect().contains(point)) {
				done(error(u"OUT_OF_BOUNDS"_q, u"Point is outside the window."_q));
			} else {
				const auto target = window->childAt(point);
				done({ .structuredContent = widgetJson(
					window,
					target ? target : window,
					0,
					-1) });
			}
		},
	});
	_dispatcher->addTool({
		.name = u"telegram.ui.click"_q,
		.description = u"Send a left click at window coordinates."_q,
		.inputSchema = pointSchema,
		.outputSchema = ObjectSchema(),
		.handler = [=](const QJsonObject &arguments, ToolCompletion done) {
			const auto window = resolve(arguments.value(
				u"window_handle"_q).toString());
			const auto point = QPoint(
				arguments.value(u"x"_q).toInt(),
				arguments.value(u"y"_q).toInt());
			if (_application->passcodeLocked()) {
				done(error(u"APP_LOCKED"_q, u"Forkgram is locked."_q));
			} else if (!window) {
				done(error(u"STALE_WINDOW"_q, u"Window handle is stale."_q));
			} else if (!window->rect().contains(point)) {
				done(error(u"OUT_OF_BOUNDS"_q, u"Point is outside the window."_q));
			} else {
				const auto target = window->childAt(point);
				const auto widget = QPointer<QWidget>(target ? target : window);
				const auto windowHandle = handle(window);
				const auto local = QPointF(widget->mapFrom(window, point));
				const auto global = QPointF(widget->mapToGlobal(local.toPoint()));
				auto press = QMouseEvent(
					QEvent::MouseButtonPress,
					local,
					global,
					Qt::LeftButton,
					Qt::LeftButton,
					Qt::NoModifier);
				QApplication::sendEvent(widget, &press);
				if (widget) {
					auto release = QMouseEvent(
						QEvent::MouseButtonRelease,
						local,
						global,
						Qt::LeftButton,
						Qt::NoButton,
						Qt::NoModifier);
					QApplication::sendEvent(widget, &release);
				}
				auto result = QJsonObject{
					{ u"window_handle"_q, windowHandle },
					{ u"clicked"_q, true },
				};
				if (!widget) {
					result.insert(u"target_destroyed"_q, true);
				}
				completeLater(std::move(done), std::move(result));
			}
		},
	});
	_dispatcher->addTool({
		.name = u"telegram.ui.type"_q,
		.description = u"Commit text to the focused widget in a window."_q,
		.inputSchema = HandleSchema({
			{ u"text"_q, QJsonObject{ { u"type"_q, u"string"_q } } },
		}, { u"text"_q }),
		.outputSchema = ObjectSchema(),
		.handler = [=](const QJsonObject &arguments, ToolCompletion done) {
			const auto window = resolve(arguments.value(
				u"window_handle"_q).toString());
			const auto target = window ? window->focusWidget() : nullptr;
			if (_application->passcodeLocked()) {
				done(error(u"APP_LOCKED"_q, u"Forkgram is locked."_q));
			} else if (!window) {
				done(error(u"STALE_WINDOW"_q, u"Window handle is stale."_q));
			} else if (!target) {
				done(error(u"NO_FOCUS"_q, u"Window has no focused widget."_q));
			} else {
				auto event = QInputMethodEvent();
				event.setCommitString(arguments.value(u"text"_q).toString());
				const auto guarded = QPointer<QWidget>(target);
				const auto windowHandle = handle(window);
				QApplication::sendEvent(guarded, &event);
				auto result = QJsonObject{
					{ u"window_handle"_q, windowHandle },
					{ u"typed"_q, true },
				};
				if (!guarded) {
					result.insert(u"target_destroyed"_q, true);
				}
				completeLater(std::move(done), std::move(result));
			}
		},
	});
	_dispatcher->addTool({
		.name = u"telegram.ui.key"_q,
		.description = u"Send a key press and release to the focused widget."_q,
		.inputSchema = HandleSchema({
			{ u"key"_q, QJsonObject{ { u"type"_q, u"string"_q } } },
			{ u"modifiers"_q, QJsonObject{ { u"type"_q, u"array"_q } } },
		}, { u"key"_q }),
		.outputSchema = ObjectSchema(),
		.handler = [=](const QJsonObject &arguments, ToolCompletion done) {
			const auto window = resolve(arguments.value(
				u"window_handle"_q).toString());
			const auto target = window ? window->focusWidget() : nullptr;
			const auto key = Key(arguments.value(u"key"_q).toString());
			const auto modifiers = Modifiers(arguments.value(
				u"modifiers"_q).toArray());
			if (_application->passcodeLocked()) {
				done(error(u"APP_LOCKED"_q, u"Forkgram is locked."_q));
			} else if (!window) {
				done(error(u"STALE_WINDOW"_q, u"Window handle is stale."_q));
			} else if (!target) {
				done(error(u"NO_FOCUS"_q, u"Window has no focused widget."_q));
			} else if (!key) {
				done(error(u"INVALID_KEY"_q, u"Key is invalid."_q));
			} else {
				const auto guarded = QPointer<QWidget>(target);
				const auto windowHandle = handle(window);
				auto press = QKeyEvent(QEvent::KeyPress, key, modifiers);
				QApplication::sendEvent(guarded, &press);
				if (guarded) {
					auto release = QKeyEvent(QEvent::KeyRelease, key, modifiers);
					QApplication::sendEvent(guarded, &release);
				}
				auto result = QJsonObject{
					{ u"window_handle"_q, windowHandle },
					{ u"pressed"_q, true },
				};
				if (!guarded) {
					result.insert(u"target_destroyed"_q, true);
				}
				completeLater(std::move(done), std::move(result));
			}
		},
	});
	_dispatcher->addTool({
		.name = u"telegram.ui.scroll"_q,
		.description = u"Send a wheel event to a window."_q,
		.inputSchema = HandleSchema({
			{ u"delta_x"_q, QJsonObject{ { u"type"_q, u"integer"_q } } },
			{ u"delta_y"_q, QJsonObject{ { u"type"_q, u"integer"_q } } },
		}, { u"delta_x"_q, u"delta_y"_q }),
		.outputSchema = ObjectSchema(),
		.handler = [=](const QJsonObject &arguments, ToolCompletion done) {
			const auto window = resolve(arguments.value(
				u"window_handle"_q).toString());
			if (_application->passcodeLocked()) {
				done(error(u"APP_LOCKED"_q, u"Forkgram is locked."_q));
			} else if (!window) {
				done(error(u"STALE_WINDOW"_q, u"Window handle is stale."_q));
			} else {
				const auto guarded = QPointer<QWidget>(window);
				const auto windowHandle = handle(window);
				const auto local = QPointF(window->rect().center());
				const auto global = QPointF(window->mapToGlobal(local.toPoint()));
				auto event = QWheelEvent(
					local,
					global,
					QPoint(),
					QPoint(
						arguments.value(u"delta_x"_q).toInt(),
						arguments.value(u"delta_y"_q).toInt()),
					Qt::NoButton,
					Qt::NoModifier,
					Qt::NoScrollPhase,
					false,
					Qt::MouseEventSynthesizedByApplication);
				QApplication::sendEvent(guarded, &event);
				auto result = QJsonObject{
					{ u"window_handle"_q, windowHandle },
					{ u"scrolled"_q, true },
				};
				if (!guarded) {
					result.insert(u"target_destroyed"_q, true);
				}
				completeLater(std::move(done), std::move(result));
			}
		},
	});
	_dispatcher->addTool({
		.name = u"telegram.ui.drag"_q,
		.description = u"Send a left-button drag inside a Forkgram window."_q,
		.inputSchema = HandleSchema({
			{ u"from_x"_q, QJsonObject{ { u"type"_q, u"integer"_q } } },
			{ u"from_y"_q, QJsonObject{ { u"type"_q, u"integer"_q } } },
			{ u"to_x"_q, QJsonObject{ { u"type"_q, u"integer"_q } } },
			{ u"to_y"_q, QJsonObject{ { u"type"_q, u"integer"_q } } },
		}, {
			u"from_x"_q,
			u"from_y"_q,
			u"to_x"_q,
			u"to_y"_q,
		}),
		.outputSchema = ObjectSchema(),
		.handler = [=](const QJsonObject &arguments, ToolCompletion done) {
			const auto window = resolve(arguments.value(
				u"window_handle"_q).toString());
			const auto from = QPoint(
				arguments.value(u"from_x"_q).toInt(),
				arguments.value(u"from_y"_q).toInt());
			const auto to = QPoint(
				arguments.value(u"to_x"_q).toInt(),
				arguments.value(u"to_y"_q).toInt());
			if (_application->passcodeLocked()) {
				done(error(u"APP_LOCKED"_q, u"Forkgram is locked."_q));
			} else if (!window) {
				done(error(u"STALE_WINDOW"_q, u"Window handle is stale."_q));
			} else if (!window->rect().contains(from)
				|| !window->rect().contains(to)) {
				done(error(u"OUT_OF_BOUNDS"_q, u"Drag is outside the window."_q));
			} else {
				const auto target = window->childAt(from);
				const auto widget = QPointer<QWidget>(target ? target : window);
				const auto windowHandle = handle(window);
				const auto event = [&](QEvent::Type type, QPoint point, auto button) {
					const auto local = QPointF(widget->mapFrom(window, point));
					return QMouseEvent(
						type,
						local,
						QPointF(widget->mapToGlobal(local.toPoint())),
						button,
						(type == QEvent::MouseButtonRelease)
							? Qt::NoButton
							: Qt::LeftButton,
						Qt::NoModifier);
				};
				auto press = event(QEvent::MouseButtonPress, from, Qt::LeftButton);
				QApplication::sendEvent(widget, &press);
				for (auto step = 1; widget && step <= 8; ++step) {
					auto move = event(
						QEvent::MouseMove,
						from + ((to - from) * step) / 8,
						Qt::NoButton);
					QApplication::sendEvent(widget, &move);
				}
				if (widget) {
					auto release = event(
						QEvent::MouseButtonRelease,
						to,
						Qt::LeftButton);
					QApplication::sendEvent(widget, &release);
				}
				auto result = QJsonObject{
					{ u"window_handle"_q, windowHandle },
					{ u"dragged"_q, true },
				};
				if (!widget) {
					result.insert(u"target_destroyed"_q, true);
				}
				completeLater(std::move(done), std::move(result));
			}
		},
	});
}

} // namespace Core::Mcp
