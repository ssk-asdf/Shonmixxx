#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <memory>

#include "preferences/usersettings.h"

class ControlProxy;
class ControlPushButton;
class Library;
class QNetworkAccessManager;
class QTcpServer;
class QTcpSocket;
class QTimer;
class ShonRequests;
class TrackCollectionManager;

/// ShonDJ phone remote. Your phone opens a page served by ShonDJ over the
/// same Wi-Fi (scan the QR code). If the phone drops off the Wi-Fi, the page
/// switches to the requests relay over cell data and back again when Wi-Fi
/// returns. The remote only presses what you press; nothing runs on its own.
class ShonRemote : public QObject {
    Q_OBJECT
  public:
    ShonRemote(UserSettingsPointer pConfig,
            Library* pLibrary,
            TrackCollectionManager* pTrackCollectionManager,
            ShonRequests* pRequests,
            QObject* pParent = nullptr);
    ~ShonRemote() override;

  signals:
    void notice(const QString& text);

  private:
    void setEnabled(bool enabled);
    void onNewConnection();
    void onSocketReadyRead(QTcpSocket* pSocket);
    void respond(QTcpSocket* pSocket,
            int status,
            const QByteArray& contentType,
            const QByteArray& body);
    QByteArray page(bool relayMode) const;
    QJsonObject buildState();
    void runCommand(const QJsonObject& cmd);
    void search(const QString& text);
    void setControl(const QString& group, const QString& item, double value);
    double getControl(const QString& group, const QString& item) const;

    void relaySync();
    QString relayUrl() const;
    QString relayApi() const;
    QString relayPageUrl() const;
    QString lanUrl() const;
    QString findLanAddress() const;
    void showQrCard();

    UserSettingsPointer m_pConfig;
    Library* m_pLibrary;
    TrackCollectionManager* m_pTrackCollectionManager;
    QPointer<ShonRequests> m_pRequests;

    std::unique_ptr<ControlPushButton> m_pEnabled;
    std::unique_ptr<ControlPushButton> m_pShowQr;
    QHash<QString, ControlProxy*> m_proxies;

    QTcpServer* m_pServer = nullptr;
    quint16 m_port = 0;
    QString m_lanAddress;
    QString m_key;       // phone key for the Wi-Fi page and the relay
    QString m_remoteId;  // relay mailbox id
    QString m_hostKey;   // proves to the relay that we are this ShonDJ

    QNetworkAccessManager* m_pNetwork = nullptr;
    QTimer* m_pRelayTimer = nullptr;
    bool m_relayBusy = false;
    bool m_phoneOnRelay = false;
    bool m_relayOnline = false;

    QString m_searchQuery;
    bool m_hasSearch = false;
    QJsonArray m_searchResults;

    QPointer<QWidget> m_pCard;
};
