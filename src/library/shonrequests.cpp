#include "library/shonrequests.h"

#include <QApplication>
#include <QSvgRenderer>
#include <QStandardPaths>
#include <QPainter>
#include <QImage>
#include <QFileDialog>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDrag>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QStyle>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSqlQuery>
#include <QSvgWidget>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QVariant>
#include <algorithm>
#include <cmath>

#include "control/controlpushbutton.h"
#include "library/library.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "mixer/playerinfo.h"
#include "moc_shonrequests.cpp"
#include "track/keyutils.h"
#include "track/track.h"

namespace {

const QString kGroup = QStringLiteral("[Shon]");
const ConfigKey kRelayUrlKey(kGroup, QStringLiteral("requests_relay_url"));
const ConfigKey kCodeKey(kGroup, QStringLiteral("requests_code"));
const ConfigKey kHostKeyKey(kGroup, QStringLiteral("requests_key"));
const ConfigKey kShopUrlKey(kGroup, QStringLiteral("requests_shop_url"));
const ConfigKey kOpenKey(kGroup, QStringLiteral("requests_open"));
const ConfigKey kCrateKey(kGroup, QStringLiteral("requests_crate"));
const ConfigKey kHandleKey(kGroup, QStringLiteral("requests_handle"));
const ConfigKey kHandleSecretKey(kGroup, QStringLiteral("requests_handle_key"));

// Tip, follow and booking links shown to guests: config item, relay field, hint.
struct ProfileField {
    const char* item;
    const char* field;
    const char* hint;
};
const ProfileField kProfileFields[] = {
        {"requests_name", "name", "Your DJ name, e.g. Shon"},
        {"requests_cashapp", "cashapp", "Cash App $cashtag"},
        {"requests_venmo", "venmo", "Venmo username"},
        {"requests_instagram", "instagram", "Instagram username"},
        {"requests_tiktok", "tiktok", "TikTok username"},
        {"requests_book", "book", "Booking: email or https:// link"},
};
constexpr int kPollSeconds = 30;

ShonRequests* s_pShonRequests = nullptr;
const QString kDefaultCrate = QStringLiteral("Requestable");
constexpr int kCatalogMs = 60 * 1000;
const QString kDefaultShopUrl = QStringLiteral("https://www.beatport.com/search?q=%1");
constexpr int kPollMs = 3000;
constexpr int kRoleLocation = Qt::UserRole + 1;

const char* const kWindowStyle = R"(
QWidget#ShonRequestsWindow { background: #151517; border-left: 1px solid #2a2a2e; }
QLabel { color: #e6e6e6; }
QLabel#ShonReqTitle { font-size: 18px; font-weight: 600; color: #ffffff; }
QLabel#ShonReqDim { color: #8a8a92; }
QLabel#ShonReqSong { font-size: 14px; font-weight: 600; color: #ffffff; }
QLabel#ShonReqMatch { color: #8a8a92; }
QLabel#ShonReqMatch[fits="true"] { color: #8fb0ff; }
QLabel#ShonReqLink { color: #8fb0ff; }
QLabel#ShonReqTip { color: #00d64f; font-weight: 600; }
QWidget#ShonReqPoll { background: #1d1d22; border: 1px solid #2f6bff; border-radius: 8px; }
QWidget#ShonReqQrCard { background: #ffffff; border-radius: 10px; }
QPushButton { background: #26262b; color: #e6e6e6; border: 1px solid #36363d;
  border-radius: 6px; padding: 5px 12px; }
QPushButton:hover { background: #2e2e34; }
QPushButton#ShonReqOpen[open="true"] { background: #2f6bff; border-color: #2f6bff; color: #ffffff; }
QPushButton#ShonReqComing[active="true"] { background: #2f6bff; border-color: #2f6bff; color: #ffffff; }
QPushButton#ShonReqDismiss { padding: 5px 9px; }
QLineEdit { background: #222226; color: #e6e6e6; border: 1px solid #33333a;
  border-radius: 6px; padding: 5px 8px; }
QListWidget { background: #151517; border: none; outline: none; }
QScrollArea#ShonReqScroll { background: transparent; border: none; }
QWidget#ShonReqContent { background: #151517; }
QListWidget::item { border-bottom: 1px solid #2a2a2e; }
QListWidget::item:selected { background: #1f1f23; }
QScrollBar:vertical { background: transparent; width: 10px; }
QScrollBar::handle:vertical { background: #3a3a40; border-radius: 4px; min-height: 24px; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; }
)";

QString randomString(int length, const QString& alphabet) {
    QString out;
    for (int i = 0; i < length; ++i) {
        out += alphabet.at(QRandomGenerator::system()->bounded(alphabet.size()));
    }
    return out;
}

QStringList searchWords(const QString& song) {
    QString text = song.toLower();
    text.remove(QRegularExpression(QStringLiteral("\\(.*?\\)|\\[.*?\\]")));
    text.replace(QRegularExpression(QStringLiteral("[^\\p{L}\\p{N}]+")), QStringLiteral(" "));
    static const QSet<QString> kSkip = {QStringLiteral("feat"),
            QStringLiteral("ft"),
            QStringLiteral("featuring"),
            QStringLiteral("by"),
            QStringLiteral("the"),
            QStringLiteral("and"),
            QStringLiteral("remix"),
            QStringLiteral("song")};
    QStringList words;
    const QStringList parts = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString& word : parts) {
        if (word.size() >= 2 && !kSkip.contains(word) && !words.contains(word)) {
            words << word;
        }
    }
    return words.mid(0, 6);
}

double bpmDiff(double bpm, double reference) {
    double diff = 1.0;
    for (const double factor : {1.0, 0.5, 2.0}) {
        diff = std::min(diff, std::abs(bpm * factor - reference) / reference);
    }
    return diff;
}

// The request list. Rows with a library match can be dragged onto a deck.
class ShonRequestList : public QListWidget {
  public:
    using QListWidget::QListWidget;

  protected:
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override {
        QList<QUrl> urls;
        for (const QListWidgetItem* pItem : items) {
            const QString location = pItem->data(kRoleLocation).toString();
            if (!location.isEmpty()) {
                urls << QUrl::fromLocalFile(location);
            }
        }
        if (urls.isEmpty()) {
            return nullptr;
        }
        auto* pMime = new QMimeData();
        pMime->setUrls(urls);
        return pMime;
    }
    QStringList mimeTypes() const override {
        return {QStringLiteral("text/uri-list")};
    }
};

// Full screen QR code for guests to scan. Click or press a key to close.
class ShonBigQr : public QWidget {
  public:
    ShonBigQr(const QByteArray& svg, const QString& link) {
        setAttribute(Qt::WA_DeleteOnClose);
        setWindowTitle(QStringLiteral("Request a song"));
        setStyleSheet(QStringLiteral("background: #0f0f11;"));
        auto* pLayout = new QVBoxLayout(this);
        pLayout->setAlignment(Qt::AlignCenter);
        auto* pTitle = new QLabel(QStringLiteral("Scan to request a song"), this);
        pTitle->setAlignment(Qt::AlignCenter);
        pTitle->setStyleSheet(QStringLiteral(
                "color: #ffffff; font-size: 40px; font-weight: 700; padding: 12px;"));
        auto* pFrame = new QWidget(this);
        pFrame->setStyleSheet(QStringLiteral("background: #ffffff; border-radius: 24px;"));
        auto* pFrameLayout = new QVBoxLayout(pFrame);
        pFrameLayout->setContentsMargins(28, 28, 28, 28);
        auto* pQr = new QSvgWidget(pFrame);
        pQr->load(svg);
        const QScreen* pScreen = QApplication::primaryScreen();
        const int side = pScreen ? std::min(pScreen->size().height() * 55 / 100, 720) : 480;
        pQr->setFixedSize(side, side);
        pFrameLayout->addWidget(pQr);
        auto* pLink = new QLabel(link, this);
        pLink->setAlignment(Qt::AlignCenter);
        pLink->setStyleSheet(QStringLiteral("color: #8a8a92; font-size: 20px; padding: 12px;"));
        pLayout->addWidget(pTitle, 0, Qt::AlignHCenter);
        pLayout->addWidget(pFrame, 0, Qt::AlignHCenter);
        pLayout->addWidget(pLink, 0, Qt::AlignHCenter);
    }

  protected:
    void mousePressEvent(QMouseEvent*) override {
        close();
    }
    void keyPressEvent(QKeyEvent*) override {
        close();
    }
};

// The side panel.
class ShonRequestsView : public QWidget {
  public:
    using QWidget::QWidget;
};

} // namespace

ShonRequests::ShonRequests(UserSettingsPointer pConfig,
        Library* pLibrary,
        TrackCollectionManager* pTrackCollectionManager,
        QObject* pParent)
        : QObject(pParent),
          m_pConfig(pConfig),
          m_pLibrary(pLibrary),
          m_pTrackCollectionManager(pTrackCollectionManager),
          m_pNetwork(new QNetworkAccessManager(this)),
          m_pPollTimer(new QTimer(this)) {
    s_pShonRequests = this;
    m_pPollTick = new QTimer(this);
    m_pPollTick->setInterval(1000);
    connect(m_pPollTick, &QTimer::timeout, this, &ShonRequests::updatePollView);
    m_code = m_pConfig->getValueString(kCodeKey);
    m_key = m_pConfig->getValueString(kHostKeyKey);
    if (m_code.isEmpty() || m_key.size() < 16) {
        newParty();
    }

    // [Shon],requests_window shows or hides the side panel (top bar button).
    m_pShowWindow = std::make_unique<ControlPushButton>(
            ConfigKey(kGroup, QStringLiteral("requests_window")));
    m_pShowWindow->setButtonMode(mixxx::control::ButtonMode::Toggle);
    connect(m_pShowWindow.get(), &ControlObject::valueChanged, this, [this](double value) {
        emit panelVisibilityRequested(value > 0.0);
        if (value > 0.0) {
            onViewShown();
        }
    });

    m_pPollTimer->setInterval(kPollMs);
    connect(m_pPollTimer, &QTimer::timeout, this, &ShonRequests::poll);
    m_pCatalogTimer = new QTimer(this);
    m_pCatalogTimer->setInterval(kCatalogMs);
    connect(m_pCatalogTimer, &QTimer::timeout, this, &ShonRequests::uploadCatalog);
    m_pCatalogTimer->start();

    // A requested song that starts playing gets marked Played by itself.
    connect(&PlayerInfo::instance(),
            &PlayerInfo::currentPlayingTrackChanged,
            this,
            [this](TrackPointer pTrack) {
                if (!pTrack || !m_open) {
                    return;
                }
                const QString location = pTrack->getLocation();
                QStringList played;
                for (const Request& request : std::as_const(m_requests)) {
                    if (request.inLibrary && request.location == location) {
                        played << request.id;
                    }
                }
                for (const QString& id : std::as_const(played)) {
                    setStatus(id, QStringLiteral("played"));
                }
            });

    // Requests were on when ShonDJ last closed: turn them back on.
    if (m_pConfig->getValue(kOpenKey, false) && !relayUrl().isEmpty()) {
        QTimer::singleShot(3000, this, [this]() {
            setOpen(true);
        });
    }
}

ShonRequests::~ShonRequests() {
    if (s_pShonRequests == this) {
        s_pShonRequests = nullptr;
    }
}

ShonRequests* ShonRequests::instance() {
    return s_pShonRequests;
}

void ShonRequests::postToRelay(const QString& action,
        QJsonObject body,
        const std::function<void(const QJsonObject&)>& onReply) {
    if (relayUrl().isEmpty()) {
        return;
    }
    QNetworkRequest request(QUrl(relayUrl() + QStringLiteral("/api/p/") + m_code +
            QLatin1Char('/') + action));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    body.insert(QStringLiteral("key"), m_key);
    QNetworkReply* pReply =
            m_pNetwork->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(pReply, &QNetworkReply::finished, this, [pReply, onReply]() {
        pReply->deleteLater();
        const QJsonObject reply = QJsonDocument::fromJson(pReply->readAll()).object();
        if (onReply) {
            QJsonObject result = reply;
            if (pReply->error() != QNetworkReply::NoError && !result.contains(QStringLiteral("error"))) {
                result.insert(QStringLiteral("error"), pReply->errorString());
            }
            onReply(result);
        }
    });
}

void ShonRequests::uploadProfile() {
    QJsonObject profile;
    for (const ProfileField& f : kProfileFields) {
        profile.insert(QLatin1String(f.field),
                m_pConfig->getValueString(ConfigKey(kGroup, QLatin1String(f.item))).trimmed());
    }
    QJsonObject body;
    body.insert(QStringLiteral("profile"), profile);
    postToRelay(QStringLiteral("profile"), body);
}

void ShonRequests::startPoll(const QString& titleA,
        const QString& artistA,
        const QString& titleB,
        const QString& artistB) {
    if (!m_open) {
        emit notice(QStringLiteral("Crowd poll: turn requests on first"));
        return;
    }
    QJsonObject a;
    a.insert(QStringLiteral("title"), titleA);
    a.insert(QStringLiteral("artist"), artistA);
    QJsonObject b;
    b.insert(QStringLiteral("title"), titleB);
    b.insert(QStringLiteral("artist"), artistB);
    QJsonObject body;
    body.insert(QStringLiteral("a"), a);
    body.insert(QStringLiteral("b"), b);
    body.insert(QStringLiteral("seconds"), kPollSeconds);
    postToRelay(QStringLiteral("poll"), body, [this](const QJsonObject& reply) {
        if (reply.contains(QStringLiteral("error"))) {
            emit notice(QStringLiteral("Crowd poll: %1")
                                .arg(reply.value(QStringLiteral("error")).toString()));
            return;
        }
        handlePoll(reply.value(QStringLiteral("poll")).toObject());
        emit notice(QStringLiteral("Crowd poll: guests are voting for %1 seconds")
                            .arg(kPollSeconds));
        if (m_pShowWindow && m_pShowWindow->get() <= 0.0) {
            m_pShowWindow->set(1.0);
            emit panelVisibilityRequested(true);
            onViewShown();
        }
    });
}

void ShonRequests::handlePoll(const QJsonObject& poll) {
    const QString oldId = m_poll.value(QStringLiteral("id")).toString();
    const bool wasOpen = QDateTime::currentMSecsSinceEpoch() < m_pollEndsAt;
    m_poll = poll;
    if (poll.isEmpty()) {
        m_pollEndsAt = 0;
        m_pPollTick->stop();
    } else {
        m_pollEndsAt = QDateTime::currentMSecsSinceEpoch() +
                static_cast<qint64>(poll.value(QStringLiteral("left")).toDouble());
        if (!m_pPollTick->isActive()) {
            m_pPollTick->start();
        }
        // Tell the DJ who won when the vote closes.
        const bool nowOpen = QDateTime::currentMSecsSinceEpoch() < m_pollEndsAt;
        if (poll.value(QStringLiteral("id")).toString() == oldId && wasOpen && !nowOpen) {
            const QJsonArray votes = poll.value(QStringLiteral("votes")).toArray();
            const int va = votes.at(0).toInt();
            const int vb = votes.at(1).toInt();
            const auto title = [&poll](const char* side) {
                return poll.value(QLatin1String(side)).toObject().value(QStringLiteral("title")).toString();
            };
            if (va + vb == 0) {
                emit notice(QStringLiteral("Crowd poll: no votes"));
            } else if (va == vb) {
                emit notice(QStringLiteral("Crowd poll: tie, %1 each").arg(va));
            } else {
                emit notice(QStringLiteral("Crowd poll: %1 wins, %2 to %3")
                                    .arg(va > vb ? title("a") : title("b"))
                                    .arg(std::max(va, vb))
                                    .arg(std::min(va, vb)));
            }
        }
    }
    updatePollView();
}

void ShonRequests::updatePollView() {
    if (!m_pPollBox) {
        return;
    }
    if (m_poll.isEmpty()) {
        m_pPollBox->hide();
        m_pPollTick->stop();
        if (m_pPollTop) {
            m_pPollTop->setEnabled(m_open && m_requests.size() >= 2);
        }
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool open = now < m_pollEndsAt;
    const QJsonArray votes = m_poll.value(QStringLiteral("votes")).toArray();
    const int va = votes.at(0).toInt();
    const int vb = votes.at(1).toInt();
    const int total = va + vb;
    const auto line = [this, total](const char* side, int count) {
        const QJsonObject song = m_poll.value(QLatin1String(side)).toObject();
        QString name = song.value(QStringLiteral("title")).toString();
        const QString artist = song.value(QStringLiteral("artist")).toString();
        if (!artist.isEmpty()) {
            name += QStringLiteral(", ") + artist;
        }
        const int pct = total ? qRound(count * 100.0 / total) : 0;
        return QStringLiteral("<b>%1%</b> &nbsp;%2 <span style='color:#8a8a92'>(%3)</span>")
                .arg(pct)
                .arg(name.toHtmlEscaped())
                .arg(count);
    };
    const QString head = open
            ? QStringLiteral("Crowd poll, %1s left")
                      .arg(std::max<qint64>(0, (m_pollEndsAt - now + 999) / 1000))
            : QStringLiteral("Crowd poll closed, %1 votes. Your call.").arg(total);
    m_pPollLabel->setText(QStringLiteral("<span style='color:#8fb0ff'>%1</span><br>%2<br>%3")
                                  .arg(head, line("a", va), line("b", vb)));
    m_pPollEnd->setText(open ? QStringLiteral("End vote") : QStringLiteral("Clear"));
    m_pPollBox->show();
    if (m_pPollTop) {
        m_pPollTop->setEnabled(m_open && m_requests.size() >= 2);
    }
}

QString ShonRequests::relayUrl() const {
    QString url = m_pConfig->getValueString(kRelayUrlKey).trimmed();
    while (url.endsWith(QLatin1Char('/'))) {
        url.chop(1);
    }
    return url;
}

QString ShonRequests::handle() const {
    const QString name = m_pConfig->getValueString(kHandleKey).trimmed().toLower();
    static const QRegularExpression kValid(QStringLiteral("^[a-z0-9-]{2,20}$"));
    return kValid.match(name).hasMatch() ? name : QString();
}

QString ShonRequests::guestUrl() const {
    // A permanent link never changes, so printed QR codes keep working.
    if (!handle().isEmpty()) {
        return relayUrl() + QStringLiteral("/dj/") + handle();
    }
    return relayUrl() + QStringLiteral("/p/") + m_code;
}

QString ShonRequests::screenUrl() const {
    if (!handle().isEmpty()) {
        return relayUrl() + QStringLiteral("/show/dj/") + handle();
    }
    return relayUrl() + QStringLiteral("/show/") + m_code;
}

void ShonRequests::claimHandle() {
    // Points the permanent link at the current party code.
    if (handle().isEmpty() || relayUrl().isEmpty()) {
        return;
    }
    QString secret = m_pConfig->getValueString(kHandleSecretKey);
    if (secret.size() < 16) {
        secret = randomString(32,
                QStringLiteral("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"));
        m_pConfig->setValue(kHandleSecretKey, secret);
    }
    QNetworkRequest request(QUrl(relayUrl() + QStringLiteral("/api/handle/") + handle()));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QJsonObject body;
    body.insert(QStringLiteral("secret"), secret);
    body.insert(QStringLiteral("code"), m_code);
    QNetworkReply* pReply =
            m_pNetwork->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(pReply, &QNetworkReply::finished, this, [this, pReply]() {
        pReply->deleteLater();
        if (pReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 403) {
            emit notice(QStringLiteral(
                    "Requests: that link name is taken, pick another one"));
        }
    });
}

void ShonRequests::copyLink() {
    QApplication::clipboard()->setText(guestUrl());
    emit notice(QStringLiteral("Requests: link copied"));
}

void ShonRequests::saveQrImage() {
    if (m_qrSvg.isEmpty()) {
        loadQr();
        return;
    }
    const QString path = QFileDialog::getSaveFileName(m_pWindow,
            QStringLiteral("Save request QR"),
            QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) +
                    QStringLiteral("/ShonDJ requests.png"),
            QStringLiteral("PNG image (*.png)"));
    if (path.isEmpty()) {
        return;
    }
    // A phone-story sized poster: logo, title, QR on a white card, link.
    QImage poster(1080, 1350, QImage::Format_ARGB32_Premultiplied);
    poster.fill(QColor(15, 15, 17));
    QPainter p(&poster);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const QPixmap logo(QStringLiteral(":/images/shondj-logo.png"));
    if (!logo.isNull()) {
        const QPixmap scaled = logo.scaledToHeight(110, Qt::SmoothTransformation);
        p.drawPixmap((poster.width() - scaled.width()) / 2, 90, scaled);
    }
    QFont titleFont = p.font();
    titleFont.setPixelSize(64);
    titleFont.setBold(true);
    p.setFont(titleFont);
    p.setPen(Qt::white);
    p.drawText(QRect(0, 250, poster.width(), 90), Qt::AlignCenter,
            QStringLiteral("Scan to request a song"));
    const QRect card(190, 380, 700, 700);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::white);
    p.drawRoundedRect(card, 40, 40);
    QSvgRenderer renderer(m_qrSvg);
    renderer.render(&p, QRectF(card.adjusted(40, 40, -40, -40)));
    QFont linkFont = p.font();
    linkFont.setPixelSize(34);
    linkFont.setBold(false);
    p.setFont(linkFont);
    p.setPen(QColor(138, 138, 146));
    QString link = guestUrl();
    link.remove(QRegularExpression(QStringLiteral("^https?://")));
    p.drawText(QRect(40, 1130, poster.width() - 80, 80), Qt::AlignCenter, link);
    p.end();
    if (poster.save(path, "PNG")) {
        emit notice(QStringLiteral("Requests: QR image saved"));
    }
}

void ShonRequests::newParty() {
    // No 0/O or 1/I so the code is easy to read out loud.
    m_code = randomString(5, QStringLiteral("ABCDEFGHJKLMNPQRSTUVWXYZ23456789"));
    m_key = randomString(32,
            QStringLiteral("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"));
    m_pConfig->setValue(kCodeKey, m_code);
    m_pConfig->setValue(kHostKeyKey, m_key);
    m_requests.clear();
    m_seenIds.clear();
    m_matchCache.clear();
    m_lastSignature.clear();
    m_qrSvg.clear();
    if (m_open) {
        m_open = false;
        setOpen(true);
    }
    claimHandle();
    if (m_pWindow) {
        loadQr();
        rebuildList();
        updateHeader();
    }
}

void ShonRequests::onViewShown() {
    if (m_qrSvg.isEmpty()) {
        loadQr();
    }
    updateHeader();
}

QWidget* ShonRequests::createView(QWidget* pParent) {
    auto* pWindow = new ShonRequestsView(pParent);
    pWindow->setObjectName(QStringLiteral("ShonRequestsWindow"));
    pWindow->setAttribute(Qt::WA_StyledBackground, true);
    pWindow->setStyleSheet(QString::fromUtf8(kWindowStyle));
    m_pWindow = pWindow;

    // Everything scrolls together, so the inbox never gets squeezed to nothing
    // when the links or a poll are showing on a short screen.
    auto* pOuter = new QVBoxLayout(pWindow);
    pOuter->setContentsMargins(0, 0, 0, 0);
    auto* pScroll = new QScrollArea(pWindow);
    pScroll->setObjectName(QStringLiteral("ShonReqScroll"));
    pScroll->setWidgetResizable(true);
    pScroll->setFrameShape(QFrame::NoFrame);
    pScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* pContent = new QWidget(pScroll);
    pContent->setObjectName(QStringLiteral("ShonReqContent"));
    pScroll->setWidget(pContent);
    pOuter->addWidget(pScroll);
    auto* pLayout = new QVBoxLayout(pContent);
    pLayout->setContentsMargins(16, 10, 16, 16);
    pLayout->setSpacing(12);

    // Title row with a close button.
    auto* pTitleRow = new QHBoxLayout();
    auto* pTitle = new QLabel(QStringLiteral("Song requests"), pWindow);
    pTitle->setObjectName(QStringLiteral("ShonReqTitle"));
    auto* pClose = new QPushButton(QStringLiteral("X"), pWindow);
    pClose->setObjectName(QStringLiteral("ShonReqDismiss"));
    pClose->setToolTip(QStringLiteral("Hide the panel. Requests keep coming in."));
    connect(pClose, &QPushButton::clicked, this, [this]() {
        // set() from code does not notify our own listener, so hide directly.
        m_pShowWindow->set(0.0);
        emit panelVisibilityRequested(false);
    });
    pTitleRow->addWidget(pTitle, 1);
    pTitleRow->addWidget(pClose);
    pLayout->addLayout(pTitleRow);

    m_pStatusLabel = new QLabel(pWindow);
    m_pStatusLabel->setObjectName(QStringLiteral("ShonReqDim"));
    m_pStatusLabel->setWordWrap(true);
    pLayout->addWidget(m_pStatusLabel);

    // On/off and new party.
    auto* pButtons = new QHBoxLayout();
    m_pOpenButton = new QPushButton(pWindow);
    m_pOpenButton->setObjectName(QStringLiteral("ShonReqOpen"));
    connect(m_pOpenButton, &QPushButton::clicked, this, [this]() {
        setOpen(!m_open);
    });
    auto* pNewParty = new QPushButton(QStringLiteral("New party"), pWindow);
    pNewParty->setToolTip(QStringLiteral(
            "New code and QR. Old links stop working and the inbox is cleared."));
    connect(pNewParty, &QPushButton::clicked, this, &ShonRequests::newParty);
    pButtons->addWidget(m_pOpenButton, 1);
    pButtons->addWidget(pNewParty, 1);
    pLayout->addLayout(pButtons);

    // One-time setup: where the relay lives.
    m_pSetupBox = new QWidget(pWindow);
    auto* pSetupRows = new QVBoxLayout(m_pSetupBox);
    pSetupRows->setContentsMargins(0, 0, 0, 0);
    auto* pSetup = new QHBoxLayout();
    m_pRelayEdit = new QLineEdit(relayUrl(), m_pSetupBox);
    m_pRelayEdit->setPlaceholderText(QStringLiteral("Relay link (https://...workers.dev)"));
    auto* pSave = new QPushButton(QStringLiteral("Save"), m_pSetupBox);
    connect(pSave, &QPushButton::clicked, this, [this]() {
        m_pConfig->setValue(kRelayUrlKey, m_pRelayEdit->text().trimmed());
        m_qrSvg.clear();
        loadQr();
        updateHeader();
    });
    pSetup->addWidget(m_pRelayEdit, 1);
    pSetup->addWidget(pSave);
    pSetupRows->addLayout(pSetup);
    // Permanent link name, e.g. "shon" gives .../dj/shon for printed QR codes.
    auto* pHandleRow = new QHBoxLayout();
    m_pHandleEdit = new QLineEdit(handle(), m_pSetupBox);
    m_pHandleEdit->setPlaceholderText(QStringLiteral("Permanent link name, e.g. shon"));
    auto* pSaveHandle = new QPushButton(QStringLiteral("Save"), m_pSetupBox);
    connect(pSaveHandle, &QPushButton::clicked, this, [this]() {
        m_pConfig->setValue(kHandleKey, m_pHandleEdit->text().trimmed().toLower());
        m_qrSvg.clear();
        claimHandle();
        loadQr();
        updateHeader();
    });
    pHandleRow->addWidget(m_pHandleEdit, 1);
    pHandleRow->addWidget(pSaveHandle);
    pSetupRows->addLayout(pHandleRow);
    pLayout->addWidget(m_pSetupBox);

    // QR code, then the link and the full screen button under it.
    // White rounded card so the code reads cleanly on the dark panel.
    auto* pQrCard = new QWidget(pWindow);
    pQrCard->setObjectName(QStringLiteral("ShonReqQrCard"));
    pQrCard->setAttribute(Qt::WA_StyledBackground, true);
    auto* pQrCardLayout = new QVBoxLayout(pQrCard);
    pQrCardLayout->setContentsMargins(10, 10, 10, 10);
    m_pQr = new QSvgWidget(pQrCard);
    m_pQr->setFixedSize(170, 170);
    pQrCardLayout->addWidget(m_pQr);
    pLayout->addSpacing(4);
    pLayout->addWidget(pQrCard, 0, Qt::AlignHCenter);
    auto* pScan = new QLabel(QStringLiteral("Guests scan this to request a song."), pWindow);
    pScan->setAlignment(Qt::AlignHCenter);
    pScan->setWordWrap(true);
    pLayout->addWidget(pScan);
    m_pLinkLabel = new QLabel(pWindow);
    m_pLinkLabel->setObjectName(QStringLiteral("ShonReqLink"));
    m_pLinkLabel->setAlignment(Qt::AlignHCenter);
    m_pLinkLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // A long link must never make the panel wider.
    m_pLinkLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    pLayout->addWidget(m_pLinkLabel);
    // Ways to get the QR out to people.
    auto* pShareRow = new QHBoxLayout();
    auto* pCopy = new QPushButton(QStringLiteral("Copy link"), pWindow);
    pCopy->setToolTip(QStringLiteral("Paste it in a group chat or an Instagram story link"));
    connect(pCopy, &QPushButton::clicked, this, &ShonRequests::copyLink);
    auto* pSaveQr = new QPushButton(QStringLiteral("Save image"), pWindow);
    pSaveQr->setToolTip(QStringLiteral("A poster to print, post or AirDrop"));
    connect(pSaveQr, &QPushButton::clicked, this, &ShonRequests::saveQrImage);
    auto* pBig = new QPushButton(QStringLiteral("Full screen"), pWindow);
    connect(pBig, &QPushButton::clicked, this, &ShonRequests::showBigQr);
    pShareRow->addWidget(pCopy, 1);
    pShareRow->addWidget(pSaveQr, 1);
    pShareRow->addWidget(pBig, 1);
    pLayout->addLayout(pShareRow);
    m_pScreenLabel = new QLabel(pWindow);
    m_pScreenLabel->setObjectName(QStringLiteral("ShonReqDim"));
    m_pScreenLabel->setWordWrap(true);
    m_pScreenLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pLayout->addWidget(m_pScreenLabel);
    // Tip, follow and booking links, folded away once filled in.
    auto* pLinksToggle = new QPushButton(QStringLiteral("Tip, follow and booking links"), pWindow);
    pLinksToggle->setToolTip(QStringLiteral("Shown to guests on the request page"));
    pLayout->addWidget(pLinksToggle);
    m_pLinksBox = new QWidget(pWindow);
    auto* pLinksRows = new QVBoxLayout(m_pLinksBox);
    pLinksRows->setContentsMargins(0, 0, 0, 0);
    pLinksRows->setSpacing(6);
    bool anyLink = false;
    for (const ProfileField& f : kProfileFields) {
        auto* pEdit = new QLineEdit(
                m_pConfig->getValueString(ConfigKey(kGroup, QLatin1String(f.item))), m_pLinksBox);
        pEdit->setPlaceholderText(QLatin1String(f.hint));
        anyLink = anyLink || !pEdit->text().isEmpty();
        m_profileEdits.insert(QLatin1String(f.item), pEdit);
        pLinksRows->addWidget(pEdit);
    }
    auto* pSaveLinks = new QPushButton(QStringLiteral("Save links"), m_pLinksBox);
    connect(pSaveLinks, &QPushButton::clicked, this, [this]() {
        for (auto it = m_profileEdits.constBegin(); it != m_profileEdits.constEnd(); ++it) {
            m_pConfig->setValue(ConfigKey(kGroup, it.key()), it.value()->text().trimmed());
        }
        uploadProfile();
        m_pLinksBox->hide();
        emit notice(QStringLiteral("Requests: links saved"));
    });
    pLinksRows->addWidget(pSaveLinks);
    m_pLinksBox->setVisible(!anyLink);
    pLayout->addWidget(m_pLinksBox);
    connect(pLinksToggle, &QPushButton::clicked, this, [this]() {
        m_pLinksBox->setVisible(!m_pLinksBox->isVisible());
    });

    // Crowd poll: the top two requests, or two songs from the library
    // (select two, right-click, Crowd poll).
    m_pPollTop = new QPushButton(QStringLiteral("Crowd poll: top 2 requests"), pWindow);
    m_pPollTop->setToolTip(QStringLiteral(
            "Guests vote between the two most wanted requests for 30 seconds. "
            "For any two songs: select them in the library, right-click, Crowd poll."));
    connect(m_pPollTop, &QPushButton::clicked, this, [this]() {
        if (m_requests.size() < 2) {
            return;
        }
        const auto split = [](const QString& song, QString* pTitle, QString* pArtist) {
            const int comma = song.lastIndexOf(QStringLiteral(", "));
            *pTitle = comma > 0 ? song.left(comma) : song;
            *pArtist = comma > 0 ? song.mid(comma + 2) : QString();
        };
        QString ta, aa, tb, ab;
        split(m_requests.at(0).song, &ta, &aa);
        split(m_requests.at(1).song, &tb, &ab);
        startPoll(ta, aa, tb, ab);
    });
    pLayout->addWidget(m_pPollTop);
    m_pPollBox = new QWidget(pWindow);
    m_pPollBox->setObjectName(QStringLiteral("ShonReqPoll"));
    m_pPollBox->setAttribute(Qt::WA_StyledBackground, true);
    auto* pPollLayout = new QHBoxLayout(m_pPollBox);
    pPollLayout->setContentsMargins(10, 8, 10, 8);
    m_pPollLabel = new QLabel(m_pPollBox);
    m_pPollLabel->setTextFormat(Qt::RichText);
    m_pPollLabel->setWordWrap(true);
    m_pPollEnd = new QPushButton(QStringLiteral("End vote"), m_pPollBox);
    connect(m_pPollEnd, &QPushButton::clicked, this, [this]() {
        if (QDateTime::currentMSecsSinceEpoch() < m_pollEndsAt) {
            postToRelay(QStringLiteral("poll_end"), QJsonObject(), [this](const QJsonObject& reply) {
                handlePoll(reply.value(QStringLiteral("poll")).toObject());
            });
        } else {
            // Hide it here; the guest page drops it a minute after the vote.
            m_poll = QJsonObject();
            m_pollEndsAt = 0;
            updatePollView();
        }
    });
    pPollLayout->addWidget(m_pPollLabel, 1);
    pPollLayout->addWidget(m_pPollEnd, 0, Qt::AlignTop);
    m_pPollBox->hide();
    pLayout->addWidget(m_pPollBox);

    auto* pHint = new QLabel(
            QStringLiteral("Drag a request onto a deck to load it. Double-click to search "
                           "your library for it."),
            pWindow);
    pHint->setObjectName(QStringLiteral("ShonReqDim"));
    pHint->setWordWrap(true);
    pLayout->addSpacing(4);
    pLayout->addWidget(pHint);

    // The inbox.
    m_pList = new ShonRequestList(pWindow);
    m_pList->setDragEnabled(true);
    m_pList->setDragDropMode(QAbstractItemView::DragOnly);
    m_pList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_pList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    connect(m_pList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* pItem) {
        // Search with plain words: phones type curly quotes (Can’t) and
        // extra bits like "feat." that would make the search find nothing.
        const QString words =
                searchWords(pItem->data(Qt::UserRole).toString()).join(QLatin1Char(' '));
        if (m_pLibrary && !words.isEmpty()) {
            m_pLibrary->searchTracksInCollection(words);
        }
    });
    m_pList->setMinimumHeight(320);
    pLayout->addWidget(m_pList, 1);

    connect(pWindow, &QObject::destroyed, this, [this]() {
        m_pList = nullptr;
        m_pPollBox = nullptr;
        m_pPollTop = nullptr;
        m_pLinksBox = nullptr;
        m_profileEdits.clear();
    });

    updateHeader();
    rebuildList();
    return pWindow;
}

void ShonRequests::updateHeader() {
    if (!m_pWindow) {
        return;
    }
    const bool configured = !relayUrl().isEmpty();
    m_pSetupBox->setVisible(!configured || handle().isEmpty() || m_pRelayEdit->hasFocus() ||
            m_pHandleEdit->hasFocus());
    QString screen = screenUrl();
    screen.remove(QRegularExpression(QStringLiteral("^https?://")));
    m_pScreenLabel->setText(configured
                    ? QStringLiteral("On a TV, tablet or another laptop, open %1").arg(screen)
                    : QString());
    m_pLinkLabel->setText(configured ? guestUrl()
                                     : QStringLiteral("Add your relay link above first."));
    m_pOpenButton->setText(m_open ? QStringLiteral("Requests ON") : QStringLiteral("Requests OFF"));
    m_pOpenButton->setProperty("open", m_open);
    m_pOpenButton->style()->unpolish(m_pOpenButton);
    m_pOpenButton->style()->polish(m_pOpenButton);
    m_pOpenButton->setEnabled(configured);
    QString status;
    if (!configured) {
        status = QStringLiteral("not set up");
    } else if (!m_open) {
        status = QStringLiteral("code %1, closed").arg(m_code);
    } else if (!m_online) {
        status = QStringLiteral("code %1, offline, retrying").arg(m_code);
    } else {
        status = QStringLiteral("code %1, %2 waiting, %3 songs in your Requestable crate")
                         .arg(m_code)
                         .arg(m_requests.size())
                         .arg(m_catalogCount);
    }
    m_pStatusLabel->setText(status);
}

void ShonRequests::loadQr() {
    if (relayUrl().isEmpty() || !m_pWindow) {
        return;
    }
    QString qrUrl;
    if (handle().isEmpty()) {
        qrUrl = relayUrl() + QStringLiteral("/qr/") + m_code + QStringLiteral(".svg");
    } else {
        qrUrl = relayUrl() + QStringLiteral("/qr/dj/") + handle() + QStringLiteral(".svg");
    }
    QNetworkReply* pReply = m_pNetwork->get(QNetworkRequest(QUrl(qrUrl)));
    connect(pReply, &QNetworkReply::finished, this, [this, pReply]() {
        pReply->deleteLater();
        if (pReply->error() != QNetworkReply::NoError) {
            return;
        }
        m_qrSvg = pReply->readAll();
        if (m_pQr) {
            m_pQr->load(m_qrSvg);
        }
    });
}

void ShonRequests::showBigQr() {
    if (m_qrSvg.isEmpty()) {
        loadQr();
        return;
    }
    auto* pBig = new ShonBigQr(m_qrSvg, guestUrl());
    pBig->showFullScreen();
}

void ShonRequests::setOpen(bool open) {
    if (relayUrl().isEmpty()) {
        return;
    }
    QNetworkRequest request(QUrl(relayUrl() + QStringLiteral("/api/p/") + m_code +
            (open ? QStringLiteral("/open") : QStringLiteral("/close"))));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QJsonObject body;
    body.insert(QStringLiteral("key"), m_key);
    QNetworkReply* pReply =
            m_pNetwork->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(pReply, &QNetworkReply::finished, this, [this, pReply, open]() {
        pReply->deleteLater();
        const int code = pReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (pReply->error() == QNetworkReply::NoError) {
            m_open = open;
            m_online = true;
            m_pConfig->setValue(kOpenKey, open ? 1 : 0);
            if (open) {
                m_primed = false;
                m_catalogSignature.clear();
                uploadCatalog();
                uploadProfile();
                claimHandle();
                m_pPollTimer->start();
                poll();
                emit notice(QStringLiteral("Requests: open (code %1)").arg(m_code));
            } else {
                m_pPollTimer->stop();
            }
        } else if (code == 403) {
            // Someone else holds this code. Start a fresh party.
            newParty();
        } else {
            m_online = false;
            emit notice(QStringLiteral("Requests: could not reach the relay (check the link)"));
        }
        updateHeader();
    });
}

void ShonRequests::poll() {
    if (!m_open || relayUrl().isEmpty()) {
        return;
    }
    QUrl url(relayUrl() + QStringLiteral("/api/p/") + m_code + QStringLiteral("/requests"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("key"), m_key);
    url.setQuery(query);
    QNetworkReply* pReply = m_pNetwork->get(QNetworkRequest(url));
    connect(pReply, &QNetworkReply::finished, this, [this, pReply]() {
        pReply->deleteLater();
        const bool ok = pReply->error() == QNetworkReply::NoError;
        if (ok != m_online) {
            m_online = ok;
            updateHeader();
        }
        if (ok) {
            handleRequests(pReply->readAll());
        }
    });
}

void ShonRequests::handleRequests(const QByteArray& data) {
    const QJsonObject root = QJsonDocument::fromJson(data).object();
    const QJsonArray items = root.value(QStringLiteral("requests")).toArray();

    QList<Request> requests;
    QString signature;
    for (const QJsonValue& value : items) {
        const QJsonObject item = value.toObject();
        Request request;
        request.id = item.value(QStringLiteral("id")).toString();
        request.song = item.value(QStringLiteral("song")).toString();
        // The relay looks up the real title and artist, use those when it found them.
        const QString title = item.value(QStringLiteral("title")).toString();
        const QString artist = item.value(QStringLiteral("artist")).toString();
        if (!title.isEmpty()) {
            request.song = title;
            if (!artist.isEmpty()) {
                request.song += QStringLiteral(", ") + artist;
            }
        }
        request.votes = item.value(QStringLiteral("votes")).toInt(1);
        request.status = item.value(QStringLiteral("status")).toString();
        request.tipped = item.value(QStringLiteral("tipped")).toInt() != 0 ||
                item.value(QStringLiteral("tipped")).toBool();
        for (const QJsonValue& name : item.value(QStringLiteral("names")).toArray()) {
            request.names << name.toString();
        }
        matchInLibrary(&request);
        signature += request.id + request.status + QString::number(request.votes) +
                (request.tipped ? QStringLiteral("$") : QString()) +
                request.playedNote + (request.fits ? QStringLiteral("f") : QString()) +
                QLatin1Char(';');
        if (!m_seenIds.contains(request.id)) {
            m_seenIds.insert(request.id);
            // The first load after opening only fills the list, no pop-ups.
            if (m_primed) {
                emit notice(QStringLiteral("Request: %1").arg(request.song));
            }
        }
        requests << request;
    }
    m_requests = requests;
    m_primed = true;
    {
        // Same poll id: keep it unless the relay dropped it (a minute after it ended).
        const QJsonObject poll = root.value(QStringLiteral("poll")).toObject();
        if (!poll.isEmpty() || !m_poll.isEmpty()) {
            handlePoll(poll);
        }
    }
    if (signature != m_lastSignature) {
        m_lastSignature = signature;
        rebuildList();
    }
    updateHeader();
}

void ShonRequests::matchInLibrary(Request* pRequest) {
    // Library lookups are cached per request, the fit check runs every time
    // because the playing track changes.
    if (m_matchCache.contains(pRequest->id)) {
        const Request& cached = m_matchCache.value(pRequest->id);
        pRequest->inLibrary = cached.inLibrary;
        pRequest->location = cached.location;
        pRequest->matchText = cached.matchText;
        pRequest->playedNote = cached.playedNote;
        pRequest->bpm = cached.bpm;
        pRequest->key = cached.key;
    } else if (m_pTrackCollectionManager) {
        const QStringList words = searchWords(pRequest->song);
        // "Title - Artist" or "Title by Artist": also try the title alone,
        // since artist names in the library are often messy (VEVO, Topic).
        QStringList titleWords;
        {
            const QStringList halves = pRequest->song.split(
                    QRegularExpression(QStringLiteral("\\s[-\\x{2013}\\x{2014}]\\s|\\sby\\s"),
                            QRegularExpression::CaseInsensitiveOption));
            if (halves.size() >= 2) {
                titleWords = searchWords(halves.first());
            }
        }
        QSqlDatabase database = m_pTrackCollectionManager->internalCollection()->database();
        for (int drop = 0; drop <= 2 && !pRequest->inLibrary && !words.isEmpty(); ++drop) {
            QStringList use;
            if (drop == 0) {
                use = words;
            } else if (drop == 1) {
                use = words.size() >= 3 ? words.mid(0, words.size() - 1) : QStringList();
            } else {
                use = titleWords;
            }
            if (use.isEmpty()) {
                continue;
            }
            QStringList conditions;
            for (int i = 0; i < use.size(); ++i) {
                conditions << QStringLiteral(
                        "lower(coalesce(library.artist, '') || ' ' || "
                        "coalesce(library.title, '')) LIKE ?");
            }
            QSqlQuery query(database);
            query.prepare(QStringLiteral(
                    "SELECT library.artist, library.title, library.bpm, library.key_id, "
                    "library.last_played_at, track_locations.location "
                    "FROM library JOIN track_locations ON library.location = "
                    "track_locations.id "
                    "WHERE library.mixxx_deleted = 0 AND ") +
                    conditions.join(QStringLiteral(" AND ")) +
                    QStringLiteral(" ORDER BY library.timesplayed DESC LIMIT 1"));
            for (const QString& word : use) {
                const QString likePattern = QStringLiteral("%") + word + QStringLiteral("%");
                query.addBindValue(likePattern);
            }
            if (query.exec() && query.next()) {
                pRequest->inLibrary = true;
                const double bpm = query.value(2).toDouble();
                const auto key = static_cast<mixxx::track::io::key::ChromaticKey>(
                        query.value(3).toInt());
                pRequest->location = query.value(5).toString();
                QString text = QStringLiteral("In library: %1, %2")
                                       .arg(query.value(1).toString(),
                                               query.value(0).toString());
                if (bpm > 0) {
                    text += QStringLiteral("  |  %1 BPM").arg(QString::number(bpm, 'f', 0));
                }
                if (key != mixxx::track::io::key::INVALID) {
                    text += QStringLiteral("  |  ") + KeyUtils::keyToString(key);
                }
                pRequest->matchText = text;
                const QDateTime lastPlayed =
                        QDateTime::fromString(query.value(4).toString(), Qt::ISODate);
                if (lastPlayed.isValid()) {
                    const qint64 minutes = lastPlayed.secsTo(QDateTime::currentDateTimeUtc()) / 60;
                    if (minutes >= 0 && minutes < 120) {
                        pRequest->playedNote = QStringLiteral("played %1 min ago").arg(minutes);
                    }
                }
                // Keep BPM and key for the fit check.
                pRequest->bpm = bpm;
                pRequest->key = static_cast<int>(key);
                m_matchCache.insert(pRequest->id, *pRequest);
            }
        }
        // Misses are not cached: a song added later (ADD, Inbox, a drag in)
        // shows up as "in library" on the next check.
    }

    // Does it mix with what is playing right now?
    pRequest->fits = false;
    if (pRequest->inLibrary) {
        const double bpm = pRequest->bpm;
        const auto key = static_cast<mixxx::track::io::key::ChromaticKey>(pRequest->key);
        TrackPointer pPlaying = PlayerInfo::instance().getCurrentPlayingTrack();
        if (pPlaying && pPlaying->getBpm() > 0 && bpm > 0 &&
                bpmDiff(bpm, pPlaying->getBpm()) <= 0.06) {
            const auto playingKey = pPlaying->getKey();
            pRequest->fits = playingKey == mixxx::track::io::key::INVALID ||
                    key == mixxx::track::io::key::INVALID ||
                    KeyUtils::getCompatibleKeys(playingKey).contains(key);
        }
    }
}

void ShonRequests::rebuildList() {
    if (!m_pList) {
        return;
    }
    const int scroll = m_pList->verticalScrollBar() ? m_pList->verticalScrollBar()->value() : 0;
    m_pList->clear();
    if (m_requests.isEmpty()) {
        auto* pItem = new QListWidgetItem(m_pList);
        pItem->setFlags(Qt::NoItemFlags);
        auto* pEmpty = new QLabel(m_open
                        ? QStringLiteral("No requests yet.")
                        : QStringLiteral("Turn requests on, then show guests the QR code."),
                m_pList);
        pEmpty->setObjectName(QStringLiteral("ShonReqDim"));
        pEmpty->setContentsMargins(8, 16, 8, 16);
        pItem->setSizeHint(pEmpty->sizeHint());
        m_pList->setItemWidget(pItem, pEmpty);
        return;
    }
    for (const Request& request : std::as_const(m_requests)) {
        auto* pItem = new QListWidgetItem(m_pList);
        pItem->setData(Qt::UserRole, request.song);
        pItem->setData(kRoleLocation, request.location);
        Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        if (request.inLibrary) {
            flags |= Qt::ItemIsDragEnabled;
        }
        pItem->setFlags(flags);

        auto* pRow = new QWidget(m_pList);
        auto* pText = new QVBoxLayout(pRow);
        pText->setContentsMargins(8, 8, 8, 8);
        pText->setSpacing(2);
        auto* pRowLayout = new QHBoxLayout();
        pRowLayout->setContentsMargins(0, 6, 0, 0);
        auto* pSong = new QLabel(request.song, pRow);
        pSong->setObjectName(QStringLiteral("ShonReqSong"));
        pSong->setWordWrap(true);
        if (request.tipped) {
            auto* pTip = new QLabel(QStringLiteral("$ TIPPED"), pRow);
            pTip->setObjectName(QStringLiteral("ShonReqTip"));
            pTip->setToolTip(QStringLiteral(
                    "The guest says they tipped. Check your Cash App or Venmo."));
            pText->addWidget(pTip);
        }
        QStringList who;
        if (!request.names.isEmpty()) {
            who << request.names.join(QStringLiteral(", "));
        }
        if (request.votes > 1) {
            who << QStringLiteral("%1 people asked").arg(request.votes);
        }
        if (!request.playedNote.isEmpty()) {
            who << request.playedNote;
        }
        auto* pWho = new QLabel(who.join(QStringLiteral("  |  ")), pRow);
        pWho->setObjectName(QStringLiteral("ShonReqDim"));
        pWho->setVisible(!who.isEmpty());
        auto* pMatch = new QLabel(
                request.inLibrary ? request.matchText : QStringLiteral("Not in library"), pRow);
        pMatch->setObjectName(QStringLiteral("ShonReqMatch"));
        pMatch->setProperty("fits", request.fits);
        pMatch->setWordWrap(true);
        pWho->setWordWrap(true);
        pText->addWidget(pSong);
        pText->addWidget(pWho);
        pText->addWidget(pMatch);

        if (!request.inLibrary) {
            auto* pFind = new QPushButton(QStringLiteral("Find"), pRow);
            pFind->setToolTip(QStringLiteral("Look it up in your store or record pool"));
            const QString song = request.song;
            connect(pFind, &QPushButton::clicked, this, [this, song]() {
                QString pattern = m_pConfig->getValueString(kShopUrlKey);
                if (!pattern.contains(QStringLiteral("%1"))) {
                    pattern = kDefaultShopUrl;
                }
                QDesktopServices::openUrl(QUrl(pattern.arg(
                        QString::fromUtf8(QUrl::toPercentEncoding(song)))));
            });
            pRowLayout->addWidget(pFind);
        }
        auto* pComing = new QPushButton(QStringLiteral("Coming up"), pRow);
        pComing->setObjectName(QStringLiteral("ShonReqComing"));
        pComing->setProperty("active", request.status == QStringLiteral("coming"));
        pComing->setToolTip(QStringLiteral("Tells the guest it's coming up"));
        auto* pPlayed = new QPushButton(QStringLiteral("Played"), pRow);
        auto* pDismiss = new QPushButton(QStringLiteral("X"), pRow);
        pDismiss->setObjectName(QStringLiteral("ShonReqDismiss"));
        pDismiss->setToolTip(QStringLiteral("Remove it. The guest is not told."));
        const QString id = request.id;
        const bool coming = request.status == QStringLiteral("coming");
        connect(pComing, &QPushButton::clicked, this, [this, id, coming]() {
            setStatus(id, coming ? QStringLiteral("sent") : QStringLiteral("coming"));
        });
        connect(pPlayed, &QPushButton::clicked, this, [this, id]() {
            setStatus(id, QStringLiteral("played"));
        });
        connect(pDismiss, &QPushButton::clicked, this, [this, id]() {
            setStatus(id, QStringLiteral("dismissed"));
        });
        pRowLayout->addWidget(pComing);
        pRowLayout->addWidget(pPlayed);
        pRowLayout->addWidget(pDismiss);
        pRowLayout->addStretch();
        pText->addLayout(pRowLayout);

        pItem->setSizeHint(pRow->sizeHint());
        m_pList->setItemWidget(pItem, pRow);
    }
    if (m_pList->verticalScrollBar()) {
        m_pList->verticalScrollBar()->setValue(scroll);
    }
}

void ShonRequests::setStatus(const QString& id, const QString& status) {
    // Update the list right away, the relay catches up in the background.
    for (int i = m_requests.size() - 1; i >= 0; --i) {
        if (m_requests[i].id == id) {
            if (status == QStringLiteral("played") || status == QStringLiteral("dismissed")) {
                m_requests.removeAt(i);
            } else {
                m_requests[i].status = status;
            }
        }
    }
    m_lastSignature.clear();
    rebuildList();
    updateHeader();

    QNetworkRequest request(QUrl(relayUrl() + QStringLiteral("/api/p/") + m_code +
            QStringLiteral("/set")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QJsonObject body;
    body.insert(QStringLiteral("key"), m_key);
    body.insert(QStringLiteral("id"), id);
    body.insert(QStringLiteral("status"), status);
    QNetworkReply* pReply =
            m_pNetwork->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(pReply, &QNetworkReply::finished, pReply, &QObject::deleteLater);
}


void ShonRequests::uploadCatalog() {
    // Titles and artists from the "Requestable" crate, so guests get
    // suggestions and a "DJ has it" answer. Nothing else leaves ShonDJ.
    if (!m_open || relayUrl().isEmpty() || !m_pTrackCollectionManager) {
        return;
    }
    QString crate = m_pConfig->getValueString(kCrateKey).trimmed();
    if (crate.isEmpty()) {
        crate = kDefaultCrate;
    }
    QSqlQuery query(m_pTrackCollectionManager->internalCollection()->database());
    query.prepare(QStringLiteral(
            "SELECT library.title, library.artist FROM crate_tracks "
            "JOIN crates ON crate_tracks.crate_id = crates.id "
            "JOIN library ON library.id = crate_tracks.track_id "
            "WHERE lower(crates.name) = lower(?) AND library.mixxx_deleted = 0 "
            "ORDER BY library.artist, library.title LIMIT 5000"));
    query.addBindValue(crate);
    QJsonArray tracks;
    QString signature;
    if (query.exec()) {
        while (query.next()) {
            QJsonObject track;
            const QString title = query.value(0).toString();
            const QString artist = query.value(1).toString();
            track.insert(QStringLiteral("t"), title);
            track.insert(QStringLiteral("a"), artist);
            tracks.append(track);
            signature += title + QLatin1Char('|') + artist + QLatin1Char(';');
        }
    }
    m_catalogCount = tracks.size();
    if (signature == m_catalogSignature) {
        return;
    }
    m_catalogSignature = signature;
    updateHeader();

    QNetworkRequest request(QUrl(relayUrl() + QStringLiteral("/api/p/") + m_code +
            QStringLiteral("/catalog")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QJsonObject body;
    body.insert(QStringLiteral("key"), m_key);
    body.insert(QStringLiteral("tracks"), tracks);
    QNetworkReply* pReply =
            m_pNetwork->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(pReply, &QNetworkReply::finished, pReply, &QObject::deleteLater);
}
