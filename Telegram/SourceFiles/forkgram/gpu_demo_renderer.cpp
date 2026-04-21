/*
This file is part of Forkgram Desktop,
the unofficial Telegram desktop client.

For license and copyright information please follow this link:
https://github.com/nicegram/nicegram-desktop/blob/master/LEGAL
*/
#include "forkgram/gpu_demo_renderer.h"

#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)

#include "ui/rhi/rhi_shader.h"
#include "ui/rp_widget.h"
#include "ui/painter.h"
#include "styles/style_basic.h"

#include <rhi/qrhi.h>
#include <QTimer>

namespace Forkgram {
namespace {

struct DemoUniforms {
	float viewport[2];
	float time;
	float _pad;
};
static_assert(sizeof(DemoUniforms) % 16 == 0);

[[nodiscard]] QShader LoadShader(const QString &name) {
	return Ui::Rhi::ShaderFromFile(
		u":/shaders/"_q + name + u".qsb"_q);
}

} // namespace

bool GpuDemoRenderer::Available() {
	static const auto result = [] {
		return LoadShader(u"demo.vert"_q).isValid()
			&& LoadShader(u"demo.frag"_q).isValid();
	}();
	return result;
}

GpuDemoRenderer::GpuDemoRenderer() {
	_elapsed.start();
}

GpuDemoRenderer::~GpuDemoRenderer() {
	releaseResources();
}

void GpuDemoRenderer::initialize(
		QRhi *rhi,
		QRhiRenderTarget *rt,
		QRhiCommandBuffer *cb) {
	if (_initialized && _rhi == rhi) {
		return;
	}
	releaseResources();

	_rhi = rhi;
	if (!Available()) {
		return;
	}

	constexpr auto kVertexSize = 4 * sizeof(float);
	constexpr auto kQuadVertices = 4;

	_vertexBuffer = rhi->newBuffer(
		QRhiBuffer::Dynamic,
		QRhiBuffer::VertexBuffer,
		kQuadVertices * kVertexSize);
	_vertexBuffer->create();

	_uniformBuffer = rhi->newBuffer(
		QRhiBuffer::Dynamic,
		QRhiBuffer::UniformBuffer,
		sizeof(DemoUniforms));
	_uniformBuffer->create();

	_srb = rhi->newShaderResourceBindings();
	_srb->setBindings({
		QRhiShaderResourceBinding::uniformBuffer(
			0,
			QRhiShaderResourceBinding::VertexStage
				| QRhiShaderResourceBinding::FragmentStage,
			_uniformBuffer),
	});
	if (!_srb->create()) {
		releaseResources();
		return;
	}

	const auto rpDesc = rt->renderPassDescriptor();
	const auto vertShader = LoadShader(u"demo.vert"_q);
	const auto fragShader = LoadShader(u"demo.frag"_q);

	_pipeline = rhi->newGraphicsPipeline();
	_pipeline->setShaderStages({
		{ QRhiShaderStage::Vertex, vertShader },
		{ QRhiShaderStage::Fragment, fragShader },
	});

	QRhiVertexInputLayout inputLayout;
	inputLayout.setBindings({
		{ quint32(kVertexSize) },
	});
	inputLayout.setAttributes({
		{ 0, 0, QRhiVertexInputAttribute::Float2, 0 },
		{ 0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float) },
	});
	_pipeline->setVertexInputLayout(inputLayout);
	_pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
	_pipeline->setShaderResourceBindings(_srb);
	_pipeline->setRenderPassDescriptor(rpDesc);
	if (!_pipeline->create()) {
		releaseResources();
		return;
	}

	_initialized = true;

	LOG(("[GPU_DEMO] backend=%1 device=%2")
		.arg(rhi->backendName())
		.arg(rhi->driverInfo().deviceName));
}

void GpuDemoRenderer::render(
		QRhi *rhi,
		QRhiRenderTarget *rt,
		QRhiCommandBuffer *cb) {
	if (!_initialized) {
		auto *rub = rhi->nextResourceUpdateBatch();
		cb->beginPass(rt, rhiClearColor(), { 1.0f, 0 }, rub);
		cb->endPass();
		return;
	}
	_rhi = rhi;

	const auto size = rt->pixelSize();
	const auto factor = style::DevicePixelRatio();
	const auto w = float(size.width() / factor);
	const auto h = float(size.height() / factor);
	const auto t = float(_elapsed.elapsed()) / 1000.0f;

	const float vertices[] = {
		0.f, 0.f,   0.f, 0.f,
		w,   0.f,   1.f, 0.f,
		0.f, h,     0.f, 1.f,
		w,   h,     1.f, 1.f,
	};

	DemoUniforms uniforms;
	uniforms.viewport[0] = w;
	uniforms.viewport[1] = h;
	uniforms.time = t;
	uniforms._pad = 0.f;

	auto *rub = rhi->nextResourceUpdateBatch();
	rub->updateDynamicBuffer(_vertexBuffer, 0, sizeof(vertices), vertices);
	rub->updateDynamicBuffer(
		_uniformBuffer, 0, sizeof(uniforms), &uniforms);

	const auto bg = QColor(0, 0, 0, 255);
	cb->beginPass(rt, bg, { 1.0f, 0 }, rub);

	cb->setGraphicsPipeline(_pipeline);
	cb->setShaderResources(_srb);
	cb->setViewport({
		0, 0,
		float(size.width()),
		float(size.height()) });

	const QRhiCommandBuffer::VertexInput vbufBinding(_vertexBuffer, 0);
	cb->setVertexInput(0, 1, &vbufBinding);
	cb->draw(4);

	cb->endPass();
}

void GpuDemoRenderer::releaseResources() {
	delete _pipeline;
	_pipeline = nullptr;
	delete _srb;
	_srb = nullptr;
	delete _uniformBuffer;
	_uniformBuffer = nullptr;
	delete _vertexBuffer;
	_vertexBuffer = nullptr;
	_initialized = false;
}

void GpuDemoRenderer::paintFallback(
		Painter &p,
		const QRegion &clip,
		Ui::GL::Backend backend) {
	p.fillRect(clip.boundingRect(), QColor(40, 40, 60));
	p.setPen(Qt::white);
	p.drawText(
		clip.boundingRect(),
		Qt::AlignCenter,
		u"GPU Demo (QRhi not available)"_q);
}

Ui::GL::ChosenRenderer ChooseDemoRenderer() {
	const auto backend = GpuDemoRenderer::Available()
		? Ui::GL::Backend::QRhi
		: Ui::GL::Backend::Raster;
	return {
		.renderer = std::make_unique<GpuDemoRenderer>(),
		.backend = backend,
	};
}

std::unique_ptr<Ui::RpWidgetWrap> CreateGpuDemoWidget(
		QWidget *parent) {
	auto surface = Ui::GL::CreateSurface(parent, ChooseDemoRenderer());
	if (const auto widget = surface->rpWidget()) {
		const auto raw = widget;
		auto *timer = new QTimer(raw);
		timer->setInterval(16);
		QObject::connect(timer, &QTimer::timeout, raw, [raw] {
			raw->update();
		});
		timer->start();
	}
	return surface;
}

} // namespace Forkgram

#endif // Qt >= 6.7
