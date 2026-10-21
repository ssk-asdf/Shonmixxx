#include "library/shonremote.h"

#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QJsonDocument>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QPainter>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSqlQuery>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "control/controlpushbutton.h"
#include "library/library.h"
#include "library/shonrequests.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "mixer/playerinfo.h"
#include "moc_shonremote.cpp"
#include "qrcodegen.hpp"
#include "track/cue.h"
#include "track/track.h"
#include "util/color/rgbcolor.h"

namespace {

const QString kGroup = QStringLiteral("[Shon]");
const ConfigKey kEnabledKey(kGroup, QStringLiteral("remote"));
const ConfigKey kShowQrKey(kGroup, QStringLiteral("remote_qr"));
const ConfigKey kKeyKey(kGroup, QStringLiteral("remote_key"));
const ConfigKey kIdKey(kGroup, QStringLiteral("remote_id"));
const ConfigKey kHostKeyKey(kGroup, QStringLiteral("remote_host_key"));
constexpr quint16 kFirstPort = 8765;
constexpr int kRelayIdleMs = 3000;   // heartbeat while nobody uses the backup
constexpr int kRelayActiveMs = 400;  // while the phone is on the backup

QString randomText(int length, bool upper) {
    const QString chars = upper ? QStringLiteral("ABCDEFGHJKLMNPQRSTUVWXYZ23456789")
                                : QStringLiteral("abcdefghijkmnpqrstuvwxyz23456789");
    QString out;
    for (int i = 0; i < length; ++i) {
        out += chars.at(QRandomGenerator::system()->bounded(chars.size()));
    }
    return out;
}

QString deckGroup(int deck) {
    return QStringLiteral("[Channel%1]").arg(deck);
}

QString stemGroup(int deck, int stem) {
    return QStringLiteral("[Channel%1_Stem%2]").arg(deck).arg(stem);
}

// Deck controls the phone may press. Sliders use the 0..1 knob position.
bool isAllowedDeckControl(const QString& item) {
    static const QRegularExpression kHotcue(QStringLiteral("^hotcue_[1-8]_activate$"));
    return item == QLatin1String("play") || item == QLatin1String("cue_default") ||
            item == QLatin1String("sync_enabled") || item == QLatin1String("volume") ||
            kHotcue.match(item).hasMatch();
}

bool isSliderControl(const QString& item) {
    return item == QLatin1String("volume") || item == QLatin1String("crossfader") ||
            item == QLatin1String("gain");
}

QImage qrImage(const QString& text, int scale) {
    using qrcodegen::QrCode;
    const QrCode qr = QrCode::encodeText(text.toUtf8().constData(), QrCode::Ecc::MEDIUM);
    const int margin = 2;
    const int size = qr.getSize() + margin * 2;
    QImage image(size * scale, size * scale, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    for (int y = 0; y < qr.getSize(); ++y) {
        for (int x = 0; x < qr.getSize(); ++x) {
            if (qr.getModule(x, y)) {
                painter.fillRect((x + margin) * scale, (y + margin) * scale, scale, scale, Qt::black);
            }
        }
    }
    return image;
}

} // namespace

ShonRemote::ShonRemote(UserSettingsPointer pConfig,
        Library* pLibrary,
        TrackCollectionManager* pTrackCollectionManager,
        ShonRequests* pRequests,
        QObject* pParent)
        : QObject(pParent),
          m_pConfig(pConfig),
          m_pLibrary(pLibrary),
          m_pTrackCollectionManager(pTrackCollectionManager),
          m_pRequests(pRequests) {
    m_key = m_pConfig->getValueString(kKeyKey);
    if (m_key.size() < 10) {
        m_key = randomText(10, false);
        m_pConfig->setValue(kKeyKey, m_key);
    }
    m_remoteId = m_pConfig->getValueString(kIdKey);
    if (m_remoteId.size() < 8) {
        m_remoteId = randomText(8, true);
        m_pConfig->setValue(kIdKey, m_remoteId);
    }
    m_hostKey = m_pConfig->getValueString(kHostKeyKey);
    if (m_hostKey.size() < 20) {
        m_hostKey = randomText(24, false);
        m_pConfig->setValue(kHostKeyKey, m_hostKey);
    }

    m_pNetwork = new QNetworkAccessManager(this);
    m_pRelayTimer = new QTimer(this);
    m_pRelayTimer->setInterval(kRelayIdleMs);
    connect(m_pRelayTimer, &QTimer::timeout, this, &ShonRemote::relaySync);

    m_pEnabled = std::make_unique<ControlPushButton>(kEnabledKey, true);
    m_pEnabled->setButtonMode(mixxx::control::ButtonMode::Toggle);
    connect(m_pEnabled.get(), &ControlObject::valueChanged, this, [this](double value) {
        setEnabled(value > 0.0);
        if (value > 0.0) {
            showQrCard();
        }
    });
    m_pShowQr = std::make_unique<ControlPushButton>(kShowQrKey);
    connect(m_pShowQr.get(), &ControlObject::valueChanged, this, [this](double value) {
        if (value > 0.0) {
            if (m_pEnabled->get() <= 0.0) {
                m_pEnabled->set(1.0);
                setEnabled(true);
            }
            showQrCard();
        }
    });

    if (m_pEnabled->get() > 0.0) {
        // Remembered from last time. Start quietly, no QR card.
        QTimer::singleShot(0, this, [this]() { setEnabled(true); });
    }
}

ShonRemote::~ShonRemote() {
    if (m_pCard) {
        m_pCard->deleteLater();
    }
}

void ShonRemote::setEnabled(bool enabled) {
    if (!enabled) {
        if (m_pServer) {
            m_pServer->close();
            m_pServer->deleteLater();
            m_pServer = nullptr;
        }
        m_pRelayTimer->stop();
        if (m_pCard) {
            m_pCard->hide();
        }
        emit notice(QStringLiteral("Phone remote: off"));
        return;
    }
    if (!m_pServer) {
        m_pServer = new QTcpServer(this);
        connect(m_pServer, &QTcpServer::newConnection, this, &ShonRemote::onNewConnection);
        m_port = 0;
        for (quint16 port = kFirstPort; port < kFirstPort + 10; ++port) {
            if (m_pServer->listen(QHostAddress::AnyIPv4, port)) {
                m_port = port;
                break;
            }
        }
        if (m_port == 0) {
            emit notice(QStringLiteral("Phone remote: could not open a port"));
        }
    }
    m_lanAddress = findLanAddress();
    if (!relayUrl().isEmpty()) {
        m_pRelayTimer->start();
        relaySync();
    }
}

QString ShonRemote::findLanAddress() const {
    // Prefer a home network address (192.168.x.x, 10.x.x.x, 172.16-31.x.x)
    // on an interface that is up.
    QString fallback;
    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface& iface : interfaces) {
        const auto flags = iface.flags();
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning) ||
                (flags & QNetworkInterface::IsLoopBack)) {
            continue;
        }
        const QString name = iface.humanReadableName().toLower();
        const bool virtualAdapter = name.contains(QLatin1String("vethernet")) ||
                name.contains(QLatin1String("virtual")) || name.contains(QLatin1String("vpn")) ||
                name.contains(QLatin1String("wsl")) || name.contains(QLatin1String("hyper-v"));
        for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
            const QHostAddress address = entry.ip();
            if (address.protocol() != QAbstractSocket::IPv4Protocol) {
                continue;
            }
            const quint32 ip = address.toIPv4Address();
            const bool isPrivate = (ip >> 24) == 10 || (ip >> 16) == 0xC0A8 ||
                    ((ip >> 20) == 0xAC1);
            if (!isPrivate) {
                continue;
            }
            if (!virtualAdapter && (ip >> 16) == 0xC0A8) {
                return address.toString();
            }
            if (fallback.isEmpty() || !virtualAdapter) {
                fallback = address.toString();
            }
        }
    }
    return fallback;
}

