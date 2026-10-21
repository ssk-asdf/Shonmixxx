#include "widget/shoncurtain.h"

#include <QApplication>
#include <QCursor>
#include <QKeyEvent>
#include <QPainter>
#include <QShortcut>
#include <QTimer>

#include "control/controlobject.h"
#include "control/controlpushbutton.h"
#include "moc_shoncurtain.cpp"

namespace {

const QString kGroup = QStringLiteral("[Shon]");
const ConfigKey kIdleMinutesKey(kGroup, QStringLiteral("curtain_idle_minutes"));
constexpr int kDefaultIdleMinutes = 5;
const QColor kBackground(18, 18, 20); // same as the loading screen
constexpr int kLogoWidth = 340;       // same as the loading screen
// Ignore input for a moment after showing, so the key or click that opened
// it doesn't close it again straight away.
constexpr int kGraceMs = 600;

} // namespace

ShonCurtain::ShonCurtain(UserSettingsPointer pConfig, QWidget* pMainWindow)
        : QWidget(pMainWindow, Qt::Tool | Qt::FramelessWindowHint),
          m_pConfig(pConfig),
          m_pMainWindow(pMainWindow),
          m_logo(QStringLiteral(":/images/shondj-logo.png")) {
    setObjectName(QStringLiteral("ShonCurtain"));
    setAttribute(Qt::WA_OpaquePaintEvent);
    setCursor(Qt::BlankCursor);
    hide();

    m_pShow = std::make_unique<ControlPushButton>(ConfigKey(kGroup, QStringLiteral("curtain")));
    m_pShow->setButtonMode(mixxx::control::ButtonMode::Toggle);
    connect(m_pShow.get(), &ControlObject::valueChanged, this, [this](double value) {
        if (value > 0.0) {
            showCurtain();
        } else {
            hideCurtain();
        }
    });
    m_pAuto = std::make_unique<ControlPushButton>(
            ConfigKey(kGroup, QStringLiteral("curtain_auto")), true, 1.0);
    m_pAuto->setButtonMode(mixxx::control::ButtonMode::Toggle);

    auto* pShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+L")), pMainWindow);
    pShortcut->setContext(Qt::ApplicationShortcut);
    connect(pShortcut, &QShortcut::activated, this, [this]() { showCurtain(); });

    qApp->installEventFilter(this);
    m_idle.start();
    m_lastCursor = QCursor::pos();
    m_pIdleTimer = new QTimer(this);
    m_pIdleTimer->setInterval(2000);
    connect(m_pIdleTimer, &QTimer::timeout, this, &ShonCurtain::checkIdle);
    m_pIdleTimer->start();
}

ShonCurtain::~ShonCurtain() {
    qApp->removeEventFilter(this);
}

bool ShonCurtain::mixing() const {
    if (ControlObject::get(ConfigKey(QStringLiteral("[AutoDJ]"),
                QStringLiteral("shon_away_mode"))) > 0.0) {
        return false;
    }
    for (int deck = 1; deck <= 4; ++deck) {
        if (ControlObject::get(ConfigKey(QStringLiteral("[Channel%1]").arg(deck),
                    QStringLiteral("play"))) > 0.0) {
            return true;
        }
    }
    return false;
}

void ShonCurtain::checkIdle() {
    // The cursor moving anywhere counts as being here, even outside ShonDJ.
    const QPoint cursor = QCursor::pos();
    if ((cursor - m_lastCursor).manhattanLength() > 3) {
        m_lastCursor = cursor;
        m_idle.restart();
        if (isVisible() && m_shownFor.elapsed() > kGraceMs) {
            hideCurtain();
        }
        return;
    }
    if (isVisible() || m_pAuto->get() <= 0.0 || mixing()) {
        return;
    }
    const int minutes = m_pConfig->getValue(kIdleMinutesKey, kDefaultIdleMinutes);
    if (minutes > 0 && m_idle.elapsed() >= minutes * 60000LL) {
        showCurtain();
    }
}

void ShonCurtain::showCurtain() {
    if (!m_pMainWindow) {
        return;
    }
    setGeometry(m_pMainWindow->frameGeometry());
    m_lastCursor = QCursor::pos();
    m_shownFor.start();
    show();
    raise();
    activateWindow();
    if (m_pShow->get() <= 0.0) {
        m_pShow->set(1.0);
    }
}

void ShonCurtain::hideCurtain() {
    m_idle.restart();
    if (isVisible()) {
        hide();
        if (m_pMainWindow) {
            m_pMainWindow->activateWindow();
        }
    }
    if (m_pShow->get() > 0.0) {
        m_pShow->set(0.0);
    }
}

bool ShonCurtain::eventFilter(QObject* pObject, QEvent* pEvent) {
    switch (pEvent->type()) {
    case QEvent::KeyPress:
    case QEvent::MouseButtonPress:
    case QEvent::Wheel:
    case QEvent::TouchBegin:
        m_idle.restart();
        if (isVisible()) {
            if (m_shownFor.elapsed() > kGraceMs) {
                hideCurtain();
            }
            // The press that wakes ShonDJ shouldn't also hit a button.
            return true;
        }
        break;
    case QEvent::MouseMove:
        m_idle.restart();
        if (isVisible() && m_shownFor.elapsed() > kGraceMs) {
            hideCurtain();
        }
        break;
    default:
        break;
    }
    return QWidget::eventFilter(pObject, pEvent);
}

void ShonCurtain::paintEvent(QPaintEvent* pEvent) {
    Q_UNUSED(pEvent);
    QPainter p(this);
    p.fillRect(rect(), kBackground);
    if (m_logo.isNull()) {
        return;
    }
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const double width = kLogoWidth;
    const double height = width * m_logo.height() / m_logo.width();
    p.drawPixmap(QRectF((this->width() - width) / 2.0,
                         (this->height() - height) / 2.0,
                         width,
                         height),
            m_logo,
            QRectF(m_logo.rect()));
}
