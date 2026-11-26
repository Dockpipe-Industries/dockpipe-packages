#include "GpuVideoWidget.h"

#include <QElapsedTimer>
#include <QLibrary>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLTimerQuery>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLShaderProgram>
#include <QSurfaceFormat>
#include <QVector3D>
#include <QMatrix3x3>
#include <array>
#include <stdexcept>

#include "cuda/dynlink_cuda.h"
extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
}

namespace dockpipe::desktop {
namespace {

template<class Function>
Function *resolve(QLibrary &library, const char *name)
{
    const auto address = library.resolve(name);
    if (!address)
        throw std::runtime_error("CUDA graphics interoperability is unavailable.");
    return reinterpret_cast<Function *>(address);
}

void checked(CUresult result)
{
    if (result != CUDA_SUCCESS)
        throw std::runtime_error("CUDA graphics interoperability failed: " + std::to_string(static_cast<int>(result)));
}

class CurrentCudaContext final {
public:
    CurrentCudaContext(tcuCtxPushCurrent_v2 *push, tcuCtxPopCurrent_v2 *pop, CUcontext context)
        : m_pop(pop)
    {
        checked(push(context));
    }
    ~CurrentCudaContext()
    {
        CUcontext previous = nullptr;
        m_pop(&previous);
    }
private:
    tcuCtxPopCurrent_v2 *m_pop;
};

constexpr auto VertexSource = R"(
#version 130
in vec2 position;
in vec2 coordinate;
out vec2 uv;
void main() {
    gl_Position = vec4(position, 0.0, 1.0);
    uv = coordinate;
}
)";

constexpr auto FragmentSource = R"(
#version 130
uniform sampler2D luma;
uniform sampler2D chroma;
uniform vec3 offset;
uniform mat3 conversion;
in vec2 uv;
out vec4 color;
void main() {
    vec3 yuv = vec3(texture(luma, uv).r, texture(chroma, uv).rg);
    color = vec4(conversion * (yuv - offset), 1.0);
}
)";

} // namespace

struct GpuVideoWidget::Impl {
    QLibrary driver{"libcuda.so.1"};
    tcuCtxPushCurrent_v2 *push = nullptr;
    tcuCtxPopCurrent_v2 *pop = nullptr;
    tcuGraphicsGLRegisterImage *registerImage = nullptr;
    tcuGraphicsUnregisterResource *unregisterResource = nullptr;
    tcuGraphicsMapResources *map = nullptr;
    tcuGraphicsUnmapResources *unmap = nullptr;
    tcuGraphicsSubResourceGetMappedArray *mappedArray = nullptr;
    tcuMemcpy2D_v2 *copy = nullptr;
    tcuStreamSynchronize *synchronize = nullptr;
    AVBufferRef *device = nullptr;
    CUcontext cudaContext = nullptr;
    std::array<CUgraphicsResource, 2> resources{};
    std::array<GLuint, 2> textures{};
    GLuint vertices = 0;
    std::unique_ptr<QOpenGLVertexArrayObject> vertexArray;
    std::unique_ptr<QOpenGLShaderProgram> program;
    NativeVideoFrame pending;
    QRect destination;
    QSize textureSize;
    qint64 frames = 0;
    std::array<std::unique_ptr<QOpenGLTimerQuery>, 3> queries;
    std::array<bool, 3> queryPending{};
    qint64 shaderNanoseconds = 0;
    qint64 shaderSamples = 0;
    qint64 copyNanoseconds = 0;
    qint64 drawSubmitNanoseconds = 0;
    bool failed = false;
    QString failureDetail;
    bool uploaded = false;

    void loadDriver()
    {
        if (!driver.load())
            throw std::runtime_error("The installed CUDA driver is unavailable.");
        push = resolve<tcuCtxPushCurrent_v2>(driver, "cuCtxPushCurrent_v2");
        pop = resolve<tcuCtxPopCurrent_v2>(driver, "cuCtxPopCurrent_v2");
        registerImage = resolve<tcuGraphicsGLRegisterImage>(driver, "cuGraphicsGLRegisterImage");
        unregisterResource = resolve<tcuGraphicsUnregisterResource>(driver, "cuGraphicsUnregisterResource");
        map = resolve<tcuGraphicsMapResources>(driver, "cuGraphicsMapResources");
        unmap = resolve<tcuGraphicsUnmapResources>(driver, "cuGraphicsUnmapResources");
        mappedArray = resolve<tcuGraphicsSubResourceGetMappedArray>(driver, "cuGraphicsSubResourceGetMappedArray");
        copy = resolve<tcuMemcpy2D_v2>(driver, "cuMemcpy2D_v2");
        synchronize = resolve<tcuStreamSynchronize>(driver, "cuStreamSynchronize");
    }