QString ShonRemote::lanUrl() const {
    if (m_lanAddress.isEmpty() || m_port == 0) {
        return QString();
    }
    return QStringLiteral("http://%1:%2/r?k=%3").arg(m_lanAddress).arg(m_port).arg(m_key);
}

QString ShonRemote::relayUrl() const {
    return m_pRequests ? m_pRequests->relayBaseUrl() : QString();
}

QString ShonRemote::relayApi() const {
    return relayUrl() + QStringLiteral("/api/r/") + m_remoteId;
}

QString ShonRemote::relayPageUrl() const {
    return relayUrl() + QStringLiteral("/remote/") + m_remoteId + QStringLiteral("?k=") + m_key;
}

// ---------------------------------------------------------------- Wi-Fi server

void ShonRemote::onNewConnection() {
    while (m_pServer && m_pServer->hasPendingConnections()) {
        QTcpSocket* pSocket = m_pServer->nextPendingConnection();
        connect(pSocket, &QTcpSocket::readyRead, this, [this, pSocket]() {
            onSocketReadyRead(pSocket);
        });
        connect(pSocket, &QTcpSocket::disconnected, pSocket, &QObject::deleteLater);
        // Drop sockets that never send a full request.
        QTimer::singleShot(5000, pSocket, [pSocket]() { pSocket->abort(); });
    }
}

