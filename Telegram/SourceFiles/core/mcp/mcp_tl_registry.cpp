/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/mcp/mcp_tl_registry.h"

#include "base/basic_types.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>

namespace Core::Mcp {

TlRegistry::TlRegistry(const QByteArray &schema) {
	const auto document = QJsonDocument::fromJson(schema);
	if (!document.isObject()) {
		return;
	}
	_schema = document.object();
	_layer = _schema.value(u"layer"_q).toInt();
	const auto constructors = _schema.value(u"constructors"_q).toArray();
	const auto methods = _schema.value(u"methods"_q).toArray();
	for (const auto &value : constructors) {
		const auto object = value.toObject();
		const auto name = object.value(u"predicate"_q).toString();
		auto idOk = false;
		const auto signedId = object.value(u"id"_q).toString().toInt(&idOk);
		const auto id = quint32(signedId);
		if (name.isEmpty()
			|| !idOk
			|| _constructors.contains(name)
			|| _constructorsById.contains(id)) {
			_constructors.clear();
			_constructorsById.clear();
			return;
		}
		_constructors.insert(name, object);
		_constructorsById.insert(id, object);
	}
	_constructorCount = _constructors.size();
	for (const auto &value : methods) {
		const auto object = value.toObject();
		const auto name = object.value(u"method"_q).toString();
		if (name.isEmpty() || _methods.contains(name)) {
			_methods.clear();
			return;
		}
		_methods.insert(name, object);
	}
	_valid = (_layer > 0)
		&& (_constructorCount > 0)
		&& !_methods.isEmpty();
}

bool TlRegistry::valid() const {
	return _valid;
}

int TlRegistry::layer() const {
	return _layer;
}

int TlRegistry::constructorCount() const {
	return _constructorCount;
}

int TlRegistry::methodCount() const {
	return _methods.size();
}

QJsonObject TlRegistry::describeMethod(const QString &name) const {
	return _methods.value(name);
}

QJsonObject TlRegistry::describeConstructor(const QString &name) const {
	return _constructors.value(name);
}

QJsonObject TlRegistry::describeConstructor(quint32 id) const {
	return _constructorsById.value(id);
}

const QJsonObject &TlRegistry::schema() const {
	return _schema;
}

} // namespace Core::Mcp
