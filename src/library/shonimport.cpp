#include "library/shonimport.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

#include <QApplication>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QLibrary>
#include <QMimeData>
#include <QMouseEvent>
#include <functional>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
#include <QUrlQuery>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

#include "analyzer/analyzerscheduledtrack.h"
#include "control/controlpushbutton.h"
#include "library/coverart.h"
#include "library/library.h"
#include "library/trackcollectionmanager.h"
#include "mixer/playerinfo.h"
#include "track/cue.h"
#include "moc_shonimport.cpp"
#include "track/track.h"
#include "track/trackref.h"

namespace {

const QString kGroup = QStringLiteral("[Shon]");
const ConfigKey kShowCardKey(kGroup, QStringLiteral("import_window"));
const ConfigKey kPythonKey(kGroup, QStringLiteral("import_python"));
const ConfigKey kScriptKey(kGroup, QStringLiteral("import_stem_script"));

const QStringList kAudioSuffixes = {QStringLiteral("mp3"),
        QStringLiteral("m4a"),
        QStringLiteral("flac"),
        QStringLiteral("wav"),
        QStringLiteral("ogg"),
        QStringLiteral("opus"),
        QStringLiteral("aiff"),
        QStringLiteral("aif")};

QString musicDir() {
    return QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
}

const ConfigKey kYtDlpKey(kGroup, QStringLiteral("import_ytdlp"));
const ConfigKey kStemsWhilePlayingKey(kGroup, QStringLiteral("import_stems_while_playing"));

#ifdef Q_OS_WIN
// Pause or resume a process and everything it started (the py launcher
// starts python, which runs Demucs).
void shonSuspendTree(qint64 rootPid, bool suspend) {
    using NtProcFn = long(__stdcall*)(void*);
    static const auto ntSuspend = reinterpret_cast<NtProcFn>(
            QLibrary::resolve(QStringLiteral("ntdll"), "NtSuspendProcess"));
    static const auto ntResume = reinterpret_cast<NtProcFn>(
            QLibrary::resolve(QStringLiteral("ntdll"), "NtResumeProcess"));
    if (!ntSuspend || !ntResume || rootPid <= 0) {
        return;
    }
    QList<qint64> pids{rootPid};
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry;
        entry.dwSize = sizeof(entry);
        // A few passes catch grandchildren too.
        for (int pass = 0; pass < 3; ++pass) {
            if (Process32FirstW(snapshot, &entry)) {
                do {
                    if (pids.contains(entry.th32ParentProcessID) &&
                            !pids.contains(entry.th32ProcessID)) {
                        pids << entry.th32ProcessID;
                    }
                } while (Process32NextW(snapshot, &entry));
            }
        }
        CloseHandle(snapshot);
    }
    for (qint64 pid : std::as_const(pids)) {
        HANDLE process = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, static_cast<DWORD>(pid));
        if (process) {
            (suspend ? ntSuspend : ntResume)(process);
            CloseHandle(process);
        }
    }
}
#endif
constexpr int kStuckMs = 150000; // no output from yt-dlp this long = stuck

// Your own yt-dlp.exe first (the one you run by hand), then whatever is on PATH.
QString findYtDlp(const UserSettingsPointer& pConfig) {
    const QString configured = pConfig->getValueString(kYtDlpKey);
    if (!configured.isEmpty() && QFileInfo::exists(configured)) {
        return configured;
    }
    const QString home = QDir::homePath();
    const QStringList dirs = {musicDir(),
            musicDir() + QStringLiteral("/Songs"),
            home + QStringLiteral("/Downloads"),
            home + QStringLiteral("/Desktop"),
            home,
            QStringLiteral("C:/dev"),
            QStringLiteral("C:/dev/shondj-stems"),
            QStringLiteral("C:/yt-dlp"),
            QStringLiteral("C:/tools")};
    for (const QString& dir : dirs) {
        const QString exe = dir + QStringLiteral("/yt-dlp.exe");
        if (QFileInfo::exists(exe)) {
            return exe;
        }
    }
    const QString onPath = QStandardPaths::findExecutable(QStringLiteral("yt-dlp"));
    return onPath.isEmpty() ? QStringLiteral("yt-dlp") : onPath;
}

// The drop box on the Add songs card: songs from Explorer or links from the browser.
class ShonDropZone : public QLabel {
  public:
    explicit ShonDropZone(QWidget* pParent)
            : QLabel(pParent) {
        setObjectName(QStringLiteral("ShonDropZone"));
        setAcceptDrops(true);
        setAlignment(Qt::AlignCenter);
        setMinimumHeight(96);
        setText(QStringLiteral("Drop songs or YouTube links here"));
        setHover(false);
    }
    std::function<void(const QStringList& files, const QStringList& links)> onDrop;

  protected:
    void dragEnterEvent(QDragEnterEvent* pEvent) override {
        if (pEvent->mimeData()->hasUrls() || pEvent->mimeData()->hasText()) {
            pEvent->acceptProposedAction();
            setHover(true);
        }
    }
    void dragLeaveEvent(QDragLeaveEvent* pEvent) override {
        QLabel::dragLeaveEvent(pEvent);
        setHover(false);
    }
    void dropEvent(QDropEvent* pEvent) override {
        setHover(false);
        QStringList files;
        QStringList links;
        const QMimeData* pMime = pEvent->mimeData();
        for (const QUrl& url : pMime->urls()) {
            if (url.isLocalFile()) {
                files << url.toLocalFile();
            } else if (url.scheme().startsWith(QLatin1String("http"))) {
                links << url.toString();
            }
        }
        if (files.isEmpty() && links.isEmpty() && pMime->hasText()) {
            const QStringList words = pMime->text().split(
                    QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            for (const QString& word : words) {
                if (word.startsWith(QLatin1String("http"))) {
                    links << word;
                }
            }
        }
        pEvent->acceptProposedAction();
        if (onDrop) {
            onDrop(files, links);
        }
    }

  private:
    void setHover(bool hover) {
        setStyleSheet(hover
                        ? QStringLiteral("QLabel { color: #ffffff; font-size: 13px; "
                                         "background: rgba(47, 107, 255, 40); "
                                         "border: 2px dashed #2f6bff; border-radius: 10px; }")
                        : QStringLiteral("QLabel { color: #8a8a92; font-size: 13px; "
                                         "background: #111113; "
                                         "border: 2px dashed #3a3a40; border-radius: 10px; }"));
    }
};

QString lastLine(const QString& output) {
    const QStringList lines = output.split(QRegularExpression(QStringLiteral("[\\r\\n]+")),
            Qt::SkipEmptyParts);
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
        const QString line = it->trimmed();
        if (!line.isEmpty() && !line.startsWith(QStringLiteral("SHONFILE:"))) {
            return line.left(90);
        }
    }
    return QString();
}

} // namespace