void ShonRemote::onSocketReadyRead(QTcpSocket* pSocket) {
    QByteArray buffer = pSocket->property("shonBuffer").toByteArray() + pSocket->readAll();
    if (buffer.size() > 64 * 1024) {
        pSocket->abort();
        return;
    }
    const int headerEnd = buffer.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
        pSocket->setProperty("shonBuffer", buffer);
        return;
    }
    const QList<QByteArray> lines = buffer.left(headerEnd).split('\n');
    const QList<QByteArray> requestLine = lines.value(0).trimmed().split(' ');
    int contentLength = 0;
    for (const QByteArray& line : lines) {
        if (line.toLower().startsWith("content-length:")) {
            contentLength = line.mid(15).trimmed().toInt();
        }
    }
    const QByteArray body = buffer.mid(headerEnd + 4);
    if (body.size() < contentLength) {
        pSocket->setProperty("shonBuffer", buffer);
        return;
    }
    pSocket->setProperty("shonBuffer", QByteArray());

    const QByteArray method = requestLine.value(0);
    const QUrl url(QString::fromUtf8(requestLine.value(1)));
    const QString path = url.path();
    const QString key = QUrlQuery(url).queryItemValue(QStringLiteral("k"));
    if (key != m_key) {
        respond(pSocket, 403, "text/plain", "Scan the QR code in ShonDJ again.");
        return;
    }
    if (method == "GET" && (path == QLatin1String("/r") || path == QLatin1String("/"))) {
        respond(pSocket, 200, "text/html; charset=utf-8", page(false));
    } else if (method == "GET" && path == QLatin1String("/api/state")) {
        respond(pSocket,
                200,
                "application/json",
                QJsonDocument(buildState()).toJson(QJsonDocument::Compact));
    } else if (method == "POST" && path == QLatin1String("/api/cmd")) {
        runCommand(QJsonDocument::fromJson(body.left(contentLength)).object());
        respond(pSocket, 200, "application/json", "{\"ok\":true}");
    } else {
        respond(pSocket, 404, "text/plain", "Not found");
    }
}

void ShonRemote::respond(QTcpSocket* pSocket,
        int status,
        const QByteArray& contentType,
        const QByteArray& body) {
    const QByteArray reason = status == 200 ? "OK" : status == 403 ? "Forbidden" : "Not Found";
    QByteArray out = "HTTP/1.1 " + QByteArray::number(status) + " " + reason + "\r\n";
    out += "Content-Type: " + contentType + "\r\n";
    out += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    out += "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
    out += body;
    pSocket->write(out);
    pSocket->disconnectFromHost();
}

QByteArray ShonRemote::page(bool relayMode) const {
    QFile file(QStringLiteral(":/images/shon/remote.html"));
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArrayLiteral("ShonDJ remote page missing.");
    }
    QByteArray html = file.readAll();
    QJsonObject config;
    config.insert(QStringLiteral("mode"), relayMode ? QStringLiteral("relay") : QStringLiteral("lan"));
    config.insert(QStringLiteral("key"), m_key);
    config.insert(QStringLiteral("phoneKey"), m_key);
    if (!relayUrl().isEmpty()) {
        config.insert(QStringLiteral("relay"), relayApi());
    }
    const QByteArray script = QByteArrayLiteral("window.SHON=") +
            QJsonDocument(config).toJson(QJsonDocument::Compact) + QByteArrayLiteral(";");
    html.replace(QByteArrayLiteral("/*SHON_CONFIG*/"), script);
    return html;
}

