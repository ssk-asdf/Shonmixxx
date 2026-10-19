#include "analyzer/analyzersilence.h"

#include "analyzer/analyzertrack.h"
#include "analyzer/constants.h"
#include "track/beats.h"
#include "track/track.h"

#include <algorithm>
#include <cmath>

namespace {

// This threshold must not be changed, because this value is also used to
// verify that the track samples have not changed since the last analysis
constexpr CSAMPLE kSilenceThreshold = 0.001f; // -60 dB
// TODO: Change the above line to:
//constexpr CSAMPLE kSilenceThreshold = db2ratio(-60.0f);

bool shouldAnalyze(TrackPointer pTrack) {
    CuePointer pIntroCue = pTrack->findCueByType(mixxx::CueType::Intro);
    CuePointer pOutroCue = pTrack->findCueByType(mixxx::CueType::Outro);
    CuePointer pN60dBSound = pTrack->findCueByType(mixxx::CueType::N60dBSound);

    if (!pIntroCue || !pOutroCue || !pN60dBSound || pN60dBSound->getLengthFrames() <= 0) {
        return true;
    }
    return false;
}

// Shon auto cues.
// Energy is measured per block of this many frames (about 23 ms at 44.1 kHz).
constexpr SINT kShonBlockFrames = 1024;
// Low-pass cutoff so kicks and bass drive the energy, not hi-hats.
constexpr double kShonLowpassHz = 150.0;
// Sections are compared 8 bars (32 beats) at a time.
constexpr int kShonBeatsPerSection = 32;
// Hot cue slots, matching the IN 1 / drops 2 to 4 / OUT 8 layout.
constexpr int kShonInSlot = 0;
constexpr int kShonFirstDropSlot = 1;
constexpr int kShonMaxDrops = 2;
constexpr int kShonBreakSlot = 3;
constexpr int kShonOutSlot = 7;
constexpr mixxx::RgbColor::code_t kShonBlue = 0x2F6BFF;
constexpr mixxx::RgbColor::code_t kShonRed = 0xFF3B30;
constexpr mixxx::RgbColor::code_t kShonGreen = 0x2ECC40;

bool hasAnyHotCue(const TrackPointer& pTrack) {
    const QList<CuePointer> cues = pTrack->getCuePoints();
    return std::any_of(cues.begin(), cues.end(), [](const CuePointer& pCue) {
        return pCue && pCue->getType() == mixxx::CueType::HotCue;
    });
}

template<typename Iterator>
Iterator first_sound(Iterator begin, Iterator end) {
    return std::find_if(begin, end, [](const auto elem) {
        return fabs(elem) >= kSilenceThreshold;
    });
}

} // anonymous namespace

AnalyzerSilence::AnalyzerSilence(UserSettingsPointer pConfig)
        : m_pConfig(pConfig),
          m_doSilenceAnalysis(false),
          m_wantAutoCues(false),
          m_sampleRate(44100.0),
          m_lowpassState(0.0f),
          m_blockEnergySum(0.0),
          m_blockFrames(0),
          m_framesProcessed(0),
          m_signalStart(-1),
          m_signalEnd(-1) {
}

bool AnalyzerSilence::initialize(const AnalyzerTrack& track,
        mixxx::audio::SampleRate sampleRate,
        mixxx::audio::ChannelCount channelCount,
        SINT frameLength) {
    // Silence analysis runs when its cues are missing. Shon auto cues run
    // when the track has no hot cues yet, so nothing you set is replaced.
    m_doSilenceAnalysis = shouldAnalyze(track.getTrack());
    m_wantAutoCues = !hasAnyHotCue(track.getTrack());
    if (!m_doSilenceAnalysis && !m_wantAutoCues) {
        return false;
    }
    m_sampleRate = sampleRate.isValid() ? sampleRate.value() : 44100.0;
    m_lowpassState = 0.0f;
    m_blockEnergySum = 0.0;
    m_blockFrames = 0;
    m_blockEnergy.clear();
    if (m_wantAutoCues && frameLength > 0) {
        m_blockEnergy.reserve(frameLength / kShonBlockFrames + 1);
    }

    m_framesProcessed = 0;
    m_signalStart = -1;
    m_signalEnd = -1;
    m_channelCount = channelCount;

    return true;
}

