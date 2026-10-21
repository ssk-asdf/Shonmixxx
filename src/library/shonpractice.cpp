#include "library/shonpractice.h"

#include <QTimer>
#include <algorithm>
#include <cmath>

#include "control/controlobject.h"
#include "control/controlpushbutton.h"
#include "mixer/playerinfo.h"
#include "moc_shonpractice.cpp"
#include "track/beats.h"
#include "track/track.h"

namespace {

const QString kGroup = QStringLiteral("[Shon]");
constexpr double kAudible = 0.15;      // deck gain that counts as "heard"
constexpr int kSettleMs = 1500;        // one deck alone this long ends a blend
constexpr int kBeatsPerPhrase = 64;    // 16 bars
constexpr int kPhraseSlackBeats = 2;   // a beat or two early or late is fine

QString deckGroup(int deck) {
    return QStringLiteral("[Channel%1]").arg(deck);
}

double get(const QString& group, const char* item) {
    return ControlObject::get(ConfigKey(group, QLatin1String(item)));
}

} // namespace

ShonPractice::ShonPractice(UserSettingsPointer pConfig, QObject* pParent)
        : QObject(pParent),
          m_pConfig(pConfig) {
    m_pEnabled = std::make_unique<ControlPushButton>(
            ConfigKey(kGroup, QStringLiteral("practice")), true, 1.0);
    m_pEnabled->setButtonMode(mixxx::control::ButtonMode::Toggle);
    connect(m_pEnabled.get(), &ControlObject::valueChanged, this, [this](double value) {
        m_incomingDeck = 0;
        if (value > 0.0) {
            m_transitionCount = 0;
            m_scoreSum = 0;
            emit notice(QStringLiteral("Practice: on. Each transition gets a score."));
        } else if (m_transitionCount > 0) {
            emit notice(QStringLiteral("Practice: off. %1 transitions, average %2/10")
                                .arg(m_transitionCount)
                                .arg(QString::number(
                                        static_cast<double>(m_scoreSum) / m_transitionCount,
                                        'f',
                                        1)));
        }
    });
    m_pTimer = new QTimer(this);
    m_pTimer->setInterval(100);
    connect(m_pTimer, &QTimer::timeout, this, &ShonPractice::tick);
    m_pTimer->start();
}

ShonPractice::~ShonPractice() = default;

double ShonPractice::audibleGain(int deck) const {
    const QString group = deckGroup(deck);
    if (get(group, "play") <= 0.0 || get(group, "track_loaded") <= 0.0) {
        return 0.0;
    }
    // Same idea as Mixxx's "which deck is the crowd hearing": channel fader
    // times the crossfader side.
    double gain = get(group, "volume");
    const double xfader = get(QStringLiteral("[Master]"), "crossfader");
    const int orientation = static_cast<int>(get(group, "orientation"));
    if (orientation == 0) { // left
        gain *= std::clamp(1.0 - xfader, 0.0, 1.0);
    } else if (orientation == 2) { // right
        gain *= std::clamp(1.0 + xfader, 0.0, 1.0);
    }
    return gain;
}

int ShonPractice::beatIndex(int deck) const {
    TrackPointer pTrack = PlayerInfo::instance().getTrackInfo(deckGroup(deck));
    if (!pTrack) {
        return -1;
    }
    const mixxx::BeatsPointer pBeats = pTrack->getBeats();
    if (!pBeats) {
        return -1;
    }
    const double samples = get(deckGroup(deck), "track_samples");
    const double position = get(deckGroup(deck), "playposition");
    if (samples <= 0.0) {
        return -1;
    }
    const auto frame = mixxx::audio::FramePos(position * samples / 2.0);
    const auto first = pBeats->firstBeat();
    if (!first.isValid() || frame < first) {
        return 0;
    }
    return pBeats->numBeatsInRange(first, frame);
}

bool ShonPractice::onPhrase(int deck) const {
    const int index = beatIndex(deck);
    if (index < 0) {
        return false;
    }
    const int offset = index % kBeatsPerPhrase;
    return offset <= kPhraseSlackBeats || offset >= kBeatsPerPhrase - kPhraseSlackBeats;
}

