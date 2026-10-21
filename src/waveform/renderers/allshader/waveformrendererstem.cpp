#include "waveform/renderers/allshader/waveformrendererstem.h"

#include <QFont>
#include <QVector2D>
#include <QVector4D>
#include <algorithm>
#include <cmath>
#include <QImage>
#include <QOpenGLTexture>

#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "engine/channels/enginedeck.h"
#include "engine/engine.h"
#include "rendergraph/material/rgbamaterial.h"
#include "rendergraph/vertexupdaters/rgbavertexupdater.h"
#include "track/track.h"
#include "util/assert.h"
#include "util/math.h"
#include "waveform/renderers/waveformwidgetrenderer.h"
#include "waveform/waveform.h"
#include "waveform/waveformwidgetfactory.h"


using namespace rendergraph;

namespace allshader {

WaveformRendererStem::WaveformRendererStem(
        WaveformWidgetRenderer* waveformWidget,
        ::WaveformRendererAbstract::PositionSource type,
        ::WaveformRendererSignalBase::Options options)
        : WaveformRendererSignalBase(waveformWidget, options),
          m_isSlipRenderer(type == ::WaveformRendererAbstract::Slip),
          m_splitStemTracks(false),
          m_outlineOpacity(0.15f),
          m_opacity(0.75f) {
    initForRectangles<RGBAMaterial>(0);
    setUsePreprocess(true);
}

void WaveformRendererStem::onSetup(const QDomNode&) {
}

bool WaveformRendererStem::init() {
    m_pStemGain.clear();
    m_pStemMute.clear();

    // Keep a valid drawing order even for displays that provide a static track
    // without a channel group. The stem data itself is independent of the
    // per-stem volume/mute controls, so those controls are optional.
    m_stackOrder.resize(mixxx::kMaxSupportedStems);
    std::iota(m_stackOrder.begin(), m_stackOrder.end(), 0);

    if (m_waveformRenderer->getGroup().isEmpty()) {
        return true;
    }
    for (int stemIdx = 0; stemIdx < mixxx::kMaxSupportedStems; stemIdx++) {
        QString stemGroup = EngineDeck::getGroupForStem(m_waveformRenderer->getGroup(), stemIdx);
        m_pStemGain.emplace_back(
                std::make_unique<ControlProxy>(stemGroup,
                        QStringLiteral("volume")));
        m_pStemMute.emplace_back(
                std::make_unique<ControlProxy>(stemGroup,
                        QStringLiteral("mute")));
        auto bringToForeground = [this, stemIdx](double) {
            if (!m_reorderOnChange) {
                return;
            }
            m_stackOrder.removeAll(stemIdx);
            m_stackOrder.append(stemIdx);
        };
        m_pStemGain.back()->connectValueChanged(this, bringToForeground);
        m_pStemMute.back()->connectValueChanged(this, bringToForeground);
    }

#ifndef __SCENEGRAPH__
    auto* pWaveformWidgetFactory = WaveformWidgetFactory::instance();
    setSplitStemTracks(pWaveformWidgetFactory->isStemSplitTracks());
    connect(pWaveformWidgetFactory,
            &WaveformWidgetFactory::stemSplitTracksChanged,
            this,
            &WaveformRendererStem::setSplitStemTracks);
    setReorderOnChange(pWaveformWidgetFactory->isStemReorderOnChange());
    connect(pWaveformWidgetFactory,
            &WaveformWidgetFactory::stemReorderOnChangeChanged,
            this,
            &WaveformRendererStem::setReorderOnChange);
    setOutlineOpacity(pWaveformWidgetFactory->getStemOutlineOpacity());
    connect(pWaveformWidgetFactory,
            &WaveformWidgetFactory::stemOutlineOpacityChanged,
            this,
            &WaveformRendererStem::setOutlineOpacity);
    setOpacity(pWaveformWidgetFactory->getStemOpacity());
    connect(pWaveformWidgetFactory,
            &WaveformWidgetFactory::stemOpacityChanged,
            this,
            &WaveformRendererStem::setOpacity);
#endif
    return true;
}

void WaveformRendererStem::preprocess() {
    if (!preprocessInner()) {
        if (geometry().vertexCount() != 0) {
            geometry().allocate(0);
            markDirtyGeometry();
        }
    }
}

bool WaveformRendererStem::preprocessInner() {
    // Shon: Spotify style level lines. Each stem gets a thin line whose height
    // is its level (top is full, bottom is off). Behind the playhead the line
    // shows what you did, so a slow fade draws a ramp. Ahead it stays flat at
    // the current level. Only drawn when stem lines are on.
    TrackPointer pTrack = m_waveformRenderer->getTrackInfo();
    if (!pTrack || m_isSlipRenderer) {
        return false;
    }
    if (ControlObject::get(ConfigKey(QStringLiteral("[Shon]"),
                QStringLiteral("stem_lines"))) <= 0.0) {
        return false;
    }
    const auto stemInfo = pTrack->getStemInfo();
    const int stemCount = std::min(static_cast<int>(stemInfo.size()),
            static_cast<int>(mixxx::kMaxSupportedStems));
    if (stemCount <= 0) {
        return false;
    }
    const double trackSamples = m_waveformRenderer->getTrackSamples();
    if (trackSamples <= 0) {
        return false;
    }

    // New song: start a fresh trail.
    if (pTrack.get() != m_pShonTrack) {
        m_pShonTrack = pTrack.get();
        m_shonHistory.clear();
    }

    // Current level of each stem, 0..1.
    ShonPoint now;
    now.pos = m_waveformRenderer->getTruePosSample(::WaveformRendererAbstract::Play);
    for (int stemIdx = 0; stemIdx < mixxx::kMaxSupportedStems; stemIdx++) {
        float level = 1.f;
        if (stemIdx < static_cast<int>(m_pStemMute.size()) && m_pStemMute[stemIdx]->toBool()) {
            level = 0.f;
        } else if (stemIdx < static_cast<int>(m_pStemGain.size())) {
            level = static_cast<float>(m_pStemGain[stemIdx]->get());
        }
        now.level[stemIdx] = std::clamp(level, 0.f, 1.f);
    }

    // Record the trail: on every change, and a point now and then while playing.
    if (!m_shonHistory.empty() && now.pos < m_shonHistory.back().pos - 1.0) {
        // Jumped back (cue, loop, seek): forget what was after this point.
        while (!m_shonHistory.empty() && m_shonHistory.back().pos > now.pos) {
            m_shonHistory.pop_back();
        }
    }
    const double samplesPerSecond = trackSamples /
            std::max(1.0, static_cast<double>(pTrack->getDuration()));
    bool record = m_shonHistory.empty();
    if (!record) {
        const ShonPoint& last = m_shonHistory.back();
        for (int stemIdx = 0; stemIdx < stemCount; stemIdx++) {
            if (std::abs(last.level[stemIdx] - now.level[stemIdx]) > 0.002f) {
                record = true;
            }
        }
        if (now.pos - last.pos > samplesPerSecond * 0.5) {
            record = true;
        }
        // A sudden change (mute) gets a corner, not a long ramp.
        if (record && now.pos - last.pos > samplesPerSecond * 0.1) {
            bool jumped = false;
            for (int stemIdx = 0; stemIdx < stemCount; stemIdx++) {
                if (std::abs(last.level[stemIdx] - now.level[stemIdx]) > 0.002f) {
                    jumped = true;
                }
            }
            if (jumped) {
                ShonPoint corner = last;
                corner.pos = now.pos - 1.0;
                m_shonHistory.push_back(corner);
            }
        }
    }
    if (record) {
        m_shonHistory.push_back(now);
        if (m_shonHistory.size() > 50000) {
            m_shonHistory.erase(m_shonHistory.begin(), m_shonHistory.begin() + 10000);
        }
    }

    const float length = static_cast<float>(m_waveformRenderer->getLength());
    const float breadth = static_cast<float>(m_waveformRenderer->getBreadth());
    const float pad = 6.f;
    const float lineHalf = 0.8f;
    const float playX = static_cast<float>(
            m_waveformRenderer->transformSamplePositionInRendererWorld(now.pos));
    const auto yFor = [&](float level, int stemIdx) {
        // Small offset per stem so lines at the same level stay visible.
        const float top = pad + stemIdx * 2.5f;
        const float bottom = breadth - pad - (stemCount - 1 - stemIdx) * 2.5f;
        return bottom - level * (bottom - top);
    };

    // Drums white, bass blue, melody orange, vocals cream.
    static const float kShonStemColors[4][3] = {
            {0.92f, 0.92f, 0.94f},
            {0.18f, 0.42f, 1.00f},
            {0.95f, 0.64f, 0.23f},
            {1.00f, 0.88f, 0.70f},
    };

    struct Segment {
        float x1, y1, x2, y2;
        float r, g, b, a;
    };
    std::vector<Segment> segments;
    segments.reserve(1024);
    for (int stemIdx = 0; stemIdx < stemCount; stemIdx++) {
        const float* c = kShonStemColors[stemIdx % 4];
        // Past: the recorded trail, only the visible part.
        float prevX = 0.f;
        float prevY = 0.f;
        bool havePrev = false;
        for (const ShonPoint& point : m_shonHistory) {
            const float x = static_cast<float>(
                    m_waveformRenderer->transformSamplePositionInRendererWorld(point.pos));
            const float y = yFor(point.level[stemIdx], stemIdx);
            if (havePrev && !(prevX > length + 2.f || x < -2.f)) {
                if (std::abs(x - prevX) > 0.3f || std::abs(y - prevY) > 0.3f) {
                    segments.push_back({prevX, prevY, x, y, c[0], c[1], c[2], 0.9f});
                }
            }
            prevX = x;
            prevY = y;
            havePrev = true;
        }
        const float yNow = yFor(now.level[stemIdx], stemIdx);
        if (havePrev && std::abs(playX - prevX) > 0.3f) {
            segments.push_back({prevX, prevY, playX, yNow, c[0], c[1], c[2], 0.9f});
        }
        // Future: flat at the current level, a bit dimmer.
        segments.push_back({playX, yNow, length, yNow, c[0], c[1], c[2], 0.45f});
        // Dot at the playhead.
        segments.push_back({playX - 2.5f, yNow, playX + 2.5f, yNow, c[0], c[1], c[2], 1.0f});
    }

    const int reserved = static_cast<int>(segments.size()) * 6;
    geometry().setDrawingMode(Geometry::DrawingMode::Triangles);
    geometry().allocate(reserved);
    markDirtyGeometry();
    RGBAVertexUpdater vertexUpdater{geometry().vertexDataAs<Geometry::RGBAColoredPoint2D>()};
    for (const Segment& seg : segments) {
        float dx = seg.x2 - seg.x1;
        float dy = seg.y2 - seg.y1;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 0.001f) {
            dx = 1.f;
            dy = 0.f;
        } else {
            dx /= len;
            dy /= len;
        }
        // Thicker for the playhead dot.
        const float half = (seg.a >= 1.0f) ? 2.0f : lineHalf;
        const float nx = -dy * half;
        const float ny = dx * half;
        const QVector4D color(seg.r, seg.g, seg.b, seg.a);
        vertexUpdater.addTriangle(QVector2D(seg.x1 + nx, seg.y1 + ny),
                QVector2D(seg.x2 + nx, seg.y2 + ny),
                QVector2D(seg.x1 - nx, seg.y1 - ny),
                color);
        vertexUpdater.addTriangle(QVector2D(seg.x1 - nx, seg.y1 - ny),
                QVector2D(seg.x2 + nx, seg.y2 + ny),
                QVector2D(seg.x2 - nx, seg.y2 - ny),
                color);
    }
    markDirtyMaterial();
    return true;
}

} // namespace allshader