// static
SINT AnalyzerSilence::findFirstSoundInChunk(std::span<const CSAMPLE> samples) {
    return std::distance(samples.begin(), first_sound(samples.begin(), samples.end()));
}

// static
SINT AnalyzerSilence::findLastSoundInChunk(std::span<const CSAMPLE> samples) {
    // -1 is required, because the distance from the fist sample index (0) to crend() is 1,
    SINT ret = std::distance(first_sound(samples.rbegin(), samples.rend()), samples.rend()) - 1;
    return ret;
}

// static
bool AnalyzerSilence::verifyFirstSound(
        std::span<const CSAMPLE> samples,
        mixxx::audio::FramePos firstSoundFrame,
        mixxx::audio::ChannelCount channelCount) {
    const SINT firstSoundSample = findFirstSoundInChunk(samples);
    if (firstSoundSample < static_cast<SINT>(samples.size())) {
        return mixxx::audio::FramePos::fromSamplePos(firstSoundSample, channelCount)
                       .toLowerFrameBoundary() == firstSoundFrame.toLowerFrameBoundary();
    }
    return false;
}

bool AnalyzerSilence::processSamples(const CSAMPLE* pIn, SINT count) {
    SINT numFrames = count / m_channelCount;

    std::span<const CSAMPLE> samples = mixxx::spanutil::spanFromPtrLen(pIn, count);
    if (m_signalStart < 0) {
        const SINT firstSoundSample = findFirstSoundInChunk(samples);
        if (firstSoundSample < count) {
            m_signalStart = m_framesProcessed + firstSoundSample / m_channelCount;
        }
    }
    if (m_signalStart >= 0) {
        const SINT lastSoundSample = findLastSoundInChunk(samples);
        if (lastSoundSample >= 0) {
            m_signalEnd = m_framesProcessed + lastSoundSample / m_channelCount + 1;
        }
    }

    if (m_wantAutoCues && m_channelCount > 0) {
        // One-pole low-pass on the mono mix, then mean energy per block.
        const float alpha = static_cast<float>(
                1.0 - std::exp(-2.0 * 3.14159265358979 * kShonLowpassHz / m_sampleRate));
        const int channels = m_channelCount;
        for (SINT frame = 0; frame < numFrames; ++frame) {
            float mono = 0.0f;
            for (int ch = 0; ch < channels; ++ch) {
                mono += pIn[frame * channels + ch];
            }
            mono /= static_cast<float>(channels);
            m_lowpassState += alpha * (mono - m_lowpassState);
            m_blockEnergySum += static_cast<double>(m_lowpassState) * m_lowpassState;
            if (++m_blockFrames >= kShonBlockFrames) {
                m_blockEnergy.push_back(
                        static_cast<float>(m_blockEnergySum / kShonBlockFrames));
                m_blockEnergySum = 0.0;
                m_blockFrames = 0;
            }
        }
    }

    m_framesProcessed += numFrames;
    return true;
}

void AnalyzerSilence::cleanup() {
}