ShonImport::ShonImport(UserSettingsPointer pConfig,
        Library* pLibrary,
        TrackCollectionManager* pTrackCollectionManager,
        QObject* pParent)
        : QObject(pParent),
          m_pConfig(pConfig),
          m_pLibrary(pLibrary),
          m_pTrackCollectionManager(pTrackCollectionManager) {
    QDir().mkpath(inboxDir());
    QDir().mkpath(workDir());
    QDir().mkpath(stemsDir());
    QDir().mkpath(originalsDir());

    m_pShowCard = std::make_unique<ControlPushButton>(kShowCardKey);
    connect(m_pShowCard.get(), &ControlObject::valueChanged, this, [this](double value) {
        if (value > 0.0) {
            showCard();
        }
    });

    // Files dropped into Music\Inbox (by yt-dlp or by hand) are picked up
    // once they stop growing.
    m_pInboxTimer = new QTimer(this);
    m_pInboxTimer->setInterval(4000);
    connect(m_pInboxTimer, &QTimer::timeout, this, &ShonImport::scanInbox);
    m_pInboxTimer->start();

    // Stems pause while you play and pick up when the music stops.
    m_pPlayWatch = new QTimer(this);
    m_pPlayWatch->setInterval(1000);
    connect(m_pPlayWatch, &QTimer::timeout, this, [this]() {
        const bool hold = anyDeckPlaying() && !m_pConfig->getValue(kStemsWhilePlayingKey, false);
        setStemsPaused(hold);
        if (!hold && !m_pStems) {
            pump(); // a batch that waited for the music to stop
        }
    });
    m_pPlayWatch->start();
}

bool ShonImport::anyDeckPlaying() const {
    for (int deck = 1; deck <= 4; ++deck) {
        const ControlObject* pPlay = ControlObject::getControl(
                ConfigKey(QStringLiteral("[Channel%1]").arg(deck), QStringLiteral("play")),
                ControlFlag::NoAssertIfMissing);
        if (pPlay && pPlay->get() > 0.0) {
            return true;
        }
    }
    return false;
}

void ShonImport::setStemsPaused(bool paused) {
    if (paused == m_stemsPaused) {
        return;
    }
    m_stemsPaused = paused;
#ifdef Q_OS_WIN
    if (m_pStems) {
        shonSuspendTree(m_pStems->processId(), paused);
    }
#endif
    for (Job& job : m_jobs) {
        if (job.stage == Stage::Stems && job.message.isEmpty() && paused) {
            job.message = QStringLiteral("playable, stems wait while you play");
        } else if (job.stage == Stage::Stems && !paused &&
                job.message == QStringLiteral("playable, stems wait while you play")) {
            job.message.clear();
        }
    }
    updateCard();
}

ShonImport::~ShonImport() {
#ifdef Q_OS_WIN
    if (m_pStems && m_stemsPaused) {
        shonSuspendTree(m_pStems->processId(), false); // let it be killed cleanly
    }
#endif
    for (QProcess* pProcess : {m_pDownload, m_pStems, m_pPlaylist}) {
        if (pProcess) {
            pProcess->disconnect(this);
            pProcess->kill();
            pProcess->waitForFinished(2000);
        }
    }
    if (m_pCard) {
        m_pCard->deleteLater();
    }
}

QString ShonImport::inboxDir() const {
    return musicDir() + QStringLiteral("/Inbox");
}
QString ShonImport::workDir() const {
    return inboxDir() + QStringLiteral("/.downloading");
}
QString ShonImport::stemsDir() const {
    return musicDir() + QStringLiteral("/Stems");
}
QString ShonImport::originalsDir() const {
    return musicDir() + QStringLiteral("/Originals");
}
QString ShonImport::pythonProgram() const {
    const QString value = m_pConfig->getValueString(kPythonKey);
    return value.isEmpty() ? QStringLiteral("py") : value;
}
QStringList ShonImport::pythonArgs() const {
    // The Windows "py" launcher picks the Python that has Demucs installed.
    return pythonProgram() == QLatin1String("py") ? QStringList{QStringLiteral("-3.11")}
                                                   : QStringList();
}
QString ShonImport::stemScript() const {
    const QString value = m_pConfig->getValueString(kScriptKey);
    return value.isEmpty() ? QStringLiteral("C:/dev/shondj-stems/shon_stems.py") : value;
}

// ---------------------------------------------------------------- queue

namespace {

// The 11-character YouTube id in a link, or empty.
QString youtubeId(const QString& url) {
    static const QRegularExpression kId(
            QStringLiteral("(?:[?&]v=|youtu\\.be/|/shorts/)([A-Za-z0-9_-]{11})"));
    const auto match = kId.match(url);
    return match.hasMatch() ? match.captured(1) : QString();
}

// A playlist link: /playlist?list=..., or a list= link with no song in it.
bool isPlaylistLink(const QString& url) {
    return url.contains(QLatin1String("list=")) &&
            (url.contains(QLatin1String("/playlist")) || youtubeId(url).isEmpty());
}

} // namespace

void ShonImport::loadHaveIds() {
    if (m_haveIdsLoaded) {
        return;
    }
    m_haveIdsLoaded = true;
    // yt-dlp writes the video link into each file's tags. Read the start of
    // every song you have (MP3 tags sit at the front) to know what you own.
    static const QRegularExpression kUrl(
            QStringLiteral("(?:youtube\\.com/watch\\?v=|youtu\\.be/)([A-Za-z0-9_-]{11})"));
    const QStringList dirs = {originalsDir(), musicDir() + QStringLiteral("/Songs"), inboxDir()};
    for (const QString& dir : dirs) {
        const QFileInfoList files = QDir(dir).entryInfoList(QDir::Files);
        for (const QFileInfo& info : files) {
            if (!kAudioSuffixes.contains(info.suffix().toLower())) {
                continue;
            }
            QFile file(info.absoluteFilePath());
            if (!file.open(QIODevice::ReadOnly)) {
                continue;
            }
            const QString head = QString::fromLatin1(file.read(600000));
            const auto match = kUrl.match(head);
            if (match.hasMatch()) {
                m_haveIds.insert(match.captured(1));
            }
        }
    }
}