    void releaseInterop()
    {
        if (cudaContext) {
            CurrentCudaContext current(push, pop, cudaContext);
            for (auto &resource : resources) {
                if (resource)
                    checked(unregisterResource(resource));
                resource = nullptr;
            }
        }
        cudaContext = nullptr;
        av_buffer_unref(&device);
        textureSize = {};
        uploaded = false;
    }

    void prepare(QOpenGLFunctions *gl, const AVFrame *frame)
    {
        const auto *pool = reinterpret_cast<const AVHWFramesContext *>(frame->hw_frames_ctx->data);
        const auto *cuda = static_cast<const AVCUDADeviceContext *>(pool->device_ctx->hwctx);
        if (!cuda || !cuda->cuda_ctx)
            throw std::runtime_error("CUDA frame has no live device context.");
        if (textureSize == QSize(frame->width, frame->height) && cudaContext == cuda->cuda_ctx)
            return;
        releaseInterop();
        device = av_buffer_ref(pool->device_ref);
        if (!device)
            throw std::runtime_error("Could not retain the CUDA presentation device.");
        cudaContext = cuda->cuda_ctx;
        CurrentCudaContext current(push, pop, cudaContext);
        for (unsigned plane = 0; plane < textures.size(); ++plane) {
            gl->glBindTexture(GL_TEXTURE_2D, textures[plane]);
            const int divisor = plane == 0 ? 1 : 2;
            const auto format = plane == 0 ? GL_RED : GL_RG;
            const auto storage = plane == 0 ? GL_R8 : GL_RG8;
            gl->glTexImage2D(GL_TEXTURE_2D, 0, storage, frame->width / divisor,
                frame->height / divisor, 0, format, GL_UNSIGNED_BYTE, nullptr);
            // WRITE_DISCARD. CUDA owns the texture only between map/unmap.
            checked(registerImage(&resources[plane], textures[plane], GL_TEXTURE_2D, CU_GRAPHICS_REGISTER_FLAGS_WRITE_DISCARD));
        }
        textureSize = QSize(frame->width, frame->height);
    }

    void upload(QOpenGLFunctions *gl, const AVFrame *frame)
    {
        prepare(gl, frame);
        const auto *pool = reinterpret_cast<const AVHWFramesContext *>(frame->hw_frames_ctx->data);
        const auto *cuda = static_cast<const AVCUDADeviceContext *>(pool->device_ctx->hwctx);
        CurrentCudaContext current(push, pop, cudaContext);
        checked(map(static_cast<unsigned>(resources.size()), resources.data(), cuda->stream));
        try {
            for (unsigned plane = 0; plane < resources.size(); ++plane) {
                CUarray destinationArray = nullptr;
                checked(mappedArray(&destinationArray, resources[plane], 0, 0));
                CUDA_MEMCPY2D transfer{};
                transfer.srcMemoryType = CU_MEMORYTYPE_DEVICE;
                transfer.srcDevice = reinterpret_cast<CUdeviceptr>(frame->data[plane]);
                transfer.srcPitch = static_cast<size_t>(frame->linesize[plane]);
                transfer.dstMemoryType = CU_MEMORYTYPE_ARRAY;
                transfer.dstArray = destinationArray;
                transfer.WidthInBytes = static_cast<size_t>(frame->width);
                transfer.Height = static_cast<size_t>(plane == 0 ? frame->height : frame->height / 2);
                checked(copy(&transfer));
            }
        } catch (...) {
            unmap(static_cast<unsigned>(resources.size()), resources.data(), cuda->stream);
            synchronize(cuda->stream);
            throw;
        }
        checked(unmap(static_cast<unsigned>(resources.size()), resources.data(), cuda->stream));
        checked(synchronize(cuda->stream));
        uploaded = true;
    }

