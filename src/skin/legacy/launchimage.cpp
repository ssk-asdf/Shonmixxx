#include "skin/legacy/launchimage.h"

#include <QDebug>
#include <QFont>
#include <QPainter>
#include <QPen>
#include <QRectF>
#include <cmath>

#include "moc_launchimage.cpp"

#ifdef MIXXX_USE_QML
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QScreen>
#endif

namespace {
const QColor kBackgroundColor(18, 18, 20);
const QColor kSpinnerColor(47, 107, 255);      // Shon blue
const QColor kSpinnerTrackColor(255, 255, 255, 20);
const QColor kStatusColor(150, 150, 158);
constexpr int kLogoWidth = 340;
constexpr int kSpinnerSize = 34;
constexpr double kSpinnerThickness = 3.5;
constexpr int kFrameIntervalMs = 16;
// One grow and shrink of the arc, in seconds.
constexpr double kCycleSeconds = 1.4;
// Steady spin added on top, in degrees per second.
constexpr double kSpinDegreesPerSecond = 160.0;
constexpr double kMaxSweepDegrees = 270.0;

double easeInOut(double x) {
    return x * x * (3.0 - 2.0 * x);
}

#ifdef MIXXX_USE_QML
// The spinner recreates the original look: one continuous arc with round
// ends that grows to 270 degrees and shrinks to a short sliver, its ends
// chasing each other, while the whole arc spins. It never closes.
//
// Qt Quick can only keep animations running during startup if they are
// Animators (rotation, opacity), which run on the render thread. So the arc
// is built from a ring seen through rotated half-circle clips:
//  - Short arcs (12 to 180 degrees): the overlap of two half circles.
//  - Long arcs (180 to 270 degrees): the union of two half circles.
// Rotating one half circle grows or shrinks the arc, and the two layers
// hand off at exactly 180 degrees where they look identical. Small dots
// ride on the arc ends to give them round caps.
const char kSplashQml[] = R"QML(
import QtQuick
import QtQuick.Window

Window {
    id: root
    property string status: "Starting up"
    width: 560
    height: 380
    color: "#121214"
    flags: Qt.SplashScreen | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    visible: true

    Image {
        id: logo
        source: "qrc:/images/shondj-logo.png"
        width: 340
        fillMode: Image.PreserveAspectFit
        smooth: true
        mipmap: true
        anchors.horizontalCenter: parent.horizontalCenter
        y: 72
    }

    Item {
        id: spinner
        readonly property real s: 38
        readonly property real t: 3.5
        readonly property color arcColor: "#2f6bff"
        width: s
        height: s
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: logo.bottom
        anchors.topMargin: 44

        component Ring: Rectangle {
            width: spinner.s
            height: spinner.s
            radius: spinner.s / 2
            color: "transparent"
            border.width: spinner.t
            border.color: spinner.arcColor
            antialiasing: true
        }
        // Round cap at the top of the ring (angle 0) in its parent's frame.
        component TopCap: Rectangle {
            x: (spinner.s - spinner.t) / 2
            y: 0
            width: spinner.t
            height: spinner.t
            radius: spinner.t / 2
            color: spinner.arcColor
            antialiasing: true
        }
        // Round cap at the bottom of the ring (angle 180).
        component BottomCap: TopCap {
            y: spinner.s - spinner.t
        }
        // Right half of the ring area, clipped: shows angles 0 to 180.
        component HalfClip: Item {
            x: spinner.s / 2
            width: spinner.s / 2
            height: spinner.s
            clip: true
        }
        component Frame: Item {
            width: spinner.s
            height: spinner.s
            transformOrigin: Item.Center
        }

        // Faint full ring behind the arc.
        Rectangle {
            anchors.fill: parent
            radius: width / 2
            color: "transparent"
            border.width: spinner.t
            border.color: "#1affffff"
            antialiasing: true
        }

        Frame {
            id: spin

            // Short arcs: overlap of half circle [0, 180] and [b, b + 180],
            // which is the arc [b, 180].
            Item {
                id: shortLayer
                anchors.fill: parent
                HalfClip {
                    Frame {
                        id: shortRotor
                        x: -spinner.s / 2
                        rotation: 168
                        HalfClip {
                            Ring { x: -spinner.s / 2 }
                        }
                    }
                }
                Frame {
                    id: shortTailRotor
                    rotation: 168
                    TopCap {}
                }
                BottomCap {}
            }

            // Long arcs: union of half circle [0, 180] and [b, b + 180],
            // which is the arc [0, b + 180].
            Item {
                id: longLayer
                anchors.fill: parent
                opacity: 0
                HalfClip {
                    Ring { x: -spinner.s / 2 }
                }
                Frame {
                    id: longRotor
                    rotation: 0
                    HalfClip {
                        Ring { x: -spinner.s / 2 }
                    }
                    BottomCap {}
                }
                TopCap {}
            }
        }

        // Steady spin of the whole arc.
        RotationAnimator {
            target: spin
            from: 0
            to: 360
            duration: 1000
            loops: Animation.Infinite
            running: true
        }

        // Grow to 270, shrink back to a sliver. Animators only, so the
        // whole sequence runs on the render thread.
        SequentialAnimation {
            loops: Animation.Infinite
            running: true

            // Timing keeps both arc ends always moving forward with the
            // spin (360 deg/s) and every speed change smooth:
            // the tail nearly holds while the head races ahead, then the
            // tail catches up.
            // Grow 12 to 180 (short layer).
            ParallelAnimation {
                RotationAnimator { target: shortRotor; from: 168; to: 0; duration: 900; easing.type: Easing.InOutQuad }
                RotationAnimator { target: shortTailRotor; from: 168; to: 0; duration: 900; easing.type: Easing.InOutQuad }
            }
            // Hand off to the long layer at 180.
            ParallelAnimation {
                OpacityAnimator { target: shortLayer; to: 0; duration: 1 }
                OpacityAnimator { target: longLayer; to: 1; duration: 1 }
            }
            // Grow 180 to 270, then shrink back to 180 (long layer).
            RotationAnimator { target: longRotor; from: 0; to: 90; duration: 300; easing.type: Easing.InOutQuad }
            RotationAnimator { target: longRotor; from: 90; to: 0; duration: 520; easing.type: Easing.InOutQuad }
            // Hand back to the short layer at 180.
            ParallelAnimation {
                OpacityAnimator { target: longLayer; to: 0; duration: 1 }
                OpacityAnimator { target: shortLayer; to: 1; duration: 1 }
            }
            // Shrink 180 to 12: the tail chases the head.
            ParallelAnimation {
                RotationAnimator { target: shortRotor; from: 0; to: 168; duration: 650; easing.type: Easing.InOutQuad }
                RotationAnimator { target: shortTailRotor; from: 0; to: 168; duration: 650; easing.type: Easing.InOutQuad }
            }
        }
    }

    Text {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: spinner.bottom
        anchors.topMargin: 22
        text: root.status
        color: "#96969e"
        font.pixelSize: 13
        font.letterSpacing: 0.6
    }
}
)QML";
#endif
} // namespace

