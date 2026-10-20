#include "library/shonrequests.h"

#include <QApplication>
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
#include <QScrollBar>
#include <QSqlQuery>
#include <QSvgWidget>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>
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
const QString kDefaultShopUrl = QStringLiteral("https://www.beatport.com/search?q=%1");
constexpr int kPollMs = 3000;
constexpr int kRoleLocation = Qt::UserRole + 1;

const char* const kWindowStyle = R"(
QWidget#ShonRequestsWindow { background: #151517; }
QLabel { color: #e6e6e6; }
QLabel#ShonReqTitle { font-size: 18px; font-weight: 600; color: #ffffff; }
QLabel#ShonReqDim { color: #8a8a92; }
QLabel#ShonReqSong { font-size: 14px; font-weight: 600; color: #ffffff; }
QLabel#ShonReqMatch { color: #8a8a92; }
QLabel#ShonReqMatch[fits="true"] { color: #8fb0ff; }
QLabel#ShonReqLink { color: #8fb0ff; }
QPushButton { background: #26262b; color: #e6e6e6; border: 1px solid #36363d;
  border-radius: 6px; padding: 5px 12px; }
QPushButton:hover { background: #2e2e34; }
QPushButton#ShonReqOpen[open="true"] { background: #2f6bff; border-color: #2f6bff; color: #ffffff; }
QPushButton#ShonReqComing[active="true"] { background: #2f6bff; border-color: #2f6bff; color: #ffffff; }
QPushButton#ShonReqDismiss { padding: 5px 9px; }
QLineEdit { background: #222226; color: #e6e6e6; border: 1px solid #33333a;
  border-radius: 6px; padding: 5px 8px; }
QListWidget { background: #151517; border: none; outline: none; }
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
    m_code = m_pConfig->getValueString(kCodeKey);
    m_key = m_pConfig->getValueString(kHostKeyKey);
    if (m_code.isEmpty() || m_key.size() < 16) {
        newParty();
    }

    // [Shon],requests_window opens the inbox (toolbar button).
    m_pShowWindow = std::make_unique<ControlPushButton>(
            ConfigKey(kGroup, QStringLiteral("requests_window")));
    m_pShowWindow->setButtonMode(mixxx::control::ButtonMode::Toggle);
    connect(m_pShowWindow.get(), &ControlObject::valueChanged, this, [this](double value) {
        showWindow(value > 0.0);
    });

    m_pPollTimer->setInterval(kPollMs);
    connect(m_pPollTimer, &QTimer::timeout, this, &ShonRequests::poll);
}

ShonRequests::~ShonRequests() {
    if (m_pWindow) {
        delete m_pWindow;
    }
}

QString ShonRequests::relayUrl() const {
    QString url = m_pConfig->getValueString(kRelayUrlKey).trimmed();
    while (url.endsWith(QLatin1Char('/'))) {
        url.chop(1);
    }
    return url;
}

QString ShonRequests::guestUrl() const {
    return relayUrl() + QStringLiteral("/p/") + m_code;
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
    if (m_pWindow) {
        loadQr();
        rebuildList();
        updateHeader();
    }
}

void ShonRequests::showWindow(bool show) {
    if (!show) {
        if (m_pWindow) {
            m_pWindow->hide();
        }
        return;
    }
    if (!m_pWindow) {
        buildWindow();
    }
    m_pWindow->show();
    m_pWindow->raise();
    m_pWindow->activateWindow();
    if (m_qrSvg.isEmpty()) {
        loadQr();
    }
}

void ShonRequests::buildWindow() {
    auto* pWindow = new QWidget(nullptr, Qt::Window);
    pWindow->setObjectName(QStringLiteral("ShonRequestsWindow"));
    pWindow->setWindowTitle(QStringLiteral("ShonDJ Requests"));
    pWindow->setStyleSheet(QString::fromUtf8(kWindowStyle));
    pWindow->resize(560, 680);
    m_pWindow = pWindow;

    auto* pLayout = new QVBoxLayout(pWindow);
    pLayout->setContentsMargins(16, 16, 16, 16);
    pLayout->setSpacing(12);

    // Header: title, status, open/close and new party.
    auto* pHeader = new QHBoxLayout();
    auto* pTitle = new QLabel(QStringLiteral("Song requests"), pWindow);
    pTitle->setObjectName(QStringLiteral("ShonReqTitle"));
    m_pStatusLabel = new QLabel(pWindow);
    m_pStatusLabel->setObjectName(QStringLiteral("ShonReqDim"));
    m_pOpenButton = new QPushButton(pWindow);
    m_pOpenButton->setObjectName(QStringLiteral("ShonReqOpen"));
    connect(m_pOpenButton, &QPushButton::clicked, this, [this]() {
        setOpen(!m_open);
    });
    auto* pNewParty = new QPushButton(QStringLiteral("New party"), pWindow);
    pNewParty->setToolTip(QStringLiteral(
            "New code and QR. Old links stop working and the inbox is cleared."));
    connect(pNewParty, &QPushButton::clicked, this, &ShonRequests::newParty);
    pHeader->addWidget(pTitle);
    pHeader->addSpacing(8);
    pHeader->addWidget(m_pStatusLabel, 1);
    pHeader->addWidget(m_pOpenButton);
    pHeader->addWidget(pNewParty);
    pLayout->addLayout(pHeader);

    // One-time setup: where the relay lives.
    m_pSetupBox = new QWidget(pWindow);
    auto* pSetup = new QHBoxLayout(m_pSetupBox);
    pSetup->setContentsMargins(0, 0, 0, 0);
    m_pRelayEdit = new QLineEdit(relayUrl(), m_pSetupBox);
    m_pRelayEdit->setPlaceholderText(
            QStringLiteral("Relay link, e.g. https://shondj-requests.you.workers.dev"));
    auto* pSave = new QPushButton(QStringLiteral("Save"), m_pSetupBox);
    connect(pSave, &QPushButton::clicked, this, [this]() {
        m_pConfig->setValue(kRelayUrlKey, m_pRelayEdit->text().trimmed());
        m_qrSvg.clear();
        loadQr();
        updateHeader();
    });
    pSetup->addWidget(m_pRelayEdit, 1);
    pSetup->addWidget(pSave);
    pLayout->addWidget(m_pSetupBox);

    // QR code and link.
    auto* pQrRow = new QHBoxLayout();
    m_pQr = new QSvgWidget(pWindow);
    m_pQr->setFixedSize(150, 150);
    m_pQr->setStyleSheet(QStringLiteral("background: #ffffff; border-radius: 8px;"));
    auto* pQrSide = new QVBoxLayout();
    auto* pScan = new QLabel(QStringLiteral("Guests scan this to request a song."), pWindow);
    pScan->setWordWrap(true);
    m_pLinkLabel = new QLabel(pWindow);
    m_pLinkLabel->setObjectName(QStringLiteral("ShonReqLink"));
    m_pLinkLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_pLinkLabel->setWordWrap(true);
    auto* pBig = new QPushButton(QStringLiteral("Show QR full screen"), pWindow);
    connect(pBig, &QPushButton::clicked, this, &ShonRequests::showBigQr);
    auto* pHint = new QLabel(
            QStringLiteral("Drag a request onto a deck to load it. Double-click to search "
                           "your library for it."),
            pWindow);
    pHint->setObjectName(QStringLiteral("ShonReqDim"));
    pHint->setWordWrap(true);
    pQrSide->addWidget(pScan);
    pQrSide->addWidget(m_pLinkLabel);
    pQrSide->addWidget(pBig, 0, Qt::AlignLeft);
    pQrSide->addStretch();
    pQrSide->addWidget(pHint);
    pQrRow->addWidget(m_pQr);
    pQrRow->addSpacing(12);
    pQrRow->addLayout(pQrSide, 1);
    pLayout->addLayout(pQrRow);

    // The inbox.
    m_pList = new ShonRequestList(pWindow);
    m_pList->setDragEnabled(true);
    m_pList->setDragDropMode(QAbstractItemView::DragOnly);
    m_pList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_pList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    connect(m_pList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* pItem) {
        const QString song = pItem->data(Qt::UserRole).toString();
        if (m_pLibrary && !song.isEmpty()) {
            m_pLibrary->searchTracksInCollection(song);
        }
    });
    pLayout->addWidget(m_pList, 1);

    // Closing the window turns the toolbar button off.
    pWindow->installEventFilter(this);
    connect(pWindow, &QObject::destroyed, this, [this]() {
        m_pList = nullptr;
    });

    updateHeader();
    rebuildList();
}

bool ShonRequests::eventFilter(QObject* pObject, QEvent* pEvent) {
    if (pObject == m_pWindow && pEvent->type() == QEvent::Hide && m_pShowWindow) {
        m_pShowWindow->set(0.0);
    }
    return QObject::eventFilter(pObject, pEvent);
}

void ShonRequests::updateHeader() {
    if (!m_pWindow) {
        return;
    }
    const bool configured = !relayUrl().isEmpty();
    m_pSetupBox->setVisible(!configured || m_pRelayEdit->hasFocus());
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
        status = QStringLiteral("code %1, %2 waiting").arg(m_code).arg(m_requests.size());
    }
    m_pStatusLabel->setText(status);
}

