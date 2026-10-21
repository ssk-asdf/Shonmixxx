#include "library/shonimport.h"

#include <QApplication>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <functional>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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
#include "library/library.h"
#include "library/trackcollectionmanager.h"
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
}

ShonImport::~ShonImport() {
    if (m_pProcess) {
        m_pProcess->disconnect(this);
        m_pProcess->kill();
        m_pProcess->waitForFinished(2000);
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

void ShonImport::addLink(const QString& url) {
    const QString link = url.trimmed();
    if (!link.startsWith(QLatin1String("http"))) {
        emit notice(QStringLiteral("Add songs: paste a full link (https://...)"));
        return;
    }
    Job job;
    job.id = m_nextId++;
    job.url = link;
    job.name = link;
    m_jobs.append(job);
    emit notice(QStringLiteral("Add songs: link added"));
    updateCard();
    startNext();
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
    startNext();
}

ShonImport::Job* ShonImport::currentJob() {
    for (Job& job : m_jobs) {
        if (job.id == m_currentId) {
            return &job;
        }
    }
    return nullptr;
}

void ShonImport::startNext() {
    if (m_pProcess) {
        return; // one song at a time
    }
    for (Job& job : m_jobs) {
        if (job.stage == Stage::Waiting) {
            m_currentId = job.id;
            if (job.file.isEmpty()) {
                runDownload(&job);
            } else {
                runStems(&job);
            }
            return;
        }
    }
    m_currentId = 0;
}

void ShonImport::startProcess(const QString& program, const QStringList& args) {
    m_processOutput.clear();
    m_pProcess = new QProcess(this);
    m_pProcess->setProcessChannelMode(QProcess::MergedChannels);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    m_pProcess->setProcessEnvironment(env);
#ifdef Q_OS_WIN
    // Below normal priority so the music never stutters, and no console window.
    m_pProcess->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* pArgs) {
        pArgs->flags |= 0x00004000; // BELOW_NORMAL_PRIORITY_CLASS
        pArgs->flags |= 0x08000000; // CREATE_NO_WINDOW
    });
#endif
    connect(m_pProcess, &QProcess::readyRead, this, &ShonImport::onProcessOutput);
    m_pProcess->start(program, args);
}

void ShonImport::onProcessOutput() {
    if (!m_pProcess) {
        return;
    }
    const QByteArray chunk = m_pProcess->readAll();
    m_processOutput += chunk;
    if (auto* pWatchdog = m_pProcess->findChild<QTimer*>(QStringLiteral("shonWatchdog"))) {
        pWatchdog->start();
    }
    if (m_processOutput.size() > 200000) {
        m_processOutput = m_processOutput.right(100000);
    }
    Job* pJob = currentJob();
    if (!pJob) {
        return;
    }
    // Progress shows up as "45.3%" in yt-dlp and in Demucs' progress bar.
    static const QRegularExpression kPercent(QStringLiteral("(\\d{1,3})(?:\\.\\d+)?%"));
    auto it = kPercent.globalMatch(QString::fromUtf8(chunk));
    int percent = -1;
    while (it.hasNext()) {
        percent = it.next().captured(1).toInt();
    }
    if (percent >= 0 && percent <= 100 && percent != pJob->percent) {
        pJob->percent = percent;
        updateCard();
    }
}

// ---------------------------------------------------------------- steps

void ShonImport::runDownload(Job* pJob) {
    pJob->stage = Stage::Downloading;
    pJob->percent = -1;
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
    startProcess(program, args);
    // Watchdog: if yt-dlp prints nothing for a while it is stuck (usually an
    // old yt-dlp after a YouTube change). Output restarts the clock.
    auto* pWatchdog = new QTimer(m_pProcess);
    pWatchdog->setObjectName(QStringLiteral("shonWatchdog"));
    pWatchdog->setSingleShot(true);
    pWatchdog->setInterval(kStuckMs);
    QProcess* pProcess = m_pProcess;
    connect(pWatchdog, &QTimer::timeout, pProcess, [pProcess]() {
        pProcess->setProperty("shonStuck", true);
        pProcess->kill();
    });
    pWatchdog->start();
    connect(m_pProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) {
            return;
        }
        m_pProcess->deleteLater();
        m_pProcess = nullptr;
        Job* pJob = currentJob();
        if (!pJob) {
            return;
        }
        if (!m_triedModuleYtDlp) {
            m_triedModuleYtDlp = true;
            runDownload(pJob);
        } else {
            failJob(pJob, QStringLiteral("yt-dlp not found"));
        }
    });
    connect(m_pProcess,
            &QProcess::finished,
            this,
            [this](int exitCode, QProcess::ExitStatus status) {
                const QString output = QString::fromUtf8(m_processOutput);
                const bool stuck = m_pProcess->property("shonStuck").toBool();
                m_pProcess->deleteLater();
                m_pProcess = nullptr;
                Job* pJob = currentJob();
                if (!pJob) {
                    startNext();
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
                runStems(pJob);
            });
}

