/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/mcp/mcp_tl_codec.h"

#include "base/basic_types.h"
#include "core/mcp/mcp_tl_registry.h"

#include <QtCore/QJsonArray>
#include <QtCore/QMap>
#include <QtCore/QSet>

#include <cmath>
#include <cstring>
#include <limits>

namespace Core::Mcp {
namespace {

constexpr auto kVectorConstructor = quint32(0x1cb5c415);
constexpr auto kBoolFalse = quint32(0xbc799737);
constexpr auto kBoolTrue = quint32(0x997275b5);
constexpr auto kMaxVectorSize = 1000000;

struct Conditional {
	QString flags;
	QString type;
	int bit = -1;
};

struct Reader {
	const QByteArray *bytes = nullptr;
	qsizetype offset = 0;
};

[[nodiscard]] TlCodecError Error(QString message) {
	return { .message = std::move(message) };
}

[[nodiscard]] quint32 DescriptorId(const QJsonObject &descriptor, bool *ok) {
	const auto value = descriptor.value(u"id"_q).toString().toInt(ok);
	return quint32(value);
}

void Append32(QByteArray &to, quint32 value) {
	for (auto i = 0; i != 4; ++i) {
		to.append(char((value >> (8 * i)) & 0xFF));
	}
}

void Append64(QByteArray &to, quint64 value) {
	for (auto i = 0; i != 8; ++i) {
		to.append(char((value >> (8 * i)) & 0xFF));
	}
}

[[nodiscard]] bool Read32(Reader &from, quint32 &value) {
	if (!from.bytes || from.offset + 4 > from.bytes->size()) {
		return false;
	}
	value = 0;
	for (auto i = 0; i != 4; ++i) {
		value |= quint32(uchar((*from.bytes)[from.offset + i])) << (8 * i);
	}
	from.offset += 4;
	return true;
}

[[nodiscard]] bool Read64(Reader &from, quint64 &value) {
	if (!from.bytes || from.offset + 8 > from.bytes->size()) {
		return false;
	}
	value = 0;
	for (auto i = 0; i != 8; ++i) {
		value |= quint64(uchar((*from.bytes)[from.offset + i])) << (8 * i);
	}
	from.offset += 8;
	return true;
}

void AppendTlBytes(QByteArray &to, const QByteArray &value) {
	const auto before = to.size();
	if (value.size() < 254) {
		to.append(char(value.size()));
	} else {
		to.append(char(254));
		to.append(char(value.size() & 0xFF));
		to.append(char((value.size() >> 8) & 0xFF));
		to.append(char((value.size() >> 16) & 0xFF));
	}
	to.append(value);
	while ((to.size() - before) % 4) {
		to.append(char(0));
	}
}

[[nodiscard]] bool ReadTlBytes(Reader &from, QByteArray &result) {
	if (!from.bytes || from.offset >= from.bytes->size()) {
		return false;
	}
	const auto before = from.offset;
	auto length = int(uchar((*from.bytes)[from.offset++]));
	if (length == 254) {
		if (from.offset + 3 > from.bytes->size()) {
			return false;
		}
		length = int(uchar((*from.bytes)[from.offset]))
			| (int(uchar((*from.bytes)[from.offset + 1])) << 8)
			| (int(uchar((*from.bytes)[from.offset + 2])) << 16);
		from.offset += 3;
	}
	if (length < 0 || from.offset + length > from.bytes->size()) {
		return false;
	}
	result = from.bytes->mid(from.offset, length);
	from.offset += length;
	while ((from.offset - before) % 4) {
		if (from.offset >= from.bytes->size()
			|| (*from.bytes)[from.offset] != char(0)) {
			return false;
		}
		++from.offset;
	}
	return true;
}

[[nodiscard]] Conditional ParseConditional(const QString &type) {
	const auto question = type.indexOf('?');
	if (question <= 0) {
		return {};
	}
	const auto condition = type.left(question);
	const auto dot = condition.lastIndexOf('.');
	if (dot <= 0) {
		return {};
	}
	auto ok = false;
	const auto bit = condition.mid(dot + 1).toInt(&ok);
	return (ok && bit >= 0 && bit < 32)
		? Conditional{
			.flags = condition.left(dot),
			.type = type.mid(question + 1),
			.bit = bit,
		}
		: Conditional();
}

[[nodiscard]] bool ParseFixedInteger(
		QString value,
		int size,
		QByteArray &result) {
	const auto negative = value.startsWith('-');
	if (negative) {
		value.remove(0, 1);
	}
	if (value.isEmpty()) {
		return false;
	}
	result = QByteArray(size, char(0));
	for (const auto ch : value) {
		if (!ch.isDigit()) {
			return false;
		}
		auto carry = ch.unicode() - u'0';
		for (auto i = 0; i != size; ++i) {
			const auto next = (uchar(result[i]) * 10) + carry;
			result[i] = char(next & 0xFF);
			carry = next >> 8;
		}
		if (carry) {
			return false;
		}
	}
	const auto high = uchar(result.back());
	if (!negative && (high & 0x80)) {
		return false;
	} else if (negative && high >= 0x80) {
		if (high != 0x80) {
			return false;
		}
		for (auto i = 0; i != size - 1; ++i) {
			if (result[i] != char(0)) {
				return false;
			}
		}
	}
	if (negative) {
		auto carry = 1;
		for (auto i = 0; i != size; ++i) {
			const auto next = (uchar(result[i]) ^ 0xFF) + carry;
			result[i] = char(next & 0xFF);
			carry = next >> 8;
		}
	}
	return true;
}

[[nodiscard]] QString FixedIntegerToString(QByteArray value) {
	const auto negative = !value.isEmpty() && (uchar(value.back()) & 0x80);
	if (negative) {
		auto carry = 1;
		for (auto i = 0; i != value.size(); ++i) {
			const auto next = (uchar(value[i]) ^ 0xFF) + carry;
			value[i] = char(next & 0xFF);
			carry = next >> 8;
		}
	}
	auto digits = std::vector<int>{ 0 };
	for (auto i = value.size(); i != 0; --i) {
		auto carry = int(uchar(value[i - 1]));
		for (auto &digit : digits) {
			const auto next = (digit * 256) + carry;
			digit = next % 10;
			carry = next / 10;
		}
		while (carry) {
			digits.push_back(carry % 10);
			carry /= 10;
		}
	}
	auto result = negative ? u"-"_q : QString();
	for (auto i = digits.rbegin(); i != digits.rend(); ++i) {
		result.append(QChar(u'0' + *i));
	}
	return result;
}

class Encoder final {
public:
	explicit Encoder(not_null<const TlRegistry*> registry)
	: _registry(registry) {
	}

