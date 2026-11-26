#pragma once

#include <LauncherDisplayPackage.h>
#include <memory>

namespace dockpipe::desktop {

class DesktopViewerSurface final : public LauncherDisplaySurface {
    Q_OBJECT
    Q_PROPERTY(int closeCompletionBudgetMs READ closeCompletionBudgetMs CONSTANT)
    Q_PROPERTY(QJsonObject closeReference READ closeReference)
    Q_PROPERTY(QJsonObject presentationOptions READ presentationOptions)
    Q_PROPERTY(QJsonObject captureObservation READ captureObservation)
    Q_PROPERTY(QJsonObject decoderStatus READ decoderStatus)
    Q_PROPERTY(QJsonObject decoderFailure READ decoderFailure)
public:
    DesktopViewerSurface(const QJsonObject &options, const QString &channelHelper, QWidget *parent);
    ~DesktopViewerSurface() override;
    void setActive(bool active) override;
    void closeDisplay() override;
    int closeCompletionBudgetMs() const;
    QJsonObject closeReference() const;
    Q_INVOKABLE void requestClose();
    Q_INVOKABLE void setPresentationOptions(const QJsonObject &options);
    QJsonObject presentationOptions() const;
    QJsonObject captureObservation() const;
    QJsonObject decoderStatus() const;
    QJsonObject decoderFailure() const;

signals:
    void closeCompleted(const QJsonObject &result);

protected:
    void paintEvent(QPaintEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    bool event(QEvent *event) override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace dockpipe::desktop
