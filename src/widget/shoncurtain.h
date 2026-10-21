#pragma once

#include <QElapsedTimer>
#include <QPixmap>
#include <QPoint>
#include <QPointer>
#include <QWidget>
#include <memory>

#include "preferences/usersettings.h"

class ControlPushButton;
class QTimer;

/// ShonDJ logo screen. A full window cover with just the logo, like the
/// loading screen without the spinner. Ctrl+L or [Shon],curtain shows it, any
/// mouse move, click or key brings ShonDJ back. It also comes up on its own
/// after a few idle minutes, but only when nothing is playing or Away mode is
/// on, so it never covers the decks while you mix. The music never stops.
class ShonCurtain : public QWidget {
    Q_OBJECT
  public:
    ShonCurtain(UserSettingsPointer pConfig, QWidget* pMainWindow);
    ~ShonCurtain() override;

  protected:
    void paintEvent(QPaintEvent* pEvent) override;
    bool eventFilter(QObject* pObject, QEvent* pEvent) override;

  private:
    void showCurtain();
    void hideCurtain();
    void fitToScreen();
    void checkIdle();
    bool mixing() const;

    UserSettingsPointer m_pConfig;
    QPointer<QWidget> m_pMainWindow;
    std::unique_ptr<ControlPushButton> m_pShow;
    std::unique_ptr<ControlPushButton> m_pAuto;
    QTimer* m_pIdleTimer = nullptr;
    QElapsedTimer m_idle;
    QElapsedTimer m_shownFor;
    QPoint m_lastCursor;
    QPixmap m_logo;
};
