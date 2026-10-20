#pragma once

#include <QHash>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QWidget>
#include <memory>

#include "preferences/usersettings.h"

class ControlPushButton;
class Library;
class QLabel;
class QLineEdit;
class QListWidget;
class QNetworkAccessManager;
class QPushButton;
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
        QString playedNote;
        double bpm = 0.0;
        int key = 0;
    };

  protected:
    bool eventFilter(QObject* pObject, QEvent* pEvent) override;

  signals:
    /// Short text for the bottom-right notice in the main window.
    void notice(const QString& text);

  private:
    void showWindow(bool show);
    void buildWindow();
    void setOpen(bool open);
    void newParty();
    void poll();
    void handleRequests(const QByteArray& data);
    void rebuildList();
    void setStatus(const QString& id, const QString& status);
    void loadQr();
    void showBigQr();
    void updateHeader();
    void matchInLibrary(Request* pRequest);
    QString relayUrl() const;
    QString guestUrl() const;

    UserSettingsPointer m_pConfig;
    Library* m_pLibrary;
    TrackCollectionManager* m_pTrackCollectionManager;
    QNetworkAccessManager* m_pNetwork;
    QTimer* m_pPollTimer;
    std::unique_ptr<ControlPushButton> m_pShowWindow;

    QString m_code;
    QString m_key;
    bool m_open = false;
    bool m_online = false;
    bool m_primed = false;
    QList<Request> m_requests;
    QHash<QString, Request> m_matchCache;
    QSet<QString> m_seenIds;
    QString m_lastSignature;

    QPointer<QWidget> m_pWindow;
    QLabel* m_pStatusLabel = nullptr;
    QPushButton* m_pOpenButton = nullptr;
    QSvgWidget* m_pQr = nullptr;
    QLabel* m_pLinkLabel = nullptr;
    QLineEdit* m_pRelayEdit = nullptr;
    QWidget* m_pSetupBox = nullptr;
    QListWidget* m_pList = nullptr;
    QByteArray m_qrSvg;
};