bool ShonImport::haveVideo(const QString& videoId) {
    if (videoId.isEmpty()) {
        return false;
    }
    loadHaveIds();
    if (m_haveIds.contains(videoId)) {
        return true;
    }
    for (const Job& job : std::as_const(m_jobs)) {
        if (job.videoId == videoId && job.stage != Stage::Failed) {
            return true;
        }
    }
    return false;
}

void ShonImport::addLink(const QString& url) {
    const QString link = url.trimmed();
    if (!link.startsWith(QLatin1String("http"))) {
        emit notice(QStringLiteral("Add songs: paste a full link (https://...)"));
        return;
    }
    if (isPlaylistLink(link)) {
        expandPlaylist(link);
        return;
    }
    const QString videoId = youtubeId(link);
    if (haveVideo(videoId)) {
        emit notice(QStringLiteral("Add songs: you already have that one"));
        return;
    }
    Job job;
    job.id = m_nextId++;
    job.url = link;
    job.videoId = videoId;
    job.name = link;
    m_jobs.append(job);
    emit notice(QStringLiteral("Add songs: link added"));
    updateCard();
    pump();
}

void ShonImport::expandPlaylist(const QString& url) {
    if (m_pPlaylist) {
        emit notice(QStringLiteral("Add songs: still reading the last playlist"));
        return;
    }
    emit notice(QStringLiteral("Add songs: reading the playlist..."));
    QStringList args = {QStringLiteral("--flat-playlist"),
            QStringLiteral("--no-warnings"),
            QStringLiteral("--print"),
            QStringLiteral("%(id)s\t%(title)s"),
            url};
    QString program = findYtDlp(m_pConfig);
    if (m_triedModuleYtDlp) {
        args = pythonArgs() + QStringList{QStringLiteral("-m"), QStringLiteral("yt_dlp")} + args;
        program = pythonProgram();
    }
    m_pPlaylist = newProcess();
    QProcess* pProcess = m_pPlaylist;
    connect(pProcess,
            &QProcess::finished,
            this,
            [this, pProcess](int exitCode, QProcess::ExitStatus) {
                const QString output = QString::fromUtf8(pProcess->readAll());
                pProcess->deleteLater();
                m_pPlaylist = nullptr;
                int added = 0;
                int skipped = 0;
                const QStringList lines = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
                for (const QString& line : lines) {
                    const QString id = line.section(QLatin1Char('\t'), 0, 0).trimmed();
                    const QString title = line.section(QLatin1Char('\t'), 1).trimmed();
                    if (id.size() != 11) {
                        continue;
                    }
                    if (haveVideo(id)) {
                        ++skipped;
                        continue;
                    }
                    Job job;
                    job.id = m_nextId++;
                    job.url = QStringLiteral("https://www.youtube.com/watch?v=") + id;
                    job.videoId = id;
                    job.name = title.isEmpty() ? job.url : title;
                    m_jobs.append(job);
                    ++added;
                }
                if (added == 0 && skipped == 0) {
                    emit notice(exitCode == 0
                                    ? QStringLiteral("Add songs: that playlist looks empty")
                                    : QStringLiteral("Add songs: could not read the playlist (%1)")
                                              .arg(lastLine(output)));
                } else {
                    emit notice(QStringLiteral("Add songs: %1 new from the playlist, %2 you already have")
                                        .arg(added)
                                        .arg(skipped));
                }
                updateCard();
                pump();
            });
    pProcess->start(program, args);
}

void ShonImport::scanInbox() {
    const QFileInfoList files = QDir(inboxDir()).entryInfoList(QDir::Files, QDir::Time | QDir::Reversed);
    QHash<QString, qint64> sizes;
    for (const QFileInfo& info : files) {
        const QString path = info.absoluteFilePath();
        const QString name = info.fileName().toLower();
        if (!kAudioSuffixes.contains(info.suffix().toLower()) || name.contains(QLatin1String(".part")) ||
                name.contains(QLatin1String(".ytdl")) || name.contains(QLatin1String(".temp"))) {
            continue;
        }
        bool queued = false;
        for (const Job& job : std::as_const(m_jobs)) {
            if (job.file == path && job.stage != Stage::Failed) {
                queued = true;
            }
        }
        if (queued) {
            continue;
        }
        sizes.insert(path, info.size());
        // Wait until the size stays the same between two scans.
        if (m_inboxSizes.value(path, -1) != info.size() || info.size() == 0) {
            continue;
        }
        Job job;
        job.id = m_nextId++;
        job.file = path;
        job.name = info.completeBaseName();
        m_jobs.append(job);
        emit notice(QStringLiteral("Add songs: found %1").arg(job.name));
    }
    m_inboxSizes = sizes;
    updateCard();
    pump();
}

ShonImport::Job* ShonImport::jobById(int id) {
    for (Job& job : m_jobs) {
        if (job.id == id) {
            return &job;
        }
    }
    return nullptr;
}

void ShonImport::pump() {
    // Dropped files skip the download and become playable right away.
    for (Job& job : m_jobs) {
        if (job.stage == Stage::Waiting && !job.file.isEmpty() && job.url.isEmpty()) {
            addPlayable(&job);
        }
    }
    // Download lane: one link at a time.
    if (!m_pDownload) {
        for (Job& job : m_jobs) {
            if (job.stage == Stage::Waiting && !job.url.isEmpty()) {
                runDownload(&job);
                break;
            }
        }
    }
    // Stems lane: everything playable so far, in one run, so the stem AI
    // loads once instead of once per song. Not while music plays.
    if (!m_pStems && !m_stemsPaused) {
        runStemsBatch();
    }
}

QProcess* ShonImport::newProcess() {
    auto* pProcess = new QProcess(this);
    pProcess->setProcessChannelMode(QProcess::MergedChannels);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    pProcess->setProcessEnvironment(env);
#ifdef Q_OS_WIN
    // Below normal priority so the music never stutters, and no console window.
    pProcess->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* pArgs) {
        pArgs->flags |= 0x00004000; // BELOW_NORMAL_PRIORITY_CLASS
        pArgs->flags |= 0x08000000; // CREATE_NO_WINDOW
    });
#endif
    return pProcess;
}

