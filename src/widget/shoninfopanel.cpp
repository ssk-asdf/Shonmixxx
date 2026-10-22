#include "widget/shoninfopanel.h"

#include <QLabel>
#include <QTimer>
#include <QVBoxLayout>

#include "control/controlobject.h"
#include "library/library.h"
#include "moc_shoninfopanel.cpp"
#include "track/track.h"

namespace {

QLabel* makeLabel(QWidget* pParent, const QString& style) {
    auto* pLabel = new QLabel(pParent);
    pLabel->setStyleSheet(style);
    pLabel->setTextFormat(Qt::RichText);
    pLabel->setWordWrap(false);
    pLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    return pLabel;
}

QString row(const QString& name, const QString& value) {
    return QStringLiteral("<span style='color:#7d7d86'>%1</span><br>%2")
            .arg(name, value.toHtmlEscaped());
}

} // namespace

ShonInfoPanel::ShonInfoPanel(QWidget* pParent, UserSettingsPointer pConfig, Library* pLibrary)
        : WWidget(pParent),
          m_pConfig(pConfig),
          m_pLibrary(pLibrary) {
    const QString base = QStringLiteral("color:#e8e8ee; font-size:12px; background:transparent;");
    m_pTitle = makeLabel(this, base + QStringLiteral("font-weight:600;"));
    m_pBpmKey = makeLabel(this, QStringLiteral("color:#4d8dff; font-size:15px; font-weight:700;"));
    m_pTonight = makeLabel(this, base);
    m_pUpNext = makeLabel(this, base);
    m_pRequests = makeLabel(this, base);

    auto* pLayout = new QVBoxLayout(this);
    pLayout->setContentsMargins(10, 6, 6, 6);
    pLayout->setSpacing(6);
    pLayout->addWidget(m_pTitle);
    pLayout->addWidget(m_pBpmKey);
    pLayout->addSpacing(4);
    pLayout->addWidget(m_pTonight);
    pLayout->addWidget(m_pUpNext);
    pLayout->addWidget(m_pRequests);
    pLayout->addStretch();

    // Crate, queue and requests change in the background; a light poll is enough.
    m_pTimer = new QTimer(this);
    connect(m_pTimer, &QTimer::timeout, this, &ShonInfoPanel::refresh);
    m_pTimer->start(2000);
    refresh();
}

void ShonInfoPanel::slotTrackSelected(TrackPointer pTrack) {
    m_pTrack = pTrack;
    refresh();
}

void ShonInfoPanel::refresh() {
    if (m_pTrack) {
        const QString title = m_pTrack->getTitle();
        const QString elided = m_pTitle->fontMetrics().elidedText(
                title, Qt::ElideRight, std::max(40, m_pTitle->width()));
        m_pTitle->setText(elided.toHtmlEscaped());
        QString bpmKey = m_pTrack->getBpm() > 0
                ? QString::number(m_pTrack->getBpm(), 'f', 1) + QStringLiteral(" BPM")
                : QString();
        const QString key = m_pTrack->getKeyText();
        if (!key.isEmpty()) {
            bpmKey += (bpmKey.isEmpty() ? QString() : QStringLiteral("  ")) + key;
        }
        m_pBpmKey->setText(bpmKey.toHtmlEscaped());
    } else {
        m_pTitle->setText(QStringLiteral("<span style='color:#7d7d86'>Select a song</span>"));
        m_pBpmKey->clear();
    }

    const QString tonight = m_pConfig
                                    ->getValueString(ConfigKey(QStringLiteral("[Shon]"),
                                            QStringLiteral("requests_crate")))
                                    .trimmed();
    m_pTonight->setText(row(QStringLiteral("Tonight"),
            tonight.isEmpty() ? QStringLiteral("Whole library")
                              : QStringLiteral("\u25CF ") + tonight));

    const QString upNext = m_pLibrary ? m_pLibrary->shonUpNext() : QString();
    m_pUpNext->setText(row(QStringLiteral("Up next"),
            upNext.isEmpty() ? QStringLiteral("Queue empty") : upNext));

    const int waiting = static_cast<int>(ControlObject::get(
            ConfigKey(QStringLiteral("[Shon]"), QStringLiteral("requests_waiting"))));
    m_pRequests->setText(row(QStringLiteral("Requests"),
            waiting > 0 ? QStringLiteral("%1 waiting").arg(waiting) : QStringLiteral("None")));
}