void AnalyzerSilence::storeResults(TrackPointer pTrack) {
    if (m_signalStart < 0) {
        m_signalStart = 0;
    }
    if (m_signalEnd < 0) {
        m_signalEnd = m_framesProcessed;
    }

    const auto firstSoundPosition = mixxx::audio::FramePos(m_signalStart);
    const auto lastSoundPosition = mixxx::audio::FramePos(m_signalEnd);

    if (m_wantAutoCues) {
        shonPlaceAutoCues(pTrack.get());
    }
    if (!m_doSilenceAnalysis) {
        return;
    }

    CuePointer pN60dBSound = pTrack->findCueByType(mixxx::CueType::N60dBSound);
    if (pN60dBSound == nullptr) {
        pN60dBSound = pTrack->createAndAddCue(
                mixxx::CueType::N60dBSound,
                Cue::kNoHotCue,
                firstSoundPosition,
                lastSoundPosition);
    } else {
        // The user has no way to directly edit the N60dBSound cue. If the user
        // has deleted the Intro or Outro Cue, this analysis will be rerun when
        // the track is loaded again. In this case, adjust the N60dBSound Cue's
        // positions. This could be helpful, for example, when the track length
        // is changed in a different program.
        pN60dBSound->setStartAndEndPosition(firstSoundPosition, lastSoundPosition);
    }

    setupMainAndIntroCue(pTrack.get(), firstSoundPosition, m_pConfig.data());
    setupOutroCue(pTrack.get(), lastSoundPosition);
}

// static
void AnalyzerSilence::setupMainAndIntroCue(
        Track* pTrack, mixxx::audio::FramePos firstSoundPosition, UserSettings* pConfig) {
    CuePointer pIntroCue = pTrack->findCueByType(mixxx::CueType::Intro);

    mixxx::audio::FramePos mainCuePosition = pTrack->getMainCuePosition();
    mixxx::audio::FramePos introStartPosition = firstSoundPosition;
    // Before Mixxx 2.3, the default position for the main cue was 0.0. In this
    // case, move the main cue point to the first sound. This case can be
    // distinguished from a user intentionally setting the main cue position
    // to 0.0 at a later time after analysis because in that case the intro cue
    // would have already been created by this analyzer.
    bool upgradingWithMainCueAtDefault =
            (mainCuePosition == mixxx::audio::kStartFramePos &&
                    pIntroCue == nullptr);
    if (!mainCuePosition.isValid() || upgradingWithMainCueAtDefault) {
        pTrack->setMainCuePosition(firstSoundPosition);
        // NOTE: the actual default for this ConfigValue is set in DlgPrefDeck.
    } else if (pConfig->getValue(ConfigKey("[Controls]", "SetIntroStartAtMainCue"), false) &&
            pIntroCue == nullptr) {
        introStartPosition = mainCuePosition;
    }

    if (pIntroCue == nullptr) {
        pIntroCue = pTrack->createAndAddCue(
                mixxx::CueType::Intro,
                Cue::kNoHotCue,
                introStartPosition,
                mixxx::audio::kInvalidFramePos);
    }
}

// static
void AnalyzerSilence::setupOutroCue(Track* pTrack, mixxx::audio::FramePos lastSoundPosition) {
    CuePointer pOutroCue = pTrack->findCueByType(mixxx::CueType::Outro);
    if (pOutroCue == nullptr) {
        pOutroCue = pTrack->createAndAddCue(
                mixxx::CueType::Outro,
                Cue::kNoHotCue,
                mixxx::audio::kInvalidFramePos,
                lastSoundPosition);
    }
}

