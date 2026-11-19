#pragma once

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QSet>
#include <memory>

#include "preferences/usersettings.h"
#include "track/track_decl.h"
#include "track/trackid.h"

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
class QNetworkAccessManager;

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

  protected:
    /// Drag the card by its title, and close it when you click anywhere
    /// else in ShonDJ.
    bool eventFilter(QObject* pObject, QEvent* pEvent) override;

  private:
    enum class Stage {
        Waiting,     // in line to download (links) or to be added (files)
        Downloading,
        Playable,    // in the library and playable, waiting for stems
        Stems,       // playable, stems being made
        Done,
        Failed,
    };
    struct Job {
        int id = 0;
        QString url;      // set for links
        QString videoId;  // YouTube id, to skip songs you already have
        QString file;     // the downloaded or dropped audio file
        QString name;     // shown to the user
        TrackId trackId;  // the playable version in the library
        QString stemFile; // finished stem file waiting to replace it
        Stage stage = Stage::Waiting;
        int percent = -1;
        QString message;
    };

    void addLink(const QString& url);
    void expandPlaylist(const QString& url);
    void scanInbox();
    /// Keeps both lanes busy: one song downloading while earlier ones get stems.
    void pump();
    void runDownload(Job* pJob);
    void addPlayable(Job* pJob);
    void runStemsBatch();
    void stemReady(Job* pJob);
    void trySwaps();
    bool swapToStems(Job* pJob);
    void failJob(Job* pJob, const QString& message);
    void onDownloadOutput();
    void onStemsOutput();
    Job* jobById(int id);
    QProcess* newProcess();
    bool haveVideo(const QString& videoId);
    void loadHaveIds();
    QString keepOriginal(const QString& file) const;
    void cleanTags(const TrackPointer& pTrack) const;
    /// Look the song up on Apple Music: real title, artist, genre, year and
    /// the album cover. Leaves the cleaned YouTube tags if nothing matches.
    void appleLookup(const TrackPointer& pTrack);
    void appleCover(const TrackPointer& pTrack, const QString& artworkUrl);

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
    QNetworkAccessManager* m_pNetwork = nullptr;

    std::unique_ptr<ControlPushButton> m_pShowCard;
    QTimer* m_pInboxTimer = nullptr;
    QHash<QString, qint64> m_inboxSizes;
    QList<Job> m_jobs;
    int m_nextId = 1;
    // Two lanes: one download at a time, one stems batch at a time.
    QProcess* m_pDownload = nullptr;
    int m_downloadId = 0;
    QByteArray m_downloadOutput;
    QProcess* m_pStems = nullptr;
    QList<int> m_stemIds;
    QByteArray m_stemsOutput;
    QProcess* m_pPlaylist = nullptr;
    QTimer* m_pSwapTimer = nullptr;
    QSet<QString> m_haveIds;
    bool m_haveIdsLoaded = false;
    bool m_triedModuleYtDlp = false;
    // Stems wait while music plays (downloads don't, so requests still land
    // and are playable right away). [Shon],import_stems_while_playing = 1
    // lets them run anyway.
    QTimer* m_pPlayWatch = nullptr;
    bool m_stemsPaused = false;
    bool anyDeckPlaying() const;
    void setStemsPaused(bool paused);

    QPointer<QWidget> m_pCard;
    QLineEdit* m_pLinkEdit = nullptr;
    QLabel* m_pJobsLabel = nullptr;
    QLabel* m_pTitleLabel = nullptr;
    QPoint m_dragOffset;
};