// ---------------------------------------------------------------- state

double ShonRemote::getControl(const QString& group, const QString& item) const {
    return ControlObject::get(ConfigKey(group, item));
}

void ShonRemote::setControl(const QString& group, const QString& item, double value) {
    const QString id = group + QLatin1Char(',') + item;
    ControlProxy* pProxy = m_proxies.value(id);
    if (!pProxy) {
        pProxy = new ControlProxy(group, item, this, ControlFlag::NoWarnIfMissing);
        m_proxies.insert(id, pProxy);
    }
    if (!pProxy->valid()) {
        return;
    }
    if (isSliderControl(item)) {
        pProxy->setParameter(qBound(0.0, value, 1.0));
    } else {
        pProxy->set(value);
    }
}

QJsonObject ShonRemote::buildState() {
    QJsonObject state;
    state.insert(QStringLiteral("host"), true);
    const auto parameterOf = [](const QString& group, const QString& item, double fallback) {
        ControlObject* pControl = ControlObject::getControl(
                ConfigKey(group, item), ControlFlag::NoWarnIfMissing);
        return pControl ? pControl->getParameter() : fallback;
    };
    const int numDecks = qBound(1, static_cast<int>(getControl(QStringLiteral("[App]"),
                                           QStringLiteral("num_decks"))),
            4);
    QJsonArray decks;
    for (int n = 1; n <= numDecks; ++n) {
        const QString group = deckGroup(n);
        QJsonObject deck;
        deck.insert(QStringLiteral("n"), n);
        const bool loaded = getControl(group, QStringLiteral("track_loaded")) > 0.0;
        deck.insert(QStringLiteral("loaded"), loaded);
        TrackPointer pTrack = loaded ? PlayerInfo::instance().getTrackInfo(group) : TrackPointer();
        if (pTrack) {
            deck.insert(QStringLiteral("title"), pTrack->getTitle());
            deck.insert(QStringLiteral("artist"), pTrack->getArtist());
            QJsonArray cues;
            const auto cuePoints = pTrack->getCuePoints();
            for (const CuePointer& pCue : cuePoints) {
                const int hotcue = pCue->getHotCue();
                if (hotcue < 0 || hotcue > 7) {
                    continue;
                }
                QJsonObject cue;
                cue.insert(QStringLiteral("n"), hotcue + 1);
                cue.insert(QStringLiteral("label"), pCue->getLabel());
                cue.insert(QStringLiteral("color"),
                        mixxx::RgbColor::toQColor(pCue->getColor()).name());
                cues.append(cue);
            }
            deck.insert(QStringLiteral("cues"), cues);
        }
        deck.insert(QStringLiteral("bpm"), getControl(group, QStringLiteral("visual_bpm")));
        deck.insert(QStringLiteral("play"), getControl(group, QStringLiteral("play")) > 0.0);
        deck.insert(QStringLiteral("pos"), getControl(group, QStringLiteral("playposition")));
        deck.insert(QStringLiteral("dur"), getControl(group, QStringLiteral("duration")));
        deck.insert(QStringLiteral("sync"), getControl(group, QStringLiteral("sync_enabled")) > 0.0);
        deck.insert(QStringLiteral("vol"), parameterOf(group, QStringLiteral("volume"), 1.0));
        const int stemCount = static_cast<int>(getControl(group, QStringLiteral("stem_count")));
        if (stemCount > 0) {
            QJsonArray stems;
            for (int s = 1; s <= qMin(stemCount, 4); ++s) {
                const QString sg = stemGroup(n, s);
                ControlObject* pVolume = ControlObject::getControl(
                        ConfigKey(sg, QStringLiteral("volume")), ControlFlag::NoWarnIfMissing);
                QJsonObject stem;
                stem.insert(QStringLiteral("vol"), pVolume ? pVolume->getParameter() : 1.0);
                stem.insert(QStringLiteral("mute"), getControl(sg, QStringLiteral("mute")) > 0.0);
                stems.append(stem);
            }
            deck.insert(QStringLiteral("stems"), stems);
        }
        decks.append(deck);
    }
    state.insert(QStringLiteral("decks"), decks);
    state.insert(QStringLiteral("away"),
            getControl(QStringLiteral("[AutoDJ]"), QStringLiteral("shon_away_mode")) > 0.0);

    state.insert(QStringLiteral("xf"),
            parameterOf(QStringLiteral("[Master]"), QStringLiteral("crossfader"), 0.5));
    state.insert(QStringLiteral("gain"),
            parameterOf(QStringLiteral("[Master]"), QStringLiteral("gain"), 0.5));

    QJsonArray requests;
    if (m_pRequests) {
        const auto& list = m_pRequests->requestList();
        for (const auto& request : list) {
            if (request.status == QLatin1String("played") ||
                    request.status == QLatin1String("skipped")) {
                continue;
            }
            QJsonObject r;
            r.insert(QStringLiteral("id"), request.id);
            r.insert(QStringLiteral("song"), request.song);
            r.insert(QStringLiteral("names"), QJsonArray::fromStringList(request.names));
            r.insert(QStringLiteral("votes"), request.votes);
            r.insert(QStringLiteral("status"), request.status);
            r.insert(QStringLiteral("has"), request.inLibrary && !request.location.isEmpty());
            requests.append(r);
        }
    }
    state.insert(QStringLiteral("requests"), requests);

    if (m_hasSearch) {
        QJsonObject search;
        search.insert(QStringLiteral("q"), m_searchQuery);
        search.insert(QStringLiteral("results"), m_searchResults);
        state.insert(QStringLiteral("search"), search);
    }
    return state;
}