namespace {

int lastPercent(const QByteArray& chunk) {
    // Progress shows up as "45.3%" in yt-dlp and in Demucs' progress bar.
    static const QRegularExpression kPercent(QStringLiteral("(\\d{1,3})(?:\\.\\d+)?%"));
    auto it = kPercent.globalMatch(QString::fromUtf8(chunk));
    int percent = -1;
    while (it.hasNext()) {
        percent = it.next().captured(1).toInt();
    }
    return percent <= 100 ? percent : -1;
}

} // namespace

void ShonImport::onDownloadOutput() {
    if (!m_pDownload) {
        return;
    }
    const QByteArray chunk = m_pDownload->readAll();
    m_downloadOutput += chunk;
    if (auto* pWatchdog = m_pDownload->findChild<QTimer*>(QStringLiteral("shonWatchdog"))) {
        pWatchdog->start();
    }
    if (m_downloadOutput.size() > 200000) {
        m_downloadOutput = m_downloadOutput.right(100000);
    }
    Job* pJob = jobById(m_downloadId);
    const int percent = lastPercent(chunk);
    if (pJob && percent >= 0 && percent != pJob->percent) {
        pJob->percent = percent;
        updateCard();
    }
}

void ShonImport::onStemsOutput() {
    if (!m_pStems) {
        return;
    }
    const QByteArray chunk = m_pStems->readAll();
    m_stemsOutput += chunk;
    if (m_stemsOutput.size() > 200000) {
        m_stemsOutput = m_stemsOutput.right(100000);
    }
    // The stem maker prints "  OK  <file name>" as each song finishes.
    static const QRegularExpression kDone(QStringLiteral("^\\s*(OK|FAIL)\\s+(.+?)(?::\\s.*)?$"),
            QRegularExpression::MultilineOption);
    auto it = kDone.globalMatch(QString::fromUtf8(chunk));
    while (it.hasNext()) {
        const auto match = it.next();
        const QString name = match.captured(2).trimmed();
        for (int id : std::as_const(m_stemIds)) {
            Job* pJob = jobById(id);
            if (!pJob || pJob->stage != Stage::Stems ||
                    QFileInfo(pJob->file).fileName() != name) {
                continue;
            }
            if (match.captured(1) == QLatin1String("OK")) {
                stemReady(pJob);
            } else {
                pJob->stage = Stage::Done;
                pJob->message = QStringLiteral("added, stems failed");
            }
        }
    }
    // Progress on the first song still in this batch.
    const int percent = lastPercent(chunk);
    if (percent >= 0) {
        for (int id : std::as_const(m_stemIds)) {
            Job* pJob = jobById(id);
            if (pJob && pJob->stage == Stage::Stems) {
                pJob->percent = percent;
                break;
            }
        }
    }
    updateCard();
}

// ---------------------------------------------------------------- steps

void ShonImport::runDownload(Job* pJob) {
    pJob->stage = Stage::Downloading;
    pJob->percent = -1;
    m_downloadId = pJob->id;
    m_downloadOutput.clear();
    updateCard();
    QStringList args = {QStringLiteral("-x"),
            QStringLiteral("--audio-format"),
            QStringLiteral("mp3"),
            QStringLiteral("--audio-quality"),
            QStringLiteral("0"),
            QStringLiteral("--embed-metadata"),
            QStringLiteral("--embed-thumbnail"),
            QStringLiteral("--no-playlist"),
            // Cut the talking, skits and outros out of music videos, using
            // SponsorBlock's crowd-marked "non-music" parts.
            QStringLiteral("--sponsorblock-remove"),
            QStringLiteral("music_offtopic"),
            QStringLiteral("--newline"),
            QStringLiteral("--progress"),
            QStringLiteral("--no-simulate"),
            QStringLiteral("-P"),
            workDir(),
            QStringLiteral("-o"),
            QStringLiteral("%(artist,uploader)s - %(track,title)s.%(ext)s"),
            QStringLiteral("--print"),
            QStringLiteral("after_move:SHONFILE:%(filepath)s"),
            pJob->url};
    QString program = findYtDlp(m_pConfig);
    if (m_triedModuleYtDlp) {
        // yt-dlp installed with pip instead of as a program.
        args = pythonArgs() + QStringList{QStringLiteral("-m"), QStringLiteral("yt_dlp")} + args;
        program = pythonProgram();
    }
    m_pDownload = newProcess();
    QProcess* pProcess = m_pDownload;
    connect(pProcess, &QProcess::readyRead, this, &ShonImport::onDownloadOutput);
    // Watchdog: if yt-dlp prints nothing for a while it is stuck (usually an
    // old yt-dlp after a YouTube change). Output restarts the clock.
    auto* pWatchdog = new QTimer(pProcess);
    pWatchdog->setObjectName(QStringLiteral("shonWatchdog"));
    pWatchdog->setSingleShot(true);
    pWatchdog->setInterval(kStuckMs);
    connect(pWatchdog, &QTimer::timeout, pProcess, [pProcess]() {
        pProcess->setProperty("shonStuck", true);
        pProcess->kill();
    });
    pWatchdog->start();
    connect(pProcess, &QProcess::errorOccurred, this, [this, pProcess](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) {
            return;
        }
        pProcess->deleteLater();
        m_pDownload = nullptr;
        Job* pJob = jobById(m_downloadId);
        if (!pJob) {
            pump();
            return;
        }
        if (!m_triedModuleYtDlp) {
            m_triedModuleYtDlp = true;
            pJob->stage = Stage::Waiting; // try again with Python's yt-dlp
            pump();
        } else {
            failJob(pJob, QStringLiteral("yt-dlp not found"));
        }
    });
    connect(pProcess,
            &QProcess::finished,
            this,
            [this, pProcess](int exitCode, QProcess::ExitStatus status) {
                const QString output = QString::fromUtf8(m_downloadOutput);
                const bool stuck = pProcess->property("shonStuck").toBool();
                pProcess->deleteLater();
                m_pDownload = nullptr;
                Job* pJob = jobById(m_downloadId);
                if (!pJob) {
                    pump();
                    return;
                }
                static const QRegularExpression kFile(QStringLiteral("SHONFILE:(.+)"));
                QString file;
                auto it = kFile.globalMatch(output);
                while (it.hasNext()) {
                    file = it.next().captured(1).trimmed();
                }
                if (status != QProcess::NormalExit || exitCode != 0 || file.isEmpty() ||
                        !QFileInfo::exists(file)) {
                    if (stuck) {
                        failJob(pJob,
                                QStringLiteral("stuck, update yt-dlp (yt-dlp -U) and try again"));
                    } else {
                        const QString why = lastLine(output);
                        failJob(pJob,
                                why.isEmpty() ? QStringLiteral("download failed")
                                              : QStringLiteral("failed: ") + why);
                    }
                    return;
                }
                pJob->file = QDir::fromNativeSeparators(file);
                pJob->name = QFileInfo(file).completeBaseName();
                addPlayable(pJob);
                pump();
            });
    pProcess->start(program, args);
}

