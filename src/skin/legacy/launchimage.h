#pragma once

#include <QElapsedTimer>
#include <QPixmap>
#include <QString>
#include <QTimer>
#include <QWidget>

QT_FORWARD_DECLARE_CLASS(QQmlEngine);
QT_FORWARD_DECLARE_CLASS(QQuickWindow);

// Shon loading screen, shown until the skin is ready.
// With Qt Quick available it opens a small splash window whose spinner is
// animated on Qt Quick's render thread, so it keeps moving while startup
// work blocks the main thread. Without Qt Quick it falls back to painting
// the same layout inside the main window.
// The skin's <LaunchImageStyle> is ignored so the branding never changes.
class LaunchImage: public QWidget {
    Q_OBJECT
  public:
    LaunchImage(QWidget* pParent, const QString& styleSheet);
    ~LaunchImage() override;
    void progress(int value, const QString& serviceName);

  protected:
    void paintEvent(QPaintEvent*) override;

  private:
    void closeSplashWindow();

    QQmlEngine* m_pQmlEngine;
    QQuickWindow* m_pSplashWindow;
    QPixmap m_logo;
    QTimer m_animationTimer;
    QElapsedTimer m_clock;
    QString m_status;
    int m_progress;
};