void ShonRequests::loadQr() {
    if (relayUrl().isEmpty() || !m_pWindow) {
        return;
    }
    QNetworkReply* pReply = m_pNetwork->get(
            QNetworkRequest(QUrl(relayUrl() + QStringLiteral("/qr/") + m_code +
                    QStringLiteral(".svg"))));
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
            if (open) {
                m_primed = false;
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
        request.votes = item.value(QStringLiteral("votes")).toInt(1);
        request.status = item.value(QStringLiteral("status")).toString();
        for (const QJsonValue& name : item.value(QStringLiteral("names")).toArray()) {
            request.names << name.toString();
        }
        matchInLibrary(&request);
        signature += request.id + request.status + QString::number(request.votes) +
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
        QSqlDatabase database = m_pTrackCollectionManager->internalCollection()->database();
        for (int drop = 0; drop <= 1 && !pRequest->inLibrary && !words.isEmpty(); ++drop) {
            const QStringList use = drop == 0 ? words : words.mid(0, words.size() - 1);
            if (use.isEmpty() || (drop == 1 && words.size() < 3)) {
                break;
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
                query.addBindValue(QStringLiteral("%") + word + QStringLiteral("%"));
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
        if (!pRequest->inLibrary) {
            m_matchCache.insert(pRequest->id, *pRequest);
        }
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
        auto* pRowLayout = new QHBoxLayout(pRow);
        pRowLayout->setContentsMargins(8, 8, 8, 8);
        auto* pText = new QVBoxLayout();
        pText->setSpacing(2);
        auto* pSong = new QLabel(request.song, pRow);
        pSong->setObjectName(QStringLiteral("ShonReqSong"));
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
        pText->addWidget(pSong);
        pText->addWidget(pWho);
        pText->addWidget(pMatch);
        pRowLayout->addLayout(pText, 1);

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