LaunchImage::LaunchImage(QWidget* pParent, const QString& styleSheet)
        : QWidget(pParent),
          m_pQmlEngine(nullptr),
          m_pSplashWindow(nullptr),
          m_logo(QStringLiteral(":/images/shondj-logo.png")),
          m_progress(0) {
    // Shon branding replaces any skin-defined launch style.
    Q_UNUSED(styleSheet);
    setAttribute(Qt::WA_OpaquePaintEvent);
    m_status = tr("Starting up");
    m_clock.start();
    connect(&m_animationTimer, &QTimer::timeout, this, [this]() {
        update();
    });

#ifdef MIXXX_USE_QML
    m_pQmlEngine = new QQmlEngine(this);
    QQmlComponent component(m_pQmlEngine);
    component.setData(QByteArray(kSplashQml), QUrl());
    QObject* pRoot = component.create();
    m_pSplashWindow = qobject_cast<QQuickWindow*>(pRoot);
    if (m_pSplashWindow) {
        if (const QScreen* pScreen = QGuiApplication::primaryScreen()) {
            const QRect area = pScreen->availableGeometry();
            m_pSplashWindow->setPosition(
                    area.center() - QPoint(m_pSplashWindow->width() / 2,
                                            m_pSplashWindow->height() / 2));
        }
        m_pSplashWindow->show();
        // Let the window appear so the render thread starts animating.
        QCoreApplication::processEvents();
        return;
    }
    if (pRoot) {
        delete pRoot;
    }
    qWarning() << "Shon splash: Qt Quick window failed, using painted spinner"
               << component.errorString();
#endif
    // Fallback: paint the spinner inside the main window.
    m_animationTimer.start(kFrameIntervalMs);
}