void ShonImport::runStems(Job* pJob) {
    pJob->stage = Stage::Stems;
    pJob->percent = -1;
    updateCard();
    emit notice(QStringLiteral("Making stems: %1").arg(pJob->name));
    const QStringList args = pythonArgs() +
            QStringList{stemScript(),
                    QDir::toNativeSeparators(pJob->file),
                    QStringLiteral("--out"),
                    QDir::toNativeSeparators(stemsDir())};
    startProcess(pythonProgram(), args);
    connect(m_pProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) {
            return;
        }
        m_pProcess->deleteLater();
        m_pProcess = nullptr;
        if (Job* pJob = currentJob()) {
            // No Python or no Demucs: add the plain song so it's still usable.
            finishJob(pJob, QString());
        }
    });
    connect(m_pProcess,
            &QProcess::finished,
            this,
            [this](int, QProcess::ExitStatus) {
                m_pProcess->deleteLater();
                m_pProcess = nullptr;
                Job* pJob = currentJob();
                if (!pJob) {
                    startNext();
                    return;
                }
                const QString stemFile = stemsDir() + QLatin1Char('/') +
                        QFileInfo(pJob->file).completeBaseName() + QStringLiteral(".stem.mp4");
                finishJob(pJob, QFileInfo::exists(stemFile) ? stemFile : QString());
            });
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

void ShonImport::finishJob(Job* pJob, const QString& stemFile) {
    const QString original = keepOriginal(pJob->file);
    pJob->file = original;
    const QString addPath = stemFile.isEmpty() ? original : stemFile;
    TrackPointer pTrack = m_pTrackCollectionManager->getOrAddTrack(TrackRef::fromFilePath(addPath));
    if (!pTrack) {
        failJob(pJob, QStringLiteral("could not add to the library"));
        return;
    }
    cleanTags(pTrack);
    emit m_pLibrary->analyzeTracks({AnalyzerScheduledTrack(pTrack->getId())});
    pJob->stage = Stage::Done;
    pJob->percent = 100;
    pJob->name = pTrack->getArtist().isEmpty()
            ? pTrack->getTitle()
            : pTrack->getArtist() + QStringLiteral(" - ") + pTrack->getTitle();
    pJob->message = stemFile.isEmpty() ? QStringLiteral("added without stems")
                                       : QStringLiteral("added with stems");
    emit notice(QStringLiteral("Added: %1 (%2)").arg(pJob->name, pJob->message));
    updateCard();
    startNext();
}

void ShonImport::failJob(Job* pJob, const QString& message) {
    pJob->stage = Stage::Failed;
    pJob->message = message;
    emit notice(QStringLiteral("Add songs: %1 (%2)").arg(pJob->name, message));
    updateCard();
    startNext();
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

// ---------------------------------------------------------------- card

void ShonImport::showCard() {
    QWidget* pParent = QApplication::activeWindow();
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
    if (pParent) {
        const QRect area = pParent->geometry();
        m_pCard->move(area.center() - QPoint(m_pCard->width() / 2, m_pCard->height() / 2));
    }
    m_pCard->show();
    m_pCard->raise();
    m_pLinkEdit->setFocus();
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
    for (const Job& job : std::as_const(m_jobs)) {
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
        case Stage::Stems:
            stage = QStringLiteral("making stems");
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
        if ((job.stage == Stage::Downloading || job.stage == Stage::Stems) && job.percent >= 0) {
            stage += QStringLiteral(" %1%").arg(job.percent);
        }
        QString name = job.name;
        if (name.size() > 60) {
            name = name.left(57) + QStringLiteral("...");
        }
        rows << QStringLiteral("%1 <span style='color:%2'>%3</span>")
                        .arg(name.toHtmlEscaped(), color, stage.toHtmlEscaped());
    }
    m_pJobsLabel->setText(rows.join(QStringLiteral("<br>")));
    m_pCard->adjustSize();
}
