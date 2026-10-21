#pragma once

#include <QHash>
#include <QJsonObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QWidget>
#include <functional>
#include <memory>

#include "preferences/usersettings.h"

class ControlPushButton;
class Library;
class QLabel;
class QLineEdit;
class QListWidget;
class QNetworkAccessManager;
class QPushButton;
class QVBoxLayout;
class QSvgWidget;
class QTimer;
class TrackCollectionManager;

/// ShonDJ guest song requests.
/// Guests scan a QR code and send requests through a small relay server.
/// This polls the relay and shows an inbox you can drag tracks from.
/// Nothing here plays, queues or reorders anything on its own.
class ShonRequests : public QObject {
    Q_OBJECT
  public:
    ShonRequests(UserSettingsPointer pConfig,
            Library* pLibrary,
            TrackCollectionManager* pTrackCollectionManager,
            QObject* pParent = nullptr);
    ~ShonRequests() override;

    struct Request {
        QString id;
        QString song;
        QStringList names;
        int votes = 1;
        QString status;
        // Library match, filled in locally.
        bool inLibrary = false;
        QString location;
        QString matchText;
        bool fits = false;
        // Tip state from the relay: 0 none, 1 pending (guest says they sent it),
        // 2 confirmed by you. Only confirmed tips are pinned to the top.
        int tipState = 0;
        bool tipped = false; // tipState == 2
        QString tipCode;     // what the guest put in the payment note, like SHON-4K2
        QString playedNote;
        double bpm = 0.0;
        int key = 0;
    };

    /// Builds the inbox panel shown at the right side of the main window.
    QWidget* createView(QWidget* pParent);
    void onViewShown();

    /// The one ShonRequests, for the track list's "Crowd poll" menu item.
    static ShonRequests* instance();

    /// Start a crowd poll on the guest page between two songs. Guests vote
    /// for 30 seconds; you see the result and still decide.
    void startPoll(const QString& titleA,
            const QString& artistA,
            const QString& titleB,
            const QString& artistB);

    /// For the phone remote.
    const QList<Request>& requestList() const {
        return m_requests;
    }
    QString relayBaseUrl() const {
        return relayUrl();
    }

  signals:
    /// Short text for the bottom-right notice in the main window.
    void notice(const QString& text);
    /// The top bar REQUESTS button: show or hide the side panel.
    void panelVisibilityRequested(bool visible);

  private:
    void setOpen(bool open);
    void newParty();
    void poll();
    void handleRequests(const QByteArray& data);
    void rebuildList();
    void setStatus(const QString& id, const QString& status);
    /// Confirm a pending tip (pins the request) or reject it (back to normal).
    void setTip(const QString& id, bool ok);
    void loadQr();
    void showBigQr();
    void updateHeader();
    void matchInLibrary(Request* pRequest);
    void uploadCatalog();
    QString relayUrl() const;
    QString guestUrl() const;
    QString screenUrl() const;
    QString handle() const;
    void claimHandle();
    void copyLink();
    void saveQrImage();
    void postToRelay(const QString& action,
            QJsonObject body,
            const std::function<void(const QJsonObject&)>& onReply = nullptr);
    void uploadProfile();
    void handlePoll(const QJsonObject& poll);
    void updatePollView();

    UserSettingsPointer m_pConfig;
    Library* m_pLibrary;
    TrackCollectionManager* m_pTrackCollectionManager;
    QNetworkAccessManager* m_pNetwork;
    QTimer* m_pPollTimer;
    QTimer* m_pCatalogTimer = nullptr;
    QString m_catalogSignature;
    int m_catalogCount = 0;
    std::unique_ptr<ControlPushButton> m_pShowWindow;

    QString m_code;
    QString m_key;
    bool m_open = false;
    bool m_online = false;
    bool m_primed = false;
    QList<Request> m_requests;
    QHash<QString, Request> m_matchCache;
    QSet<QString> m_seenIds;
    QSet<QString> m_tipNoticed;
    QString m_lastSignature;

    QPointer<QWidget> m_pWindow;
    QLabel* m_pStatusLabel = nullptr;
    QPushButton* m_pOpenButton = nullptr;
    QSvgWidget* m_pQr = nullptr;
    QLabel* m_pLinkLabel = nullptr;
    QLineEdit* m_pRelayEdit = nullptr;
    QLineEdit* m_pHandleEdit = nullptr;
    QLabel* m_pScreenLabel = nullptr;
    QWidget* m_pSetupBox = nullptr;
    QListWidget* m_pList = nullptr;
    QByteArray m_qrSvg;

    // Tip, follow and booking links.
    QWidget* m_pLinksBox = nullptr;
    QHash<QString, QLineEdit*> m_profileEdits;

    // Crowd poll.
    QJsonObject m_poll;
    qint64 m_pollEndsAt = 0;
    QTimer* m_pPollTick = nullptr;
    QWidget* m_pPollBox = nullptr;
    QLabel* m_pPollLabel = nullptr;
    QPushButton* m_pPollEnd = nullptr;
    QPushButton* m_pPollTop = nullptr;
};

