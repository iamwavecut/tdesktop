/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QMap>

namespace Core::Mcp {

class TlRegistry final {
public:
	explicit TlRegistry(const QByteArray &schema);

	[[nodiscard]] bool valid() const;
	[[nodiscard]] int layer() const;
	[[nodiscard]] int constructorCount() const;
	[[nodiscard]] int methodCount() const;
	[[nodiscard]] QJsonObject describeMethod(const QString &name) const;
	[[nodiscard]] QJsonObject describeConstructor(const QString &name) const;
	[[nodiscard]] QJsonObject describeConstructor(quint32 id) const;
	[[nodiscard]] const QJsonObject &schema() const;

private:
	QJsonObject _schema;
	QMap<QString, QJsonObject> _methods;
	QMap<QString, QJsonObject> _constructors;
	QMap<quint32, QJsonObject> _constructorsById;
	int _layer = 0;
	int _constructorCount = 0;
	bool _valid = false;

};

} // namespace Core::Mcp