    void setColor(const AVFrame *frame)
    {
        const bool full = frame->color_range == AVCOL_RANGE_JPEG;
        const bool bt709 = frame->colorspace == AVCOL_SPC_BT709;
        const float yScale = full ? 1.0f : 255.0f / 219.0f;
        const float uvScale = full ? 1.0f : 255.0f / 224.0f;
        const float redV = bt709 ? 1.5748f : 1.402f;
        const float greenU = bt709 ? -0.187324f : -0.344136f;
        const float greenV = bt709 ? -0.468124f : -0.714136f;
        const float blueU = bt709 ? 1.8556f : 1.772f;
        QMatrix3x3 matrix;
        matrix(0, 0) = yScale;
        matrix(0, 1) = 0;
        matrix(0, 2) = redV * uvScale;
        matrix(1, 0) = yScale;
        matrix(1, 1) = greenU * uvScale;
        matrix(1, 2) = greenV * uvScale;
        matrix(2, 0) = yScale;
        matrix(2, 1) = blueU * uvScale;
        matrix(2, 2) = 0;
        program->setUniformValue("offset", QVector3D(full ? 0.0f : 16.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f));
        program->setUniformValue("conversion", matrix);
    }
};

GpuVideoWidget::GpuVideoWidget(QWidget *parent)
    : QOpenGLWidget(parent), m_impl(std::make_unique<Impl>())
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    QSurfaceFormat format;
    format.setVersion(3, 0);
    format.setProfile(QSurfaceFormat::NoProfile);
    format.setSwapInterval(0);
    setFormat(format);
}

GpuVideoWidget::~GpuVideoWidget()
{
    if (context())
        disconnect(context(), nullptr, this, nullptr);
    cleanup();
}

bool GpuVideoWidget::available() const
{
    return m_available.load();
}

