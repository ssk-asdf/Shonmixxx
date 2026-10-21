#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <memory>

#include "preferences/usersettings.h"

class ControlPushButton;
class QTimer;

/// ShonDJ practice mode. Watches your transitions and scores each one after
/// it finishes: how tight the beats were, whether it started on a phrase,
/// and how long the blend ran. It only reports; it never touches the mix.
class ShonPractice : public QObject {
    Q_OBJECT
  public:
    explicit ShonPractice(UserSettingsPointer pConfig, QObject* pParent = nullptr);
    ~ShonPractice() override;

  signals:
    void notice(const QString& text);

  private:
    void tick();
    double audibleGain(int deck) const;
    int beatIndex(int deck) const;
    bool onPhrase(int deck) const;
    void finishBlend(int winner);

    UserSettingsPointer m_pConfig;
    std::unique_ptr<ControlPushButton> m_pEnabled;
    QTimer* m_pTimer = nullptr;

    int m_mainDeck = 0;      // deck the crowd is hearing, 0 = none
    int m_incomingDeck = 0;  // deck being blended in, 0 = no blend
    bool m_startOnPhrase = false;
    QElapsedTimer m_blendClock;
    QElapsedTimer m_settleClock;
    double m_alignSumMs = 0.0;
    int m_alignCount = 0;
    int m_transitionCount = 0;
    int m_scoreSum = 0;
};