void ShonImport::addPlayable(Job* pJob) {
    // Into the library right away so you can play it while stems are made.
    const QString original = keepOriginal(pJob->file);
    pJob->file = original;
    TrackPointer pTrack =
            m_pTrackCollectionManager->getOrAddTrack(TrackRef::fromFilePath(original));
    if (!pTrack) {
        failJob(pJob, QStringLiteral("could not add to the library"));
        return;
    }
    cleanTags(pTrack);
    appleLookup(pTrack);
    emit m_pLibrary->analyzeTracks({AnalyzerScheduledTrack(pTrack->getId())});
    pJob->trackId = pTrack->getId();
    pJob->stage = Stage::Playable;
    pJob->percent = -1;
    if (!pJob->videoId.isEmpty()) {
        m_haveIds.insert(pJob->videoId);
    }
    pJob->name = pTrack->getArtist().isEmpty()
            ? pTrack->getTitle()
            : pTrack->getArtist() + QStringLiteral(" - ") + pTrack->getTitle();
    emit notice(QStringLiteral("Ready to play: %1 (stems coming)").arg(pJob->name));
    updateCard();
}

void ShonImport::runStemsBatch() {
    QStringList files;
    m_stemIds.clear();
    for (Job& job : m_jobs) {
        if (job.stage == Stage::Playable) {
            job.stage = Stage::Stems;
            job.percent = -1;
            m_stemIds << job.id;
            files << QDir::toNativeSeparators(job.file);
        }
    }
    if (files.isEmpty()) {
        return;
    }
    m_stemsOutput.clear();
    updateCard();
    const QStringList args = pythonArgs() + QStringList{stemScript()} + files +
            QStringList{QStringLiteral("--out"), QDir::toNativeSeparators(stemsDir())};
    m_pStems = newProcess();
    QProcess* pProcess = m_pStems;
    connect(pProcess, &QProcess::readyRead, this, &ShonImport::onStemsOutput);
    const auto finishBatch = [this, pProcess]() {
        pProcess->deleteLater();
        m_pStems = nullptr;
        // Anything not reported yet: use its stem file if one was made.
        for (int id : std::as_const(m_stemIds)) {
            Job* pJob = jobById(id);
            if (!pJob || pJob->stage != Stage::Stems) {
                continue;
            }
            const QString stemFile = stemsDir() + QLatin1Char('/') +
                    QFileInfo(pJob->file).completeBaseName() + QStringLiteral(".stem.mp4");
            if (QFileInfo::exists(stemFile)) {
                stemReady(pJob);
            } else {
                // No Python or Demucs: the song stays as it is, still playable.
                pJob->stage = Stage::Done;
                pJob->message = QStringLiteral("added without stems");
            }
        }
        m_stemIds.clear();
        updateCard();
        pump(); // songs that became playable meanwhile get the next batch
    };
    connect(pProcess, &QProcess::errorOccurred, this, [finishBatch](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            finishBatch();
        }
    });
    connect(pProcess, &QProcess::finished, this, [finishBatch](int, QProcess::ExitStatus) {
        finishBatch();
    });
    emit notice(QStringLiteral("Making stems for %1 song(s)").arg(files.size()));
    pProcess->start(pythonProgram(), args);
}

void ShonImport::stemReady(Job* pJob) {
    pJob->stemFile = stemsDir() + QLatin1Char('/') +
            QFileInfo(pJob->file).completeBaseName() + QStringLiteral(".stem.mp4");
    if (!swapToStems(pJob)) {
        // On a deck right now: swap once it's unloaded.
        pJob->stage = Stage::Stems;
        pJob->message = QStringLiteral("stems ready, swaps in after you unload it");
        pJob->percent = 100;
        if (!m_pSwapTimer) {
            m_pSwapTimer = new QTimer(this);
            m_pSwapTimer->setInterval(5000);
            connect(m_pSwapTimer, &QTimer::timeout, this, &ShonImport::trySwaps);
        }
        m_pSwapTimer->start();
    }
    updateCard();
}

void ShonImport::trySwaps() {
    bool waiting = false;
    for (Job& job : m_jobs) {
        if (job.stage == Stage::Stems && !job.stemFile.isEmpty() && !m_stemIds.contains(job.id)) {
            if (!swapToStems(&job)) {
                waiting = true;
            }
        }
    }
    if (!waiting && m_pSwapTimer) {
        m_pSwapTimer->stop();
    }
    updateCard();
}

bool ShonImport::swapToStems(Job* pJob) {
    TrackPointer pOld = m_pTrackCollectionManager->getTrackById(pJob->trackId);
    if (pOld && PlayerInfo::instance().isTrackLoaded(pOld)) {
        return false; // never pull a song out from under a deck
    }
    TrackPointer pNew =
            m_pTrackCollectionManager->getOrAddTrack(TrackRef::fromFilePath(pJob->stemFile));
    if (!pNew) {
        pJob->stage = Stage::Done;
        pJob->message = QStringLiteral("added, stems could not be swapped in");
        return true;
    }
    if (pOld && pOld != pNew) {
        // Everything you did to the song while stems were made comes along.
        pNew->setTitle(pOld->getTitle());
        pNew->setArtist(pOld->getArtist());
        if (!pOld->getGenre().isEmpty()) {
            pNew->updateGenre(pOld->getGenre());
        }
        pNew->setYear(pOld->getYear());
        pNew->setColor(pOld->getColor());
        pNew->setRating(pOld->getRating());
        pNew->setComment(pOld->getComment());
        pNew->setPlayCounter(pOld->getPlayCounter());
        pNew->setCoverInfo(pOld->getCoverInfo());
        const auto oldRate = pOld->getSampleRate();
        const auto newRate = pNew->getSampleRate();
        if (oldRate.isValid() && newRate.isValid()) {
            QList<CuePointer> cues;
            for (const CuePointer& pCue : pOld->getCuePoints()) {
                if (pCue) {
                    cues << CuePointer(new Cue(pCue->getCueInfo(oldRate), newRate, true));
                }
            }
            if (!cues.isEmpty()) {
                pNew->setCuePoints(cues);
            }
            if (const mixxx::BeatsPointer pBeats = pOld->getBeats();
                    pBeats && oldRate == newRate) {
                pNew->trySetBeats(pBeats);
            }
        }
        pNew->setKeys(pOld->getKeys());
        m_pTrackCollectionManager->hideTracks({pOld->getId()});
    }
    emit m_pLibrary->analyzeTracks({AnalyzerScheduledTrack(pNew->getId())});
    pJob->trackId = pNew->getId();
    pJob->stage = Stage::Done;
    pJob->percent = 100;
    pJob->message = QStringLiteral("stems ready");
    emit notice(QStringLiteral("Stems ready: %1").arg(pJob->name));
    return true;
}