void GpuVideoWidget::initializeGL()
{
    try {
        auto *gl = context()->functions();
        m_impl->loadDriver();
        m_impl->program = std::make_unique<QOpenGLShaderProgram>();
        if (!m_impl->program->addShaderFromSourceCode(QOpenGLShader::Vertex, VertexSource)
            || !m_impl->program->addShaderFromSourceCode(QOpenGLShader::Fragment, FragmentSource)
            || !m_impl->program->link())
            throw std::runtime_error("GPU desktop color conversion is unavailable.");
        gl->glGenTextures(2, m_impl->textures.data());
        for (const auto texture : m_impl->textures) {
            gl->glBindTexture(GL_TEXTURE_2D, texture);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        m_impl->vertexArray = std::make_unique<QOpenGLVertexArrayObject>();
        if (!m_impl->vertexArray->create())
            throw std::runtime_error("OpenGL desktop vertex array is unavailable.");
        m_impl->vertexArray->bind();
        constexpr GLfloat vertices[]{-1, -1, 0, 1, 1, -1, 1, 1, -1, 1, 0, 0, 1, 1, 1, 0};
        gl->glGenBuffers(1, &m_impl->vertices);
        gl->glBindBuffer(GL_ARRAY_BUFFER, m_impl->vertices);
        gl->glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
        connect(context(), &QOpenGLContext::aboutToBeDestroyed, this, [this] { cleanup(); }, Qt::DirectConnection);
        for (auto &query : m_impl->queries) {
            query = std::make_unique<QOpenGLTimerQuery>();
            if (!query->create())
                query.reset();
        }
        m_available.store(true);
    } catch (...) {
        m_impl->failed = true;
        m_available.store(false);
    }
}

void GpuVideoWidget::present(NativeVideoFrame frame, const QRect &destination)
{
    m_impl->pending = std::move(frame);
    m_impl->destination = destination;
    update();
}

void GpuVideoWidget::setDestination(const QRect &destination)
{
    if (m_impl->destination == destination)
        return;
    m_impl->destination = destination;
    update();
}

void GpuVideoWidget::paintGL()
{
    auto *gl = context()->functions();
    gl->glClearColor(12.0f / 255, 16.0f / 255, 22.0f / 255, 1);
    gl->glClear(GL_COLOR_BUFFER_BIT);
    if (!m_available.load())
        return;
    try {
        auto frame = std::move(m_impl->pending);
        auto &program = *m_impl->program;
        if (!program.bind())
            throw std::runtime_error("GPU desktop shader activation failed.");
        if (frame) {
            QElapsedTimer transfer;
            transfer.start();
            m_impl->upload(gl, frame.get());
            m_impl->copyNanoseconds += transfer.nsecsElapsed();
            m_impl->setColor(frame.get());
        }
        if (!m_impl->uploaded)
            return;
        const auto rect = m_impl->destination;
        const auto ratio = devicePixelRatioF();
        gl->glViewport(qRound(rect.x() * ratio), qRound((height() - rect.y() - rect.height()) * ratio),
            qRound(rect.width() * ratio), qRound(rect.height() * ratio));
        QOpenGLTimerQuery *timer = nullptr;
        int timerIndex = -1;
        for (unsigned index = 0; index < m_impl->queries.size(); ++index) {
            auto &query = m_impl->queries[index];
            if (!query)
                continue;
            if (m_impl->queryPending[index] && query->isResultAvailable()) {
                m_impl->shaderNanoseconds += static_cast<qint64>(query->waitForResult());
                ++m_impl->shaderSamples;
                m_impl->queryPending[index] = false;
            }
            if (frame && !m_impl->queryPending[index] && !timer) {
                timer = query.get();
                timerIndex = static_cast<int>(index);
            }
        }
        QElapsedTimer draw;
        draw.start();
        if (timer)
            timer->begin();
        for (unsigned plane = 0; plane < m_impl->textures.size(); ++plane) {
            gl->glActiveTexture(GL_TEXTURE0 + plane);
            gl->glBindTexture(GL_TEXTURE_2D, m_impl->textures[plane]);
        }
        program.setUniformValue("luma", 0);
        program.setUniformValue("chroma", 1);
        m_impl->vertexArray->bind();
        gl->glBindBuffer(GL_ARRAY_BUFFER, m_impl->vertices);
        program.enableAttributeArray("position");
        program.enableAttributeArray("coordinate");
        program.setAttributeBuffer("position", GL_FLOAT, 0, 2, 4 * sizeof(GLfloat));
        program.setAttributeBuffer("coordinate", GL_FLOAT, 2 * sizeof(GLfloat), 2, 4 * sizeof(GLfloat));
        gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        if (timer) {
            timer->end();
            m_impl->queryPending[static_cast<unsigned>(timerIndex)] = true;
        }
        const auto glError = gl->glGetError();
        if (glError != GL_NO_ERROR)
            throw std::runtime_error("OpenGL desktop presentation failed: " + std::to_string(glError));
        program.disableAttributeArray("position");
        program.disableAttributeArray("coordinate");
        program.release();
        if (frame) {
            ++m_impl->frames;
            m_impl->drawSubmitNanoseconds += draw.nsecsElapsed();
        }
    } catch (const std::exception &error) {
        m_available.store(false);
        m_impl->failed = true;
        m_impl->failureDetail = QString::fromUtf8(error.what());
        qWarning() << m_impl->failureDetail;
        m_impl->pending.reset();
        if (presentationFailed)
            presentationFailed();
    }
}

void GpuVideoWidget::reset()
{
    m_impl->pending.reset();
    if (context() && context()->isValid()) {
        makeCurrent();
        try {
            m_impl->releaseInterop();
        } catch (...) {
            m_available.store(false);
        }
        doneCurrent();
    }
    hide();
}

void GpuVideoWidget::cleanup()
{
    m_available.store(false);
    m_impl->pending.reset();
    if (!context() || !context()->isValid())
        return;
    makeCurrent();
    try {
        m_impl->releaseInterop();
    } catch (...) {
        // Keep the device reference alive until this widget's destruction.
        // A failed unregister must never be followed by deleting its texture.
        doneCurrent();
        return;
    }
    auto *gl = context()->functions();
    gl->glDeleteTextures(2, m_impl->textures.data());
    m_impl->textures = {};
    if (m_impl->vertices)
        gl->glDeleteBuffers(1, &m_impl->vertices);
    m_impl->vertices = 0;
    for (auto &query : m_impl->queries)
        query.reset();
    m_impl->queryPending = {};
    m_impl->vertexArray.reset();
    m_impl->program.reset();
    doneCurrent();
}

QJsonObject GpuVideoWidget::observation() const
{
    return {{"failure_detail", m_impl->failureDetail}, {"backend", m_impl->failed ? "raster-fallback" : "cuda-opengl"},
        {"painted_frames", m_impl->frames}, {"device_copy_total_ns", m_impl->copyNanoseconds},
        {"draw_submit_total_ns", m_impl->drawSubmitNanoseconds},
        {"shader_total_ns", m_impl->shaderNanoseconds}, {"shader_samples", m_impl->shaderSamples},
        {"registered_resources", static_cast<int>((m_impl->resources[0] != nullptr) + (m_impl->resources[1] != nullptr))},
        {"device_retained", m_impl->device != nullptr}};
}

} // namespace dockpipe::desktop