// ---------------------------------------------------------------- commands

void ShonRemote::runCommand(const QJsonObject& cmd) {
    const QString c = cmd.value(QStringLiteral("c")).toString();
    const int deck = cmd.value(QStringLiteral("d")).toInt();
    const double value = cmd.value(QStringLiteral("v")).toDouble();
    const int numDecks = static_cast<int>(getControl(QStringLiteral("[App]"),
            QStringLiteral("num_decks")));
    const bool deckOk = deck >= 1 && deck <= qBound(1, numDecks, 4);

    if (c == QLatin1String("set") && deckOk) {
        const QString item = cmd.value(QStringLiteral("k")).toString();
        if (isAllowedDeckControl(item)) {
            setControl(deckGroup(deck), item, value);
        }
    } else if (c == QLatin1String("stem") && deckOk) {
        const int stem = cmd.value(QStringLiteral("s")).toInt();
        const QString item = cmd.value(QStringLiteral("k")).toString();
        if (stem >= 1 && stem <= 4 &&
                (item == QLatin1String("volume") || item == QLatin1String("mute"))) {
            setControl(stemGroup(deck, stem), item, value);
        }
    } else if (c == QLatin1String("master")) {
        const QString item = cmd.value(QStringLiteral("k")).toString();
        if (item == QLatin1String("crossfader") || item == QLatin1String("gain")) {
            setControl(QStringLiteral("[Master]"), item, value);
        }
    } else if (c == QLatin1String("away")) {
        // Away Mode: Auto DJ keeps a matched set going.
        setControl(QStringLiteral("[AutoDJ]"), QStringLiteral("shon_away_mode"), value > 0.0 ? 1.0 : 0.0);
    } else if (c == QLatin1String("search")) {
        search(cmd.value(QStringLiteral("q")).toString().left(80));
    } else if (c == QLatin1String("load") && deckOk) {
        const int id = cmd.value(QStringLiteral("id")).toInt();
        QSqlQuery query(m_pTrackCollectionManager->internalCollection()->database());
        query.prepare(QStringLiteral(
                "SELECT track_locations.location FROM library JOIN track_locations "
                "ON library.location = track_locations.id WHERE library.id = ?"));
        query.addBindValue(id);
        if (query.exec() && query.next()) {
            m_pLibrary->slotLoadLocationToPlayer(query.value(0).toString(), deckGroup(deck), false);
        }
    } else if (c == QLatin1String("loadreq") && deckOk && m_pRequests) {
        const QString id = cmd.value(QStringLiteral("id")).toString();
        for (const auto& request : m_pRequests->requestList()) {
            if (request.id == id && !request.location.isEmpty()) {
                m_pLibrary->slotLoadLocationToPlayer(request.location, deckGroup(deck), false);
                break;
            }
        }
    }
}