QString ShonImport::keepOriginal(const QString& file) const {
    const QFileInfo info(file);
    QString target = originalsDir() + QLatin1Char('/') + info.fileName();
    for (int n = 2; QFileInfo::exists(target); ++n) {
        target = originalsDir() + QLatin1Char('/') + info.completeBaseName() +
                QStringLiteral(" (%1).").arg(n) + info.suffix();
    }
    return QFile::rename(file, target) ? target : file;
}

void ShonImport::failJob(Job* pJob, const QString& message) {
    pJob->stage = Stage::Failed;
    pJob->message = message;
    emit notice(QStringLiteral("Add songs: %1 (%2)").arg(pJob->name, message));
    updateCard();
    pump();
}

void ShonImport::cleanTags(const TrackPointer& pTrack) const {
    // YouTube titles look like "Artist - Song (Official Video) [HD]".
    static const QRegularExpression kJunk(QStringLiteral(
            "\\s*[\\(\\[「【][^\\)\\]」】]*(official|video|audio|lyric|visuali[sz]er|"
            "\\bhd\\b|4k|\\bmv\\b|explicit|clean|remaster)[^\\)\\]」】]*[\\)\\]」】]"),
            QRegularExpression::CaseInsensitiveOption);
    QString title = pTrack->getTitle();
    QString artist = pTrack->getArtist();
    if (title.isEmpty()) {
        return;
    }
    title.replace(kJunk, QString());
    const int dash = title.indexOf(QRegularExpression(QStringLiteral("\\s[-\\x{2013}\\x{2014}]\\s")));
    if (dash > 0) {
        artist = title.left(dash).trimmed();
        title = title.mid(dash + 3).trimmed();
    }
    artist.remove(QRegularExpression(QStringLiteral("\\s*-\\s*Topic$|VEVO$"),
            QRegularExpression::CaseInsensitiveOption));
    title = title.simplified();
    artist = artist.simplified();
    if (!title.isEmpty() && title != pTrack->getTitle()) {
        pTrack->setTitle(title);
    }
    if (!artist.isEmpty() && artist != pTrack->getArtist()) {
        pTrack->setArtist(artist);
    }
}

namespace {

// Lowercase words without "(Official Video)", "feat. X" and punctuation.
QSet<QString> shonWords(QString text) {
    static const QRegularExpression kBrackets(QStringLiteral("[\\(\\[][^\\)\\]]*[\\)\\]]"));
    static const QRegularExpression kFeat(QStringLiteral("\\b(ft|feat|featuring)\\b.*"),
            QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression kNonWord(QStringLiteral("[^\\p{L}\\p{N}]+"));
    text = text.toLower();
    text.remove(kBrackets);
    text.remove(kFeat);
    text.replace(kNonWord, QStringLiteral(" "));
    const QStringList list = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    return QSet<QString>(list.cbegin(), list.cend());
}

double shonOverlap(const QSet<QString>& a, const QSet<QString>& b) {
    if (a.isEmpty() || b.isEmpty()) {
        return 0.0;
    }
    const int shared = static_cast<int>(QSet<QString>(a).intersect(b).size());
    return static_cast<double>(shared) / std::max(a.size(), b.size());
}

QString shonMainArtist(const QString& artist) {
    static const QRegularExpression kSplit(
            QStringLiteral(",|&|\\sx\\s|\\bft\\b|\\bfeat\\b|\\("),
            QRegularExpression::CaseInsensitiveOption);
    return artist.split(kSplit).value(0).trimmed();
}

} // namespace

void ShonImport::appleLookup(const TrackPointer& pTrack) {
    if (!pTrack || pTrack->getTitle().isEmpty()) {
        return;
    }
    if (!m_pNetwork) {
        m_pNetwork = new QNetworkAccessManager(this);
    }
    const QString artist = shonMainArtist(pTrack->getArtist());
    QUrl url(QStringLiteral("https://itunes.apple.com/search"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("term"), artist + QLatin1Char(' ') + pTrack->getTitle());
    query.addQueryItem(QStringLiteral("entity"), QStringLiteral("song"));
    query.addQueryItem(QStringLiteral("limit"), QStringLiteral("15"));
    query.addQueryItem(QStringLiteral("country"), QStringLiteral("US"));
    url.setQuery(query);

    QNetworkReply* pReply = m_pNetwork->get(QNetworkRequest(url));
    connect(pReply, &QNetworkReply::finished, this, [this, pReply, pTrack]() {
        pReply->deleteLater();
        if (pReply->error() != QNetworkReply::NoError) {
            return; // offline: keep the cleaned YouTube tags
        }
        const QJsonArray results =
                QJsonDocument::fromJson(pReply->readAll()).object().value(QStringLiteral("results")).toArray();
        const QSet<QString> wantTitle = shonWords(pTrack->getTitle());
        const QSet<QString> wantArtist = shonWords(shonMainArtist(pTrack->getArtist()));

        // Score every result, then take the earliest release among the best
        // ones so remasters and compilations don't win.
        double top = 0.0;
        QList<QPair<double, QJsonObject>> scored;
        for (const QJsonValue& value : results) {
            const QJsonObject r = value.toObject();
            const double score =
                    0.6 * shonOverlap(wantTitle, shonWords(r.value(QStringLiteral("trackName")).toString())) +
                    0.4 * shonOverlap(wantArtist, shonWords(r.value(QStringLiteral("artistName")).toString()));
            scored.append({score, r});
            top = std::max(top, score);
        }
        if (top < 0.6) {
            return; // not confident, leave it as is
        }
        QJsonObject best;
        QString bestDate = QStringLiteral("9999");
        for (const auto& entry : std::as_const(scored)) {
            const QString date = entry.second.value(QStringLiteral("releaseDate")).toString();
            if (entry.first >= top - 0.05 && date < bestDate) {
                best = entry.second;
                bestDate = date;
            }
        }
        const QString title = best.value(QStringLiteral("trackName")).toString();
        const QString artist = best.value(QStringLiteral("artistName")).toString();
        const QString genre = best.value(QStringLiteral("primaryGenreName")).toString();
        const QString year = bestDate.left(4);
        if (!title.isEmpty()) {
            pTrack->setTitle(title);
        }
        if (!artist.isEmpty()) {
            pTrack->setArtist(artist);
        }
        if (!genre.isEmpty()) {
            pTrack->updateGenre(genre);
        }
        if (year != QStringLiteral("9999")) {
            pTrack->setYear(year);
        }
        emit notice(QStringLiteral("Apple Music: %1 - %2 (%3, %4)").arg(artist, title, genre, year));
        appleCover(pTrack, best.value(QStringLiteral("artworkUrl100")).toString());
    });
}

void ShonImport::appleCover(const TrackPointer& pTrack, const QString& artworkUrl) {
    if (artworkUrl.isEmpty()) {
        return;
    }
    QString big = artworkUrl;
    big.replace(QRegularExpression(QStringLiteral("\\d+x\\d+bb")), QStringLiteral("1000x1000bb"));
    QNetworkReply* pReply = m_pNetwork->get(QNetworkRequest(QUrl(big)));
    connect(pReply, &QNetworkReply::finished, this, [pReply, pTrack]() {
        pReply->deleteLater();
        if (pReply->error() != QNetworkReply::NoError) {
            return;
        }
        const QByteArray data = pReply->readAll();
        QImage image;
        if (!image.loadFromData(data)) {
            return;
        }
        // Saved next to your music so the stem file itself is never rewritten.
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::MusicLocation) +
                QStringLiteral("/Covers");
        QDir().mkpath(dir);
        QString name = pTrack->getArtist() + QStringLiteral(" - ") + pTrack->getTitle();
        name.remove(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*]")));
        const QString path = dir + QLatin1Char('/') +
                QString::number(pTrack->getId().toVariant().toLongLong()) +
                QLatin1Char(' ') + name.left(80).trimmed() + QStringLiteral(".jpg");
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            return;
        }
        file.write(data);
        file.close();
        CoverInfoRelative cover;
        cover.source = CoverInfo::USER_SELECTED;
        cover.type = CoverInfo::FILE;
        cover.coverLocation = path;
        cover.setImageDigest(image);
        pTrack->setCoverInfo(cover);
    });
}