LaunchImage::~LaunchImage() {
    closeSplashWindow();
}

void LaunchImage::closeSplashWindow() {
    if (m_pSplashWindow) {
        m_pSplashWindow->close();
        delete m_pSplashWindow;
        m_pSplashWindow = nullptr;
    }
}

void LaunchImage::progress(int value, const QString& serviceName) {
    m_progress = value;
    if (!serviceName.isEmpty()) {
        m_status = tr("Loading %1").arg(serviceName);
    }
    const QString text = value > 0
            ? QStringLiteral("%1  %2%").arg(m_status).arg(value)
            : m_status;
    if (m_pSplashWindow) {
        m_pSplashWindow->setProperty("status", text);
        if (value >= 100) {
            closeSplashWindow();
        }
        return;
    }
    if (value >= 100) {
        m_animationTimer.stop();
    }
    // Startup runs on this thread, so paint right away.
    repaint();
}

void LaunchImage::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.fillRect(rect(), kBackgroundColor);
    if (m_pSplashWindow) {
        // The splash window shows everything; keep this area plain.
        return;
    }

    // Logo, scaled to a fixed width.
    QSizeF logoSize(kLogoWidth, 0);
    if (!m_logo.isNull()) {
        logoSize.setHeight(kLogoWidth * m_logo.height() / static_cast<double>(m_logo.width()));
    }
    const double gapLogoSpinner = 44.0;
    const double gapSpinnerText = 22.0;
    const double textHeight = 18.0;
    const double totalHeight = logoSize.height() + gapLogoSpinner + kSpinnerSize +
            gapSpinnerText + textHeight;
    double y = (height() - totalHeight) / 2.0;
    const double centerX = width() / 2.0;

    if (!m_logo.isNull()) {
        const QRectF logoRect(centerX - logoSize.width() / 2.0,
                y,
                logoSize.width(),
                logoSize.height());
        painter.drawPixmap(logoRect, m_logo, QRectF(m_logo.rect()));
    }
    y += logoSize.height() + gapLogoSpinner;

    // Spinner: a faint ring with one continuous arc on top. The arc's head
    // races ahead, then its tail catches up, while the whole thing spins.
    const QRectF spinnerRect(centerX - kSpinnerSize / 2.0,
            y,
            kSpinnerSize,
            kSpinnerSize);
    QPen trackPen(kSpinnerTrackColor, kSpinnerThickness);
    painter.setPen(trackPen);
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(spinnerRect);

    const double seconds = m_clock.elapsed() / 1000.0;
    const double cycle = seconds / kCycleSeconds;
    const double cycleIndex = std::floor(cycle);
    const double phase = cycle - cycleIndex;
    double head;
    double tail;
    if (phase < 0.5) {
        head = easeInOut(phase * 2.0) * kMaxSweepDegrees;
        tail = 0.0;
    } else {
        head = kMaxSweepDegrees;
        tail = easeInOut((phase - 0.5) * 2.0) * kMaxSweepDegrees;
    }
    // Each cycle starts where the last one ended, so the line never jumps.
    const double base = std::fmod(cycleIndex * kMaxSweepDegrees +
                    seconds * kSpinDegreesPerSecond,
            360.0);
    const double sweep = std::max(head - tail, 12.0);
    const double start = base + tail;

    QPen arcPen(kSpinnerColor, kSpinnerThickness, Qt::SolidLine, Qt::RoundCap);
    painter.setPen(arcPen);
    // Qt angles are in 1/16 degree and run counterclockwise; negate for clockwise.
    painter.drawArc(spinnerRect,
            static_cast<int>(-start * 16.0),
            static_cast<int>(-sweep * 16.0));
    y += kSpinnerSize + gapSpinnerText;

    // Status line under the spinner.
    QFont font = painter.font();
    font.setPointSizeF(10.5);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
    painter.setFont(font);
    painter.setPen(kStatusColor);
    const QString text = m_progress > 0
            ? QStringLiteral("%1  %2%").arg(m_status).arg(m_progress)
            : m_status;
    painter.drawText(QRectF(0, y, width(), textHeight), Qt::AlignHCenter | Qt::AlignVCenter, text);
}
