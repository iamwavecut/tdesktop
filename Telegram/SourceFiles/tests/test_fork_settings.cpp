/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "base/basic_types.h"
#include "core/link_rewrite.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>

namespace {

[[noreturn]] void Fail(const QString &message) {
	qCritical().noquote() << message;
	std::exit(1);
}

void Require(bool condition, const QString &message) {
	if (!condition) {
		Fail(message);
	}
}

void TestLinkRewritePrefixNormalization() {
	Require(
		Core::NormalizeLinkRewritePrefix(
			u" Example.COM/docs/ "_q) == u"example.com/docs"_q,
		u"link rewrite prefix was not normalized"_q);
	Require(
		Core::NormalizeLinkRewritePrefix(u"example.com/"_q)
			== u"example.com"_q,
		u"root path was not normalized"_q);
	for (const auto &invalid : {
		u"https://example.com/docs"_q,
		u"example.com/docs?q=1"_q,
		u"example.com/docs#part"_q,
		u"user@example.com/docs"_q,
		u"example.com/a b"_q,
	}) {
		Require(
			Core::NormalizeLinkRewritePrefix(invalid).isEmpty(),
			u"invalid link rewrite prefix was accepted: "_q + invalid);
	}
}

void TestLinkRewritePreservesSuffix() {
	const auto rules = std::vector<Core::LinkRewriteRule>{
		{ u"example.com/docs"_q, u"mirror.example/archive"_q },
	};
	Require(
		Core::RewriteLink(
			u"https://example.com/docs/api?q=1#part"_q,
			rules)
			== u"https://mirror.example/archive/api?q=1#part"_q,
		u"path rewrite did not preserve the URL suffix"_q);
	Require(
		Core::RewriteLink(
			u"https://example.com/docs-old/api"_q,
			rules)
			== u"https://example.com/docs-old/api"_q,
		u"path rewrite ignored the prefix boundary"_q);
}

void TestMostSpecificRewriteWins() {
	const auto rules = std::vector<Core::LinkRewriteRule>{
		{ u"example.com"_q, u"mirror.example"_q },
		{ u"example.com/docs"_q, u"docs.example/archive"_q },
	};
	Require(
		Core::RewriteLink(
			u"http://example.com/docs/api"_q,
			rules) == u"http://docs.example/archive/api"_q,
		u"less specific rewrite shadowed a path rewrite"_q);
}

void TestSeveralSourcesCanShareTarget() {
	const auto rules = std::vector<Core::LinkRewriteRule>{
		{ u"example.com"_q, u"mirror.example/view"_q },
		{ u"www.example.com"_q, u"mirror.example/view"_q },
	};
	for (const auto &source : {
		u"https://example.com/post/1"_q,
		u"https://www.example.com/post/1"_q,
	}) {
		Require(
			Core::RewriteLink(source, rules)
				== u"https://mirror.example/view/post/1"_q,
			u"sources sharing one target rewrote differently"_q);
	}
}

void TestDefaultLinkRewritesMatchCurrentProfile() {
	const auto expected = std::vector<Core::LinkRewriteRule>{
		{ u"x.com"_q, u"fixupx.com"_q },
		{ u"www.instagram.com"_q, u"eeinstagram.com"_q },
		{ u"instagram.com"_q, u"eeinstagram.com"_q },
		{ u"youtu.be"_q, u"koutu.be"_q },
		{ u"www.youtube.com"_q, u"koutu.be"_q },
		{ u"youtube.com"_q, u"koutu.be"_q },
	};
	Require(
		Core::DefaultLinkRewriteRules() == expected,
		u"default link rewrites differ from the current profile"_q);
}

} // namespace

int main(int argc, char *argv[]) {
	const auto app = QCoreApplication(argc, argv);
	TestLinkRewritePrefixNormalization();
	TestLinkRewritePreservesSuffix();
	TestMostSpecificRewriteWins();
	TestSeveralSourcesCanShareTarget();
	TestDefaultLinkRewritesMatchCurrentProfile();
	return 0;
}