void AnalyzerSilence::shonPlaceAutoCues(Track* pTrack) const {
    const mixxx::BeatsPointer pBeats = pTrack->getBeats();
    if (!pBeats || m_blockEnergy.size() < 16) {
        return;
    }

    // Beats from the first sound to the last sound. The first one is IN.
    std::vector<double> beatFrames;
    const auto start = mixxx::audio::FramePos(std::max<SINT>(m_signalStart, 0));
    const auto end = mixxx::audio::FramePos(m_signalEnd > 0 ? m_signalEnd : m_framesProcessed);
    for (auto it = pBeats->iteratorFrom(start); it != pBeats->cend() && *it < end; ++it) {
        beatFrames.push_back(it->value());
    }
    const int sectionCount = static_cast<int>(beatFrames.size()) / kShonBeatsPerSection;
    if (sectionCount < 3) {
        return; // too short to judge sections
    }

    // Mean low-band energy of each 8-bar section.
    std::vector<double> energy(sectionCount, 0.0);
    double maxEnergy = 0.0;
    for (int k = 0; k < sectionCount; ++k) {
        const double from = beatFrames[k * kShonBeatsPerSection];
        const double to = (k + 1) * kShonBeatsPerSection < static_cast<int>(beatFrames.size())
                ? beatFrames[(k + 1) * kShonBeatsPerSection]
                : static_cast<double>(end.value());
        const auto firstBlock = static_cast<std::size_t>(from / kShonBlockFrames);
        const auto lastBlock = std::min(
                static_cast<std::size_t>(to / kShonBlockFrames), m_blockEnergy.size());
        if (lastBlock <= firstBlock) {
            continue;
        }
        double sum = 0.0;
        for (std::size_t b = firstBlock; b < lastBlock; ++b) {
            sum += m_blockEnergy[b];
        }
        energy[k] = sum / static_cast<double>(lastBlock - firstBlock);
        maxEnergy = std::max(maxEnergy, energy[k]);
    }
    if (maxEnergy <= 0.0) {
        return;
    }
    for (double& e : energy) {
        e /= maxEnergy;
    }

    // Drops: a loud section right after a clearly quieter one.
    struct Change {
        int section;
        double amount;
    };
    std::vector<Change> rises;
    std::vector<Change> falls;
    for (int k = 1; k < sectionCount; ++k) {
        if (energy[k] >= 0.7 && energy[k - 1] <= 0.6 * energy[k]) {
            rises.push_back({k, energy[k] - energy[k - 1]});
        }
        if (energy[k - 1] >= 0.7 && energy[k] <= 0.55 * energy[k - 1]) {
            falls.push_back({k, energy[k - 1] - energy[k]});
        }
    }
    auto biggestFirst = [](const Change& a, const Change& b) {
        return a.amount > b.amount;
    };
    std::sort(rises.begin(), rises.end(), biggestFirst);
    std::sort(falls.begin(), falls.end(), biggestFirst);
    if (static_cast<int>(rises.size()) > kShonMaxDrops) {
        rises.resize(kShonMaxDrops);
    }
    // Keep drops in track order so slot 2 is the first drop.
    std::sort(rises.begin(), rises.end(), [](const Change& a, const Change& b) {
        return a.section < b.section;
    });

    auto addCue = [pTrack](int slot, double frame, const QString& label, mixxx::RgbColor::code_t color) {
        CuePointer pCue = pTrack->createAndAddCue(mixxx::CueType::HotCue,
                slot,
                mixxx::audio::FramePos(frame),
                mixxx::audio::kInvalidFramePos,
                mixxx::RgbColor(color));
        if (pCue) {
            pCue->setLabel(label);
        }
    };
    auto sectionStart = [&beatFrames](int section) {
        return beatFrames[section * kShonBeatsPerSection];
    };

    addCue(kShonInSlot, beatFrames.front(), QStringLiteral("IN"), kShonBlue);
    for (int i = 0; i < static_cast<int>(rises.size()); ++i) {
        addCue(kShonFirstDropSlot + i,
                sectionStart(rises[i].section),
                QStringLiteral("DROP"),
                kShonGreen);
    }
    if (!falls.empty()) {
        addCue(kShonBreakSlot, sectionStart(falls.front().section),
                QStringLiteral("BREAK"), kShonGreen);
    }
    // OUT: start of the last 16 bars, after the last drop.
    const int lastDrop = rises.empty() ? 0 : rises.back().section;
    const int outSection = std::max(sectionCount - 2, lastDrop + 1);
    if (outSection > 0 && outSection < sectionCount) {
        addCue(kShonOutSlot, sectionStart(outSection), QStringLiteral("OUT"), kShonRed);
    }
}
