#pragma once

#include "NativeVideoFrame.h"
#include <QOpenGLWidget>
#include <QJsonObject>
#include <atomic>
#include <functional>
#include <memory>


namespace dockpipe::desktop {


// The UI thread owns OpenGL and CUDA/GL registration. The retained AVFrame
// keeps its decoder surface and CUDA device alive until the copy finishes.
class GpuVideoWidget final : public QOpenGLWidget {
public:
    explicit GpuVideoWidget(QWidget *parent);
    ~GpuVideoWidget() override;
    bool available() const;
    void present(NativeVideoFrame frame, const QRect &destination);
    void setDestination(const QRect &destination);
    void reset();
    QJsonObject observation() const;
    std::function<void()> presentationFailed;

protected:
    void initializeGL() override;
    void paintGL() override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::atomic<bool> m_available{false};
    void cleanup();
};

} // namespace dockpipe::desktop