	[[nodiscard]] TlEncodeResult method(
			const QString &name,
			const QJsonObject &params) {
		const auto descriptor = _registry->describeMethod(name);
		if (descriptor.isEmpty()) {
			return Error(u"Unknown TL method: "_q + name);
		}
		auto bytes = QByteArray();
		auto genericType = QString();
		if (!descriptorValue(descriptor, params, bytes, genericType, true)) {
			return _error;
		}
		auto resultType = descriptor.value(u"type"_q).toString();
		if (resultType == u"X"_q && !genericType.isEmpty()) {
			resultType = std::move(genericType);
		}
		return TlEncodedRequest{
			.bytes = std::move(bytes),
			.resultType = std::move(resultType),
		};
	}

private:
	[[nodiscard]] bool descriptorValue(
			const QJsonObject &descriptor,
			const QJsonObject &value,
			QByteArray &to,
			QString &genericType,
			bool method) {
		auto idOk = false;
		const auto id = DescriptorId(descriptor, &idOk);
		if (!idOk) {
			return fail(u"TL descriptor has an invalid id."_q);
		}
		Append32(to, id);
		const auto params = descriptor.value(u"params"_q).toArray();
		auto allowed = QSet<QString>();
		if (!method) {
			allowed.insert(u"_"_q);
		}
		auto flags = QMap<QString, quint32>();
		for (const auto &entry : params) {
			const auto param = entry.toObject();
			const auto name = param.value(u"name"_q).toString();
			const auto type = param.value(u"type"_q).toString();
			if (type != u"#"_q) {
				allowed.insert(name);
			}
			const auto conditional = ParseConditional(type);
			if (conditional.bit < 0 || !value.contains(name)) {
				continue;
			}
			const auto supplied = value.value(name);
			if (!supplied.isNull()
				&& (conditional.type != u"true"_q || supplied.toBool())) {
				flags[conditional.flags]
					|= (quint32(1) << conditional.bit);
			}
		}
		for (auto i = value.begin(); i != value.end(); ++i) {
			if (!allowed.contains(i.key())) {
				return fail(u"Unknown TL field: "_q + i.key());
			}
		}
		for (const auto &entry : params) {
			const auto param = entry.toObject();
			const auto name = param.value(u"name"_q).toString();
			const auto type = param.value(u"type"_q).toString();
			if (type == u"#"_q) {
				Append32(to, flags.value(name));
				continue;
			}
			const auto conditional = ParseConditional(type);
			if (conditional.bit >= 0) {
				if (!(flags.value(conditional.flags)
						& (quint32(1) << conditional.bit))) {
					continue;
				} else if (conditional.type == u"true"_q) {
					continue;
				} else if (!value.contains(name) || value.value(name).isNull()) {
					return fail(u"Missing TL field sharing a flag: "_q + name);
				} else if (!encodeValue(
						conditional.type,
						value.value(name),
						to,
						genericType)) {
					return false;
				}
				continue;
			}
			if (!value.contains(name)) {
				return fail(u"Missing required TL field: "_q + name);
			}
			if (!encodeValue(type, value.value(name), to, genericType)) {
				return false;
			}
		}
		return true;
	}

