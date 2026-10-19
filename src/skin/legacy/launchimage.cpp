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
// The spinner is a ring seen through two half-plane clips. Each clip spins
// at its own steady speed, so the visible arc's two ends move at different
// speeds and chase each other. Only RotationAnimators are used, and those
// run on Qt Quick's render thread, which keeps going while the main thread
// is busy starting Mixxx.
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
        width: 38
        height: 38
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: logo.bottom
        anchors.topMargin: 44

        // Faint full ring behind the arc.
        Rectangle {
            anchors.fill: parent
            radius: width / 2
            color: "transparent"
            border.width: 3.5
            border.color: "#1affffff"
        }

        // First half-plane clip: right half of the spinner, spun around
        // the spinner's center (its left edge midpoint).
        Item {
            id: outerClip
            x: spinner.width / 2
            width: spinner.width / 2
            height: spinner.height
            clip: true
            transformOrigin: Item.Left

            // Second half-plane clip, spun relative to the first.
            Item {
                id: innerClip
                x: 0
                width: spinner.width / 2
                height: spinner.height
                clip: true
                transformOrigin: Item.Left

                Rectangle {
                    x: -spinner.width / 2
                    width: spinner.width
                    height: spinner.height
                    radius: width / 2
                    color: "transparent"
                    border.width: 3.5
                    border.color: "#2f6bff"
                    antialiasing: true
                }
            }
        }

        RotationAnimator {
            target: outerClip
            from: 0
            to: 360
            duration: 1000
            loops: Animation.Infinite
            running: true
        }
        RotationAnimator {
            target: innerClip
            from: 0
            to: 360
            duration: 2400
            loops: Animation.Infinite
            running: true
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