void ShonRemote::search(const QString& text) {
    m_hasSearch = true;
    m_searchQuery = text;
    m_searchResults = QJsonArray();
    const QStringList words = text.toLower().split(
            QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    // Nothing typed: the whole library.
    QStringList conditions{QStringLiteral("1")};
    for (int i = 0; i < words.size() && i < 6; ++i) {
        conditions << QStringLiteral(
                "lower(coalesce(artist, '') || ' ' || coalesce(title, '')) LIKE ?");
    }
    QSqlQuery query(m_pTrackCollectionManager->internalCollection()->database());
    query.prepare(QStringLiteral(
            "SELECT id, title, artist, bpm, key FROM library "
            "WHERE mixxx_deleted = 0 AND ") +
            conditions.join(QStringLiteral(" AND ")) +
            QStringLiteral(" ORDER BY artist, title LIMIT 300"));
    for (int i = 0; i < words.size() && i < 6; ++i) {
        const QString pattern = QLatin1Char('%') + words.at(i) + QLatin1Char('%');
        query.addBindValue(pattern);
    }
    if (!query.exec()) {
        return;
    }
    while (query.next()) {
        QJsonObject r;
        r.insert(QStringLiteral("id"), query.value(0).toInt());
        r.insert(QStringLiteral("title"), query.value(1).toString());
        r.insert(QStringLiteral("artist"), query.value(2).toString());
        r.insert(QStringLiteral("bpm"), query.value(3).toDouble());
        r.insert(QStringLiteral("key"), query.value(4).toString());
        m_searchResults.append(r);
    }
}

// ---------------------------------------------------------------- relay backup

void ShonRemote::relaySync() {
    if (m_relayBusy || relayUrl().isEmpty()) {
        return;
    }
    m_relayBusy = true;
    QNetworkRequest request(QUrl(relayApi() + QStringLiteral("/rsync")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QJsonObject body;
    body.insert(QStringLiteral("key"), m_hostKey);
    body.insert(QStringLiteral("phoneKey"), m_key);
    // A recent state is always waiting on the relay, so a phone that just
    // lost Wi-Fi sees the decks right away.
    body.insert(QStringLiteral("state"), buildState());
    QNetworkReply* pReply =
            m_pNetwork->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(pReply, &QNetworkReply::finished, this, [this, pReply]() {
        pReply->deleteLater();
        m_relayBusy = false;
        m_relayOnline = pReply->error() == QNetworkReply::NoError;
        if (!m_relayOnline) {
            return;
        }
        const QJsonObject reply = QJsonDocument::fromJson(pReply->readAll()).object();
        const QJsonArray cmds = reply.value(QStringLiteral("cmds")).toArray();
        for (const QJsonValue& cmd : cmds) {
            runCommand(cmd.toObject());
        }
        const bool active = reply.value(QStringLiteral("active")).toBool();
        if (active != m_phoneOnRelay) {
            m_phoneOnRelay = active;
            m_pRelayTimer->setInterval(active ? kRelayActiveMs : kRelayIdleMs);
            if (active) {
                // Send the state right away so the phone doesn't wait.
                QTimer::singleShot(0, this, &ShonRemote::relaySync);
            }
        }
    });
}

// ---------------------------------------------------------------- QR card

void ShonRemote::showQrCard() {
    QWidget* pParent = QApplication::activeWindow();
    if (!m_pCard) {
        m_pCard = new QWidget(pParent, Qt::Tool | Qt::FramelessWindowHint);
        m_pCard->setAttribute(Qt::WA_DeleteOnClose, false);
        m_pCard->setObjectName(QStringLiteral("ShonRemoteCard"));
        m_pCard->setStyleSheet(QStringLiteral(
                "#ShonRemoteCard { background: #17171a; border: 1px solid #2a2a2e; "
                "border-radius: 14px; }"
                "QLabel { color: #e6e6ea; font-size: 13px; }"
                "QLabel#ShonRemoteTitle { font-size: 16px; font-weight: 700; color: #ffffff; }"
                "QLabel#ShonRemoteDim { color: #8a8a92; font-size: 12px; }"
                "QPushButton { background: #232329; color: #e6e6ea; border: none; "
                "border-radius: 8px; padding: 8px 14px; font-weight: 600; }"
                "QPushButton:hover { background: #2f6bff; color: #ffffff; }"));
        auto* pLayout = new QVBoxLayout(m_pCard);
        pLayout->setContentsMargins(22, 20, 22, 18);
        pLayout->setSpacing(10);
        auto* pTitle = new QLabel(QStringLiteral("Phone remote"), m_pCard);
        pTitle->setObjectName(QStringLiteral("ShonRemoteTitle"));
        pLayout->addWidget(pTitle);
        auto* pHint = new QLabel(QStringLiteral("Scan with your phone camera. "
                                                "Your phone must be on the same Wi-Fi."),
                m_pCard);
        pHint->setObjectName(QStringLiteral("ShonRemoteDim"));
        pHint->setWordWrap(true);
        pLayout->addWidget(pHint);
        auto* pQr = new QLabel(m_pCard);
        pQr->setObjectName(QStringLiteral("ShonRemoteQr"));
        pQr->setAlignment(Qt::AlignCenter);
        pLayout->addWidget(pQr, 0, Qt::AlignHCenter);
        auto* pLink = new QLabel(m_pCard);
        pLink->setObjectName(QStringLiteral("ShonRemoteLink"));
        pLink->setAlignment(Qt::AlignCenter);
        pLink->setTextInteractionFlags(Qt::TextSelectableByMouse);
        pLayout->addWidget(pLink);
        auto* pBackup = new QLabel(m_pCard);
        pBackup->setObjectName(QStringLiteral("ShonRemoteDim"));
        pBackup->setWordWrap(true);
        pBackup->setProperty("shonBackup", true);
        pLayout->addWidget(pBackup);
        auto* pButtons = new QHBoxLayout();
        auto* pCopy = new QPushButton(QStringLiteral("Copy backup link"), m_pCard);
        auto* pOff = new QPushButton(QStringLiteral("Turn off"), m_pCard);
        auto* pClose = new QPushButton(QStringLiteral("Done"), m_pCard);
        pButtons->addWidget(pCopy);
        pButtons->addStretch();
        pButtons->addWidget(pOff);
        pButtons->addWidget(pClose);
        pLayout->addLayout(pButtons);
        connect(pCopy, &QPushButton::clicked, this, [this]() {
            if (!relayUrl().isEmpty()) {
                QApplication::clipboard()->setText(relayPageUrl());
                emit notice(QStringLiteral("Backup remote link copied"));
            }
        });
        connect(pOff, &QPushButton::clicked, this, [this]() {
            m_pEnabled->set(0.0);
            setEnabled(false);
        });
        connect(pClose, &QPushButton::clicked, m_pCard, &QWidget::hide);
    }

    m_lanAddress = findLanAddress();
    const QString url = lanUrl();
    auto* pQr = m_pCard->findChild<QLabel*>(QStringLiteral("ShonRemoteQr"));
    auto* pLink = m_pCard->findChild<QLabel*>(QStringLiteral("ShonRemoteLink"));
    if (url.isEmpty()) {
        pQr->setPixmap(QPixmap());
        pQr->setText(QStringLiteral("No Wi-Fi network found."));
        pLink->clear();
    } else {
        pQr->setPixmap(QPixmap::fromImage(qrImage(url, 7)));
        pLink->setText(QStringLiteral("%1:%2").arg(m_lanAddress).arg(m_port));
    }
    const auto labels = m_pCard->findChildren<QLabel*>();
    for (QLabel* pLabel : labels) {
        if (pLabel->property("shonBackup").toBool()) {
            pLabel->setText(relayUrl().isEmpty()
                            ? QStringLiteral("Backup over cell data needs the requests relay "
                                             "link (set it in Requests).")
                            : QStringLiteral("If your phone leaves the Wi-Fi, the remote "
                                             "switches to cell data by itself."));
        }
    }
    m_pCard->adjustSize();
    if (pParent) {
        const QRect area = pParent->geometry();
        m_pCard->move(area.center() - QPoint(m_pCard->width() / 2, m_pCard->height() / 2));
    }
    m_pCard->show();
    m_pCard->raise();
}