void ShonPractice::tick() {
    if (m_pEnabled->get() <= 0.0) {
        return;
    }
    const int numDecks = std::clamp(
            static_cast<int>(get(QStringLiteral("[App]"), "num_decks")), 2, 4);

    if (m_incomingDeck == 0) {
        // No blend going: keep track of the main deck, and start a blend
        // as soon as a second deck becomes audible.
        if (m_mainDeck == 0 || audibleGain(m_mainDeck) < kAudible) {
            m_mainDeck = 0;
            double best = kAudible;
            for (int deck = 1; deck <= numDecks; ++deck) {
                const double gain = audibleGain(deck);
                if (gain >= best) {
                    best = gain;
                    m_mainDeck = deck;
                }
            }
            return;
        }
        for (int deck = 1; deck <= numDecks; ++deck) {
            if (deck != m_mainDeck && audibleGain(deck) >= kAudible) {
                m_incomingDeck = deck;
                m_startOnPhrase = onPhrase(deck);
                m_blendClock.start();
                m_settleClock.invalidate();
                m_alignSumMs = 0.0;
                m_alignCount = 0;
                return;
            }
        }
        return;
    }

    const bool mainHeard = audibleGain(m_mainDeck) >= kAudible;
    const bool incomingHeard = audibleGain(m_incomingDeck) >= kAudible;

    if (mainHeard && incomingHeard) {
        m_settleClock.invalidate();
        // How far apart the beats are, in milliseconds.
        const double bpm = get(deckGroup(m_incomingDeck), "bpm");
        if (bpm > 0.0) {
            double distance = std::abs(get(deckGroup(m_mainDeck), "beat_distance") -
                    get(deckGroup(m_incomingDeck), "beat_distance"));
            distance = std::min(distance, 1.0 - distance);
            m_alignSumMs += distance * 60000.0 / bpm;
            ++m_alignCount;
        }
        return;
    }

    if (!m_settleClock.isValid()) {
        m_settleClock.start();
        return;
    }
    if (m_settleClock.elapsed() < kSettleMs) {
        return;
    }
    if (incomingHeard && !mainHeard) {
        finishBlend(m_incomingDeck);
        m_mainDeck = m_incomingDeck;
    } else if (!incomingHeard && !mainHeard) {
        m_mainDeck = 0; // everything stopped, no score
    }
    // Main deck alone again: the blend was pulled back, no score.
    m_incomingDeck = 0;
}

void ShonPractice::finishBlend(int winner) {
    Q_UNUSED(winner);
    const double blendSeconds =
            std::max<qint64>(0, m_blendClock.elapsed() - m_settleClock.elapsed()) / 1000.0;
    const double alignMs = m_alignCount > 0 ? m_alignSumMs / m_alignCount : -1.0;

    int score = 0;
    QString beatText;
    if (alignMs < 0) {
        beatText = QStringLiteral("hard cut");
        score += 2;
    } else if (alignMs < 15) {
        beatText = QStringLiteral("beats tight (%1 ms)").arg(qRound(alignMs));
        score += 4;
    } else if (alignMs < 30) {
        beatText = QStringLiteral("beats close (%1 ms)").arg(qRound(alignMs));
        score += 3;
    } else if (alignMs < 50) {
        beatText = QStringLiteral("beats a bit off (%1 ms)").arg(qRound(alignMs));
        score += 2;
    } else if (alignMs < 80) {
        beatText = QStringLiteral("beats drifting (%1 ms)").arg(qRound(alignMs));
        score += 1;
    } else {
        beatText = QStringLiteral("beats clashing (%1 ms)").arg(qRound(alignMs));
    }

    const QString phraseText = m_startOnPhrase ? QStringLiteral("on phrase")
                                               : QStringLiteral("off phrase");
    score += m_startOnPhrase ? 3 : 0;

    if (blendSeconds >= 8.0 && blendSeconds <= 90.0) {
        score += 3;
    } else if (blendSeconds >= 4.0) {
        score += 2;
    } else {
        score += 1;
    }

    ++m_transitionCount;
    m_scoreSum += score;
    emit notice(QStringLiteral("Transition %1: %2, %3, %4 s blend. %5/10")
                        .arg(m_transitionCount)
                        .arg(beatText, phraseText)
                        .arg(qRound(blendSeconds))
                        .arg(score));
}