// ---------------------------------------------------------------- card

void ShonImport::showCard() {
    QWidget* pParent = QApplication::activeWindow();
    const bool firstTime = !m_pCard;
    if (!m_pCard) {
        m_pCard = new QWidget(pParent, Qt::Tool | Qt::FramelessWindowHint);
        m_pCard->setObjectName(QStringLiteral("ShonImportCard"));
        m_pCard->setMinimumWidth(460);
        m_pCard->setStyleSheet(QStringLiteral(
                "#ShonImportCard { background: #17171a; border: 1px solid #2a2a2e; "
                "border-radius: 14px; }"
                "QLabel { color: #e6e6ea; font-size: 13px; }"
                "QLabel#ShonImportTitle { font-size: 16px; font-weight: 700; color: #ffffff; }"
                "QLabel#ShonImportDim { color: #8a8a92; font-size: 12px; }"
                "QLineEdit { background: #111113; color: #ffffff; border: 1px solid #2a2a2e; "
                "border-radius: 8px; padding: 8px 10px; font-size: 13px; }"
                "QLineEdit:focus { border-color: #2f6bff; }"
                "QPushButton { background: #232329; color: #e6e6ea; border: none; "
                "border-radius: 8px; padding: 8px 14px; font-weight: 600; }"
                "QPushButton:hover { background: #2f6bff; color: #ffffff; }"
                "QPushButton#ShonImportPlus { padding: 0px; font-size: 22px; font-weight: 400; }"));
        auto* pLayout = new QVBoxLayout(m_pCard);
        pLayout->setContentsMargins(22, 20, 22, 18);
        pLayout->setSpacing(10);
        auto* pTitle = new QLabel(QStringLiteral("Add songs"), m_pCard);
        pTitle->setObjectName(QStringLiteral("ShonImportTitle"));
        pTitle->setCursor(Qt::OpenHandCursor);
        pTitle->setToolTip(QStringLiteral("Drag to move"));
        pTitle->installEventFilter(this);
        m_pTitleLabel = pTitle;
        qApp->installEventFilter(this);
        pLayout->addWidget(pTitle);
        auto* pHint = new QLabel(QStringLiteral(
                                         "Drop songs or links in the box, or paste a YouTube link. "
                                         "Each one gets stems, lands in your library and is "
                                         "analyzed. You can keep playing."),
                m_pCard);
        pHint->setObjectName(QStringLiteral("ShonImportDim"));
        pHint->setWordWrap(true);
        pLayout->addWidget(pHint);
        auto* pDrop = new ShonDropZone(m_pCard);
        pDrop->onDrop = [this](const QStringList& files, const QStringList& links) {
            for (const QString& link : links) {
                addLink(link);
            }
            int copied = 0;
            for (const QString& file : files) {
                const QFileInfo info(file);
                if (!info.isFile() || !kAudioSuffixes.contains(info.suffix().toLower())) {
                    continue;
                }
                // A copy goes to the Inbox, your original stays where it is.
                const QString target = inboxDir() + QLatin1Char('/') + info.fileName();
                if (QFile::exists(target) || QFile::copy(file, target)) {
                    ++copied;
                }
            }
            if (copied > 0) {
                emit notice(QStringLiteral("Add songs: %1 song(s) added").arg(copied));
                scanInbox();
            } else if (links.isEmpty() && !files.isEmpty()) {
                emit notice(QStringLiteral("Add songs: those aren't songs (mp3, m4a, flac, wav...)"));
            }
        };
        pLayout->addWidget(pDrop);
        auto* pRow = new QHBoxLayout();
        m_pLinkEdit = new QLineEdit(m_pCard);
        m_pLinkEdit->setPlaceholderText(QStringLiteral("https://www.youtube.com/watch?v=..."));
        auto* pAdd = new QPushButton(QStringLiteral("+"), m_pCard);
        pAdd->setObjectName(QStringLiteral("ShonImportPlus"));
        pAdd->setToolTip(QStringLiteral("Add this link"));
        pAdd->setFixedSize(38, 38);
        pRow->addWidget(m_pLinkEdit, 1);
        pRow->addWidget(pAdd);
        pLayout->addLayout(pRow);
        const auto submit = [this]() {
            addLink(m_pLinkEdit->text());
            m_pLinkEdit->clear();
        };
        connect(pAdd, &QPushButton::clicked, this, submit);
        connect(m_pLinkEdit, &QLineEdit::returnPressed, this, submit);
        m_pJobsLabel = new QLabel(m_pCard);
        m_pJobsLabel->setTextFormat(Qt::RichText);
        m_pJobsLabel->setWordWrap(true);
        pLayout->addWidget(m_pJobsLabel);
        auto* pButtons = new QHBoxLayout();
        auto* pInbox = new QPushButton(QStringLiteral("Open Inbox folder"), m_pCard);
        auto* pClear = new QPushButton(QStringLiteral("Clear finished"), m_pCard);
        auto* pClose = new QPushButton(QStringLiteral("Done"), m_pCard);
        pButtons->addWidget(pInbox);
        pButtons->addWidget(pClear);
        pButtons->addStretch();
        pButtons->addWidget(pClose);
        pLayout->addLayout(pButtons);
        connect(pInbox, &QPushButton::clicked, this, [this]() {
            QDesktopServices::openUrl(QUrl::fromLocalFile(inboxDir()));
        });
        connect(pClear, &QPushButton::clicked, this, [this]() {
            m_jobs.erase(std::remove_if(m_jobs.begin(),
                                 m_jobs.end(),
                                 [](const Job& job) {
                                     return job.stage == Stage::Done ||
                                             job.stage == Stage::Failed;
                                 }),
                    m_jobs.end());
            updateCard();
        });
        connect(pClose, &QPushButton::clicked, m_pCard, &QWidget::hide);
    }
    updateCard();
    m_pCard->adjustSize();
    if (pParent && firstTime) { // after that it stays where you dragged it
        const QRect area = pParent->geometry();
        m_pCard->move(area.center() - QPoint(m_pCard->width() / 2, m_pCard->height() / 2));
    }
    m_pCard->show();
    m_pCard->raise();
    m_pLinkEdit->setFocus();
}

