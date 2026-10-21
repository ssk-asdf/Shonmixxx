#pragma once

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <memory>

#include "preferences/usersettings.h"
#include "track/track_decl.h"

class ControlPushButton;
class Library;
class QLabel;
class QLineEdit;
class QProcess;
class QTimer;
class QWidget;
class TrackCollectionManager;

/// ShonDJ song adding: paste a YouTube link (yt-dlp downloads it) or drop
/// files into Music\Inbox. Each song gets stems made, is added to the
/// library as its stem version and gets analyzed, one at a time in the
/// background. The original file is kept in Music\Originals.
class ShonImport : public QObject {
    Q_OBJECT
  public:
    ShonImport(UserSettingsPointer pConfig,
            Library* pLibrary,
            TrackCollectionManager* pTrackCollectionManager,
            QObject* pParent = nullptr);
    ~ShonImport() override;

  signals:
    void notice(const QString& text);

  private:
    enum class Stage {
        Waiting,
        Downloading,
        Stems,
        Done,
        Failed,
    };
    struct Job {
        int id = 0;
        QString url;   // set for links
        QString file;  // the downloaded or dropped audio file
        QString name;  // shown to the user
        Stage stage = Stage::Waiting;
        int percent = -1;
        QString message;
    };

    void addLink(const QString& url);
    void scanInbox();
    void startNext();
    void runDownload(Job* pJob);
    void runStems(Job* pJob);
    void finishJob(Job* pJob, const QString& stemFile);
    void failJob(Job* pJob, const QString& message);
    void onProcessOutput();
    Job* currentJob();
    QString keepOriginal(const QString& file) const;
    void cleanTags(const TrackPointer& pTrack) const;
    void startProcess(const QString& program, const QStringList& args);

    QString inboxDir() const;
    QString workDir() const;
    QString stemsDir() const;
    QString originalsDir() const;
    QString pythonProgram() const;
    QStringList pythonArgs() const;
    QString stemScript() const;

    void showCard();
    void updateCard();

    UserSettingsPointer m_pConfig;
    Library* m_pLibrary;
    TrackCollectionManager* m_pTrackCollectionManager;

    std::unique_ptr<ControlPushButton> m_pShowCard;
    QTimer* m_pInboxTimer = nullptr;
    QHash<QString, qint64> m_inboxSizes;
    QList<Job> m_jobs;
    int m_nextId = 1;
    int m_currentId = 0;
    QProcess* m_pProcess = nullptr;
    QByteArray m_processOutput;
    QString m_downloadedFile;
    bool m_triedModuleYtDlp = false;

    QPointer<QWidget> m_pCard;
    QLineEdit* m_pLinkEdit = nullptr;
    QLabel* m_pJobsLabel = nullptr;
};