	[[nodiscard]] bool encodeValue(
			QString type,
			const QJsonValue &value,
			QByteArray &to,
			QString &genericType) {
		if (type.startsWith('%')) {
			type.remove(0, 1);
		}
		if (type == u"int"_q) {
			if (!value.isDouble()
				|| std::trunc(value.toDouble()) != value.toDouble()
				|| value.toDouble() < std::numeric_limits<qint32>::min()
				|| value.toDouble() > std::numeric_limits<qint32>::max()) {
				return fail(u"TL int requires a 32-bit JSON integer."_q);
			}
			Append32(to, quint32(qint32(value.toDouble())));
			return true;
		} else if (type == u"long"_q) {
			auto ok = false;
			const auto number = value.toString().toLongLong(&ok);
			if (!ok) {
				return fail(u"TL long requires a decimal string."_q);
			}
			Append64(to, quint64(number));
			return true;
		} else if (type == u"double"_q) {
			if (!value.isDouble()) {
				return fail(u"TL double requires a JSON number."_q);
			}
			const auto number = value.toDouble();
			auto bits = quint64();
			std::memcpy(&bits, &number, sizeof(bits));
			Append64(to, bits);
			return true;
		} else if (type == u"string"_q) {
			if (!value.isString()) {
				return fail(u"TL string requires a JSON string."_q);
			}
			AppendTlBytes(to, value.toString().toUtf8());
			return true;
		} else if (type == u"bytes"_q) {
			if (!value.isString()) {
				return fail(u"TL bytes require a base64 string."_q);
			}
			const auto decoded = QByteArray::fromBase64Encoding(
				value.toString().toLatin1(),
				QByteArray::AbortOnBase64DecodingErrors);
			if (!decoded) {
				return fail(u"TL bytes require valid base64."_q);
			}
			AppendTlBytes(to, decoded.decoded);
			return true;
		} else if (type == u"int128"_q || type == u"int256"_q) {
			auto encoded = QByteArray();
			if (!value.isString() || !ParseFixedInteger(
					value.toString(),
					(type == u"int128"_q) ? 16 : 32,
					encoded)) {
				return fail(type + u" requires a fitting decimal string."_q);
			}
			to.append(encoded);
			return true;
		} else if (type == u"Bool"_q) {
			if (!value.isBool()) {
				return fail(u"TL Bool requires a JSON boolean."_q);
			}
			Append32(to, value.toBool() ? kBoolTrue : kBoolFalse);
			return true;
		} else if (type == u"true"_q) {
			return value.toBool() || fail(u"TL true requires true."_q);
		} else if (type.startsWith(u"Vector<"_q) && type.endsWith('>')) {
			if (!value.isArray()) {
				return fail(u"TL Vector requires a JSON array."_q);
			}
			const auto array = value.toArray();
			if (array.size() > kMaxVectorSize) {
				return fail(u"TL Vector is too large."_q);
			}
			Append32(to, kVectorConstructor);
			Append32(to, array.size());
			const auto inner = type.mid(7, type.size() - 8);
			for (const auto &item : array) {
				if (!encodeValue(inner, item, to, genericType)) {
					return false;
				}
			}
			return true;
		} else if (type.startsWith('!')) {
			if (!value.isObject()) {
				return fail(u"Generic TL query requires an object."_q);
			}
			const auto query = value.toObject();
			const auto method = query.value(u"_method"_q).toString();
			const auto params = query.value(u"params"_q).toObject();
			const auto encoded = this->method(method, params);
			if (const auto error = std::get_if<TlCodecError>(&encoded)) {
				return fail(error->message);
			}
			const auto &request = std::get<TlEncodedRequest>(encoded);
			to.append(request.bytes);
			genericType = request.resultType;
			return true;
		}
		if (!value.isObject()) {
			return fail(u"TL object requires a constructor object."_q);
		}
		const auto object = value.toObject();
		const auto predicate = object.value(u"_"_q).toString();
		const auto descriptor = _registry->describeConstructor(predicate);
		if (descriptor.isEmpty()) {
			return fail(u"Unknown TL constructor: "_q + predicate);
		} else if (descriptor.value(u"type"_q).toString() != type) {
			return fail(u"TL constructor has the wrong result type."_q);
		}
		return descriptorValue(descriptor, object, to, genericType, false);
	}