bool ShonImport::eventFilter(QObject* pObject, QEvent* pEvent) {
    if (!m_pCard) {
        return QObject::eventFilter(pObject, pEvent);
    }
    if (pObject == m_pTitleLabel) {
        auto* pMouse = static_cast<QMouseEvent*>(pEvent);
        if (pEvent->type() == QEvent::MouseButtonPress && pMouse->button() == Qt::LeftButton) {
            m_dragOffset = pMouse->globalPosition().toPoint() - m_pCard->pos();
            m_pTitleLabel->setCursor(Qt::ClosedHandCursor);
            return true;
        }
        if (pEvent->type() == QEvent::MouseMove && (pMouse->buttons() & Qt::LeftButton)) {
            m_pCard->move(pMouse->globalPosition().toPoint() - m_dragOffset);
            return true;
        }
        if (pEvent->type() == QEvent::MouseButtonRelease) {
            m_pTitleLabel->setCursor(Qt::OpenHandCursor);
            return true;
        }
    }
    // A click anywhere else in ShonDJ closes the card. Other apps (like
    // Explorer, to drag songs in) don't count.
    if (pEvent->type() == QEvent::MouseButtonPress && m_pCard->isVisible()) {
        auto* pWidget = qobject_cast<QWidget*>(pObject);
        if (pWidget && pWidget->window() != m_pCard &&
                !(pWidget->window()->windowFlags() & Qt::Popup)) {
            m_pCard->hide();
        }
    }
    return QObject::eventFilter(pObject, pEvent);
}

void ShonImport::updateCard() {
    if (!m_pCard || !m_pJobsLabel) {
        return;
    }
    if (m_jobs.isEmpty()) {
        m_pJobsLabel->setText(QStringLiteral("<span style='color:#8a8a92'>Nothing in the queue.</span>"));
        m_pCard->adjustSize();
        return;
    }
    QStringList rows;
    // A long playlist: show the songs being worked on and the next few,
    // then a count, so the card stays a sensible size.
    int waitingShown = 0;
    int waitingHidden = 0;
    for (const Job& job : std::as_const(m_jobs)) {
        if (job.stage == Stage::Waiting && ++waitingShown > 5) {
            ++waitingHidden;
            continue;
        }
        QString stage;
        QString color = QStringLiteral("#8a8a92");
        switch (job.stage) {
        case Stage::Waiting:
            stage = QStringLiteral("waiting");
            break;
        case Stage::Downloading:
            stage = QStringLiteral("downloading");
            color = QStringLiteral("#2f6bff");
            break;
        case Stage::Playable:
            stage = QStringLiteral("playable, stems next");
            color = QStringLiteral("#34c759");
            break;
        case Stage::Stems:
            stage = job.message.isEmpty() ? QStringLiteral("playable, making stems")
                                          : job.message;
            color = QStringLiteral("#2f6bff");
            break;
        case Stage::Done:
            stage = job.message;
            color = QStringLiteral("#34c759");
            break;
        case Stage::Failed:
            stage = job.message;
            color = QStringLiteral("#ff3b30");
            break;
        }
        if ((job.stage == Stage::Downloading ||
                    (job.stage == Stage::Stems && job.message.isEmpty())) &&
                job.percent >= 0) {
            stage += QStringLiteral(" %1%").arg(job.percent);
        }
        QString name = job.name;
        if (name.size() > 60) {
            name = name.left(57) + QStringLiteral("...");
        }
        rows << QStringLiteral("%1 <span style='color:%2'>%3</span>")
                        .arg(name.toHtmlEscaped(), color, stage.toHtmlEscaped());
    }
    if (waitingHidden > 0) {
        rows << QStringLiteral("<span style='color:#8a8a92'>and %1 more waiting</span>")
                        .arg(waitingHidden);
    }
    if (rows.size() > 14) {
        rows = rows.mid(rows.size() - 14); // newest at the bottom
    }
    m_pJobsLabel->setText(rows.join(QStringLiteral("<br>")));
    m_pCard->adjustSize();
}
