/*
This file is part of Forkgram Desktop,
the unofficial Telegram desktop client.

For license and copyright information please follow this link:
https://github.com/nicegram/nicegram-desktop/blob/master/LEGAL
*/
#pragma once

#include "ui/rhi/rhi_renderer.h"
#include "ui/gl/gl_surface.h"

#include <QElapsedTimer>

#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)

class QRhi;
class QRhiBuffer;
class QRhiSampler;
class QRhiGraphicsPipeline;
class QRhiShaderResourceBindings;

namespace Ui {
class RpWidgetWrap;
class RpWidget;
} // namespace Ui

namespace Forkgram {

class GpuDemoRenderer final
	: public Ui::GL::Renderer
	, public Ui::Rhi::Renderer {
public:
	GpuDemoRenderer();
	~GpuDemoRenderer();

	[[nodiscard]] static bool Available();

	void initialize(
		QRhi *rhi,
		QRhiRenderTarget *rt,
		QRhiCommandBuffer *cb) override;
	void render(
		QRhi *rhi,
		QRhiRenderTarget *rt,
		QRhiCommandBuffer *cb) override;
	void releaseResources() override;

	QColor rhiClearColor() override {
		return QColor(0, 0, 0, 255);
	}

	std::optional<QColor> clearColor() override {
		return QColor(0, 0, 0, 255);
	}

	void paintFallback(
		Painter &p,
		const QRegion &clip,
		Ui::GL::Backend backend) override;

private:
	QRhi *_rhi = nullptr;
	QRhiBuffer *_vertexBuffer = nullptr;
	QRhiBuffer *_uniformBuffer = nullptr;
	QRhiGraphicsPipeline *_pipeline = nullptr;
	QRhiShaderResourceBindings *_srb = nullptr;

	QElapsedTimer _elapsed;
	bool _initialized = false;

};

[[nodiscard]] Ui::GL::ChosenRenderer ChooseDemoRenderer();

[[nodiscard]] std::unique_ptr<Ui::RpWidgetWrap> CreateGpuDemoWidget(
	QWidget *parent);

} // namespace Forkgram

#endif // Qt >= 6.7