	[[nodiscard]] bool fail(QString message) {
		_error = Error(std::move(message));
		return false;
	}

	const not_null<const TlRegistry*> _registry;
	TlCodecError _error;

};

class Decoder final {
public:
	explicit Decoder(not_null<const TlRegistry*> registry)
	: _registry(registry) {
	}

	[[nodiscard]] TlDecodeResult decode(
			const QString &type,
			const QByteArray &bytes) {
		auto reader = Reader{ .bytes = &bytes };
		auto value = QJsonValue();
		if (!decodeValue(type, reader, value)) {
			return _error;
		} else if (reader.offset != bytes.size()) {
			return Error(u"TL response has trailing bytes."_q);
		}
		return value;
	}

private:
	[[nodiscard]] bool decodeValue(
			QString type,
			Reader &from,
			QJsonValue &result) {
		if (type.startsWith('%')) {
			type.remove(0, 1);
		}
		if (type == u"int"_q) {
			auto value = quint32();
			if (!Read32(from, value)) {
				return fail(u"Truncated TL int."_q);
			}
			result = qint32(value);
			return true;
		} else if (type == u"long"_q) {
			auto value = quint64();
			if (!Read64(from, value)) {
				return fail(u"Truncated TL long."_q);
			}
			result = QString::number(qint64(value));
			return true;
		} else if (type == u"double"_q) {
			auto bits = quint64();
			if (!Read64(from, bits)) {
				return fail(u"Truncated TL double."_q);
			}
			auto value = double();
			std::memcpy(&value, &bits, sizeof(value));
			result = value;
			return true;
		} else if (type == u"string"_q || type == u"bytes"_q) {
			auto value = QByteArray();
			if (!ReadTlBytes(from, value)) {
				return fail(u"Truncated TL bytes."_q);
			}
			result = (type == u"string"_q)
				? QJsonValue(QString::fromUtf8(value))
				: QJsonValue(QString::fromLatin1(value.toBase64()));
			return true;
		} else if (type == u"int128"_q || type == u"int256"_q) {
			const auto size = (type == u"int128"_q) ? 16 : 32;
			if (!from.bytes || from.offset + size > from.bytes->size()) {
				return fail(u"Truncated fixed TL integer."_q);
			}
			result = FixedIntegerToString(from.bytes->mid(from.offset, size));
			from.offset += size;
			return true;
		} else if (type == u"true"_q) {
			result = true;
			return true;
		} else if (type.startsWith(u"Vector<"_q) && type.endsWith('>')) {
			auto constructor = quint32();
			auto count = quint32();
			if (!Read32(from, constructor)
				|| constructor != kVectorConstructor
				|| !Read32(from, count)
				|| count > kMaxVectorSize) {
				return fail(u"Invalid TL Vector."_q);
			}
			const auto inner = type.mid(7, type.size() - 8);
			auto array = QJsonArray();
			for (auto i = quint32(); i != count; ++i) {
				auto item = QJsonValue();
				if (!decodeValue(inner, from, item)) {
					return false;
				}
				array.append(std::move(item));
			}
			result = std::move(array);
			return true;
		}
		auto constructor = quint32();
		if (!Read32(from, constructor)) {
			return fail(u"Truncated TL constructor."_q);
		}
		if (type == u"Bool"_q) {
			if (constructor == kBoolTrue) {
				result = true;
				return true;
			} else if (constructor == kBoolFalse) {
				result = false;
				return true;
			}
			return fail(u"Invalid TL Bool constructor."_q);
		}
		const auto descriptor = _registry->describeConstructor(constructor);
		if (descriptor.isEmpty()
			|| descriptor.value(u"type"_q).toString() != type) {
			return fail(u"Unexpected TL constructor."_q);
		}
		auto object = QJsonObject{
			{ u"_"_q, descriptor.value(u"predicate"_q) },
		};
		auto flags = QMap<QString, quint32>();
		for (const auto &entry : descriptor.value(u"params"_q).toArray()) {
			const auto param = entry.toObject();
			const auto name = param.value(u"name"_q).toString();
			const auto paramType = param.value(u"type"_q).toString();
			if (paramType == u"#"_q) {
				auto value = quint32();
				if (!Read32(from, value)) {
					return fail(u"Truncated TL flags."_q);
				}
				flags.insert(name, value);
				continue;
			}
			const auto conditional = ParseConditional(paramType);
			if (conditional.bit >= 0) {
				if (!(flags.value(conditional.flags)
						& (quint32(1) << conditional.bit))) {
					continue;
				} else if (conditional.type == u"true"_q) {
					object.insert(name, true);
					continue;
				}
			}
			auto value = QJsonValue();
			if (!decodeValue(
					(conditional.bit >= 0) ? conditional.type : paramType,
					from,
					value)) {
				return false;
			}
			object.insert(name, std::move(value));
		}
		result = std::move(object);
		return true;
	}

	[[nodiscard]] bool fail(QString message) {
		_error = Error(std::move(message));
		return false;
	}

	const not_null<const TlRegistry*> _registry;
	TlCodecError _error;

};

} // namespace

TlCodec::TlCodec(const TlRegistry *registry)
: _registry(registry) {
	Expects(_registry != nullptr);
}

TlEncodeResult TlCodec::encodeMethod(
		const QString &method,
		const QJsonObject &params) const {
	return Encoder(_registry).method(method, params);
}

TlDecodeResult TlCodec::decode(
		const QString &type,
		const QByteArray &bytes) const {
	return Decoder(_registry).decode(type, bytes);
}

} // namespace Core::Mcp
