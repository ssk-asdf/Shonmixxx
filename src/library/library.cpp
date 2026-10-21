#include "library/library.h"
#include "library/shonrequests.h"
#include "library/shonremote.h"
#include "library/shonimport.h"
#include "library/shonpractice.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QDir>
#include <QMessageBox>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QSqlQuery>
#include <algorithm>
#include <cmath>

#include "library/basetracktablemodel.h"
#include "track/cue.h"
#include "control/controlobject.h"
#include "controllers/keyboard/keyboardeventfilter.h"
#include "library/analysis/analysisfeature.h"
#include "library/autodj/autodjfeature.h"
#include "library/banshee/bansheefeature.h"
#include "library/browse/browsefeature.h"
#include "library/dateformatbroadcaster.h"
#ifdef __ENGINEPRIME__
#include "library/export/libraryexporter.h"
#endif
#include "library/externaltrackcollection.h"
#include "library/itunes/itunesfeature.h"
#include "library/library_prefs.h"
#include "library/librarycontrol.h"
#include "library/libraryfeature.h"
#include "library/mixxxlibraryfeature.h"
#include "library/recording/recordingfeature.h"
#include "library/rekordbox/rekordboxfeature.h"
#include "library/rhythmbox/rhythmboxfeature.h"
#include "library/serato/seratofeature.h"
#include "library/sidebarmodel.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "library/trackmodel.h"
#include "library/trackset/crate/cratefeature.h"
#include "library/trackset/playlistfeature.h"
#include "library/trackset/setlogfeature.h"
#include "library/dao/analysisdao.h"
#include "library/dao/trackdao.h"
#include "library/dao/trackschema.h"
#include "track/beats.h"
#include "waveform/waveform.h"
#include "library/traktor/traktorfeature.h"
#include "mixer/playerinfo.h"
#include "control/controlproxy.h"
#include "control/controlpushbutton.h"
#include "mixer/playermanager.h"
#include "track/keyutils.h"
#include "track/track.h"
#include "moc_library.cpp"
#include "util/assert.h"
#include "util/logger.h"
#include "util/sandbox.h"
#include "widget/wlibrary.h"
#include "widget/wlibrarysidebar.h"
#include "widget/wsearchlineedit.h"
#include "widget/wtracktableview.h"

namespace {

const mixxx::Logger kLogger("Library");

} // namespace

using namespace mixxx::library::prefs;

// This is the name which we use to register the WTrackTableView with the
// WLibrary
const QString Library::m_sTrackViewName = QStringLiteral("WTrackTableView");

const QString Library::kAutoDJViewName = QStringLiteral("Auto DJ");

// The default row height of the library.
const int Library::kDefaultRowHeightPx = 20;

Library::Library(
        QObject* parent,
        UserSettingsPointer pConfig,
        mixxx::DbConnectionPoolPtr pDbConnectionPool,
        TrackCollectionManager* pTrackCollectionManager,
        PlayerManager* pPlayerManager,
        RecordingManager* pRecordingManager)
        : QObject(parent),
          m_pConfig(pConfig),
          m_pDbConnectionPool(std::move(pDbConnectionPool)),
          m_pTrackCollectionManager(pTrackCollectionManager),
          m_pSidebarModel(make_parented<SidebarModel>(this)),
          m_pLibraryControl(make_parented<LibraryControl>(this)),
          m_pLibraryWidget(nullptr),
          m_pKeyNotation(std::make_unique<ControlObject>(
                  mixxx::library::prefs::kKeyNotationConfigKey, false)) {
    qRegisterMetaType<LibraryRemovalType>("LibraryRemovalType");

    DateFormatChangedBroadcaster::createInstance();

    connect(m_pTrackCollectionManager,
            &TrackCollectionManager::libraryScanFinished,
            this,
            &Library::slotRefreshLibraryModels);

    // TODO(rryan) -- turn this construction / adding of features into a static
    // method or something -- CreateDefaultLibrary
    m_pMixxxLibraryFeature = make_parented<MixxxLibraryFeature>(
            this,
            m_pConfig);
    addFeature(m_pMixxxLibraryFeature);
#ifdef __ENGINEPRIME__
    connect(m_pMixxxLibraryFeature,
            &MixxxLibraryFeature::exportLibrary,
            this,
            &Library::exportLibrary,
            Qt::DirectConnection /* signal-to-signal */);
#endif

    m_pAutoDJFeature = make_parented<AutoDJFeature>(this, m_pConfig, pPlayerManager);
    addFeature(m_pAutoDJFeature);

    // Shon: stealth mode hides song names and covers. Remembered between
    // sessions. Holding stealth_peek shows them while held.
    m_pShonStealth = std::make_unique<ControlPushButton>(
            ConfigKey(QStringLiteral("[Shon]"), QStringLiteral("stealth")), true);
    static_cast<ControlPushButton*>(m_pShonStealth.get())
            ->setButtonMode(mixxx::control::ButtonMode::Toggle);
    m_pShonStealthPeek = std::make_unique<ControlPushButton>(
            ConfigKey(QStringLiteral("[Shon]"), QStringLiteral("stealth_peek")));
    const auto shonRepaintTables = [](double) {
        // Track tables read stealth on paint, so a repaint is enough.
        const auto widgets = QApplication::allWidgets();
        for (QWidget* pWidget : widgets) {
            if (auto* pView = qobject_cast<QAbstractItemView*>(pWidget)) {
                pView->viewport()->update();
            }
        }
    };
    connect(m_pShonStealth.get(), &ControlObject::valueChanged, this, shonRepaintTables);

    // Shon: songs swapped to stem files get their cues and beatgrid lined
    // up with the new file after its waveform is made the first time.
    connect(&m_pTrackCollectionManager->internalCollection()->getTrackDAO(),
            &TrackDAO::waveformSummaryUpdated,
            this,
            &Library::shonAlignSwappedStem,
            Qt::QueuedConnection);

    // Shon: stem lines on the waveform. On by default, remembered.
    m_pShonStemLines = std::make_unique<ControlPushButton>(
            ConfigKey(QStringLiteral("[Shon]"), QStringLiteral("stem_lines")), true, 1.0);
    static_cast<ControlPushButton*>(m_pShonStemLines.get())
            ->setButtonMode(mixxx::control::ButtonMode::Toggle);

    // Shon: [ChannelN],shon_unmute_stems turns every stem of that deck back on.
    for (int deck = 1; deck <= 4; ++deck) {
        const QString group = QStringLiteral("[Channel%1]").arg(deck);
        auto pButton = std::make_unique<ControlPushButton>(
                ConfigKey(group, QStringLiteral("shon_unmute_stems")));
        connect(pButton.get(), &ControlObject::valueChanged, this, [this, deck](double value) {
            if (value <= 0.0) {
                return;
            }
            for (int stem = 1; stem <= 4; ++stem) {
                ControlProxy mute(QStringLiteral("[Channel%1_Stem%2]").arg(deck).arg(stem),
                        QStringLiteral("mute"));
                if (mute.valid()) {
                    mute.set(0.0);
                }
            }
        });
        m_shonUnmuteStems.push_back(std::move(pButton));
    }
    connect(m_pShonStealthPeek.get(), &ControlObject::valueChanged, this, shonRepaintTables);

    // Shon: guest song requests through a QR code, shown as "Requests".
    m_pShonRequests = make_parented<ShonRequests>(
            m_pConfig, this, m_pTrackCollectionManager.data(), this);

    // Shon: phone remote over Wi-Fi, with the relay as a backup.
    m_pShonRemote = make_parented<ShonRemote>(m_pConfig,
            this,
            m_pTrackCollectionManager.data(),
            m_pShonRequests.get(),
            this);
    connect(m_pShonRemote.get(), &ShonRemote::notice, this, &Library::shonNotice);

    // Shon: add songs from a link or the Inbox folder, with stems.
    m_pShonImport = make_parented<ShonImport>(
            m_pConfig, this, m_pTrackCollectionManager.data(), this);
    connect(m_pShonImport.get(), &ShonImport::notice, this, &Library::shonNotice);

    // Shon: practice mode scores each transition.
    m_pShonPractice = make_parented<ShonPractice>(m_pConfig, this);
    connect(m_pShonPractice.get(), &ShonPractice::notice, this, &Library::shonNotice);

    m_pPlaylistFeature = make_parented<PlaylistFeature>(this, UserSettingsPointer(m_pConfig));
    addFeature(m_pPlaylistFeature);
#ifdef __ENGINEPRIME__
    connect(m_pPlaylistFeature,
            &PlaylistFeature::exportAllPlaylists,
            this,
            &Library::exportLibrary, // signal-to-signal
            Qt::DirectConnection);
    connect(m_pPlaylistFeature,
            &PlaylistFeature::exportPlaylist,
            this,
            &Library::exportPlaylist, // signal-to-signal
            Qt::DirectConnection);
#endif

    m_pCrateFeature = make_parented<CrateFeature>(this, m_pConfig);
    addFeature(m_pCrateFeature);
#ifdef __ENGINEPRIME__
    connect(m_pCrateFeature,
            &CrateFeature::exportAllCrates,
            this,
            &Library::exportLibrary, // signal-to-signal
            Qt::DirectConnection);
    connect(m_pCrateFeature,
            &CrateFeature::exportCrate,
            this,
            &Library::exportCrate, // signal-to-signal
            Qt::DirectConnection);
#endif

    m_pBrowseFeature = make_parented<BrowseFeature>(
            this, m_pConfig, pRecordingManager);
    connect(m_pBrowseFeature,
            &BrowseFeature::scanLibrary,
            m_pTrackCollectionManager,
            &TrackCollectionManager::startLibraryScan);
    connect(m_pTrackCollectionManager,
            &TrackCollectionManager::libraryScanStarted,
            m_pBrowseFeature,
            &BrowseFeature::slotLibraryScanStarted);
    connect(m_pTrackCollectionManager,
            &TrackCollectionManager::libraryScanFinished,
            m_pBrowseFeature,
            &BrowseFeature::slotLibraryScanFinished);
    addFeature(m_pBrowseFeature);

    addFeature(new RecordingFeature(this, m_pConfig, pRecordingManager));

    addFeature(new SetlogFeature(this, UserSettingsPointer(m_pConfig)));

    m_pAnalysisFeature = make_parented<AnalysisFeature>(this, m_pConfig);
    connect(m_pPlaylistFeature,
            &PlaylistFeature::analyzeTracks,
            m_pAnalysisFeature,
            &AnalysisFeature::analyzeTracks);
    connect(m_pCrateFeature,
            &CrateFeature::analyzeTracks,
            m_pAnalysisFeature,
            &AnalysisFeature::analyzeTracks);
    connect(this,
            &Library::analyzeTracks,
            m_pAnalysisFeature,
            &AnalysisFeature::analyzeTracks);
    addFeature(m_pAnalysisFeature);
    // Suspend a batch analysis while an ad-hoc analysis of
    // loaded tracks is in progress and resume it afterwards.
    connect(pPlayerManager,
            &PlayerManager::trackAnalyzerProgress,
            this,
            &Library::onPlayerManagerTrackAnalyzerProgress);
    connect(pPlayerManager,
            &PlayerManager::trackAnalyzerIdle,
            this,
            &Library::onPlayerManagerTrackAnalyzerIdle);
    connect(m_pAnalysisFeature,
            &AnalysisFeature::trackProgress,
            this,
            &Library::onTrackAnalyzerProgress);

    // iTunes and Rhythmbox should be last until we no longer have an obnoxious
    // messagebox popup when you select them. (This forces you to reach for your
    // mouse or keyboard if you're using MIDI control and you scroll through them...)
    if (RhythmboxFeature::isSupported() &&
            m_pConfig->getValue(
                    ConfigKey(kConfigGroup, "ShowRhythmboxLibrary"), true)) {
        addFeature(new RhythmboxFeature(this, m_pConfig));
    }
    if (m_pConfig->getValue(
                ConfigKey(kConfigGroup, "ShowBansheeLibrary"), true)) {
        BansheeFeature::prepareDbPath(m_pConfig);
        if (BansheeFeature::isSupported()) {
            addFeature(new BansheeFeature(this, m_pConfig));
        }
    }
    if (ITunesFeature::isSupported() &&
            m_pConfig->getValue(
                    ConfigKey(kConfigGroup, "ShowITunesLibrary"), true)) {
        addFeature(new ITunesFeature(this, m_pConfig));
    }
    if (TraktorFeature::isSupported() &&
            m_pConfig->getValue(
                    ConfigKey(kConfigGroup, "ShowTraktorLibrary"), true)) {
        addFeature(new TraktorFeature(this, m_pConfig));
    }

    // TODO(XXX) Rekordbox feature added persistently as the only way to enable it to
    // dynamically appear/disappear when correctly prepared removable devices
    // are mounted/unmounted would be to have some form of timed thread to check
    // periodically. Not ideal performance wise.
    if (m_pConfig->getValue(
                ConfigKey(kConfigGroup, "ShowRekordboxLibrary"), true)) {
        addFeature(new RekordboxFeature(this, m_pConfig));
    }

    if (m_pConfig->getValue(
                ConfigKey(kConfigGroup, "ShowSeratoLibrary"), true)) {
        addFeature(new SeratoFeature(this, m_pConfig));
    }

    for (const auto& externalTrackCollection : m_pTrackCollectionManager->externalCollections()) {
        auto* feature = externalTrackCollection->newLibraryFeature(this, m_pConfig);
        if (feature) {
            kLogger.info() << "Adding library feature for"
                           << externalTrackCollection->name();
            addFeature(feature);
        } else {
            kLogger.info() << "Library feature for"
                           << externalTrackCollection->name()
                           << "is not available";
        }
    }

    // On startup we need to check if all of the user's library folders are
    // accessible to us. If the user is using a database from <1.12.0 with
    // sandboxing then we will need them to give us permission.
    const auto rootDirs = m_pTrackCollectionManager->internalCollection()->loadRootDirs();
    for (mixxx::FileInfo dirInfo : rootDirs) {
        if (!dirInfo.exists() || !dirInfo.isDir()) {
            kLogger.warning()
                    << "Skipping access check for missing or invalid directory"
                    << dirInfo;
            continue;
        }
        if (Sandbox::askForAccess(&dirInfo)) {
            kLogger.info()
                    << "Access to directory"
                    << dirInfo
                    << "from sandbox granted";
        } else {
            kLogger.warning()
                    << "Access to directory"
                    << dirInfo
                    << "from sandbox denied";
        }
    }

    m_iTrackTableRowHeight = m_pConfig->getValue(
            ConfigKey(kConfigGroup, "RowHeight"), kDefaultRowHeightPx);
    QString fontStr =
            m_pConfig->getValueString(ConfigKey(kConfigGroup, "Font"));
    if (!fontStr.isEmpty()) {
        m_trackTableFont.fromString(fontStr);
    } else {
        m_trackTableFont = QApplication::font();
    }

    m_editMetadataSelectedClick = m_pConfig->getValue(
            kEditMetadataSelectedClickConfigKey,
            kEditMetadataSelectedClickDefault);

    m_pShonShowMatches = std::make_unique<ControlPushButton>(
            ConfigKey(QStringLiteral("[Library]"), QStringLiteral("shon_show_matches")));
    // Toggle, so the MATCH button stays lit while the filter is on.
    static_cast<ControlPushButton*>(m_pShonShowMatches.get())
            ->setButtonMode(mixxx::control::ButtonMode::Toggle);
    connect(m_pShonShowMatches.get(),
            &ControlObject::valueChanged,
            this,
            &Library::slotShonToggleMatches);
    connect(&PlayerInfo::instance(),
            &PlayerInfo::currentPlayingTrackChanged,
            this,
            &Library::slotShonPlayingTrackChanged);
    // Loading the next song: point at its "from <playing song>" cue.
    connect(&PlayerInfo::instance(),
            &PlayerInfo::trackChanged,
            this,
            &Library::slotShonDeckTrackChanged);

    m_pShonAwayMode = std::make_unique<ControlPushButton>(
            ConfigKey(QStringLiteral("[AutoDJ]"), QStringLiteral("shon_away_mode")));
    static_cast<ControlPushButton*>(m_pShonAwayMode.get())
            ->setButtonMode(mixxx::control::ButtonMode::Toggle);
    connect(m_pShonAwayMode.get(),
            &ControlObject::valueChanged,
            this,
            &Library::slotShonToggleAwayMode);
    m_pShonAwayTimer = new QTimer(this);
    connect(m_pShonAwayTimer, &QTimer::timeout, this, &Library::slotShonAwayTopUp);
    // If Auto DJ gets turned off some other way, Away Mode turns off too.
    auto* pAutoDjEnabled = new ControlProxy(
            QStringLiteral("[AutoDJ]"), QStringLiteral("enabled"), this);
    pAutoDjEnabled->connectValueChanged(this, [this](double value) {
        if (value <= 0.0 && m_pShonAwayMode->get() > 0.0) {
            m_pShonAwayMode->set(0.0);
        }
    });
}

void Library::slotShonToggleMatches(double value) {
    const bool active = value > 0.0;
    if (active == m_shonMatchesActive) {
        return;
    }
    m_shonMatchesActive = active;
    if (m_shonMatchesActive) {
        shonApplyMatchesFilter(PlayerInfo::instance().getCurrentPlayingTrack());
    } else {
        searchTracksInCollection(QString());
    }
}

void Library::slotShonDeckTrackChanged(
        const QString& group, TrackPointer pNewTrack, TrackPointer pOldTrack) {
    Q_UNUSED(pOldTrack);
    // A cue labeled "from Blow" on the song you just loaded = where to bring
    // it in when Blow is playing. Show it so you don't have to remember.
    TrackPointer pPlaying = PlayerInfo::instance().getCurrentPlayingTrack();
    if (!pNewTrack || !pPlaying || pPlaying == pNewTrack) {
        return;
    }
    const auto wordsOf = [](QString text) {
        text = text.toLower();
        text.replace(QRegularExpression(QStringLiteral("[^\\p{L}\\p{N}]+")), QStringLiteral(" "));
        return text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    };
    const QStringList playingWords =
            wordsOf(pPlaying->getTitle() + QLatin1Char(' ') + pPlaying->getArtist());
    const QList<CuePointer> cues = pNewTrack->getCuePoints();
    for (const CuePointer& pCue : cues) {
        if (!pCue || pCue->getHotCue() < 0) {
            continue;
        }
        QString label = pCue->getLabel().trimmed().toLower();
        if (label.startsWith(QLatin1Char('<'))) {
            label = label.mid(1);
        } else if (label.startsWith(QLatin1String("from "))) {
            label = label.mid(5);
        } else {
            continue;
        }
        const QStringList want = wordsOf(label);
        if (want.isEmpty()) {
            continue;
        }
        const bool matches = std::all_of(want.cbegin(), want.cend(), [&](const QString& word) {
            return playingWords.contains(word);
        });
        if (matches) {
            emit shonNotice(QStringLiteral("%1: come in at cue %2 (from %3)")
                                    .arg(pNewTrack->getTitle())
                                    .arg(pCue->getHotCue() + 1)
                                    .arg(pPlaying->getTitle()));
            Q_UNUSED(group);
            return;
        }
    }
}

void Library::slotShonPlayingTrackChanged(TrackPointer pTrack) {
    if (pTrack) {
        shonUpdatePlannedTransitions(pTrack);
    }
    // Only follow the playing track while the filter is on, and keep the
    // last results when nothing is playing.
    if (m_shonMatchesActive && pTrack) {
        shonApplyMatchesFilter(pTrack);
    }
}

namespace {
// Shon Away Mode tuning.
constexpr int kShonAwayInitialTracks = 12;
constexpr int kShonAwayTopUpBelow = 4;
constexpr int kShonAwayTopUpTracks = 8;
constexpr int kShonAwayCheckIntervalMs = 20000;

struct ShonTrackInfo {
    qlonglong id = -1;
    double bpm = 0.0;
    int key = 0;
    int energy = 0;
    bool prepped = false;
};

double shonBpmDiff(double bpm, double referenceBpm) {
    double diff = 1.0;
    for (const double factor : {1.0, 0.5, 2.0}) {
        diff = std::min(diff, std::abs(bpm * factor - referenceBpm) / referenceBpm);
    }
    return diff;
}

// Score how well candidate follows reference. Negative means "don't".
// strict = BPM within 6% and compatible key; relaxed = BPM within 10%.
double shonChainScore(const ShonTrackInfo& reference,
        const ShonTrackInfo& candidate,
        bool strict) {
    if (reference.bpm <= 0.0 || candidate.bpm <= 0.0) {
        return -1.0;
    }
    const double bpmDiff = shonBpmDiff(candidate.bpm, reference.bpm);
    if (bpmDiff > (strict ? 0.06 : 0.10)) {
        return -1.0;
    }
    const auto referenceKey = static_cast<mixxx::track::io::key::ChromaticKey>(reference.key);
    if (strict && referenceKey != mixxx::track::io::key::INVALID && candidate.key != 0) {
        const auto compatible = KeyUtils::getCompatibleKeys(referenceKey);
        if (!compatible.contains(
                    static_cast<mixxx::track::io::key::ChromaticKey>(candidate.key))) {
            return -1.0;
        }
    }
    double score = 100.0;
    // Keep the room steady: same energy best, one up or down is fine.
    if (reference.energy <= 0 || candidate.energy <= 0) {
        score += 25.0;
    } else if (candidate.energy == reference.energy) {
        score += 50.0;
    } else if (candidate.energy == reference.energy + 1) {
        score += 40.0;
    } else if (candidate.energy == reference.energy - 1) {
        score += 30.0;
    }
    if (candidate.prepped) {
        score += 30.0;
    }
    score -= bpmDiff * 200.0;
    // A little variety so Away Mode doesn't play the same chain every time.
    score += QRandomGenerator::global()->bounded(8.0);
    return score;
}
} // namespace

void Library::slotShonToggleAwayMode(double value) {
    ControlProxy autoDjEnabled(QStringLiteral("[AutoDJ]"), QStringLiteral("enabled"));
    if (value > 0.0) {
        shonAwayAddTracks(kShonAwayInitialTracks);
        autoDjEnabled.set(1.0);
        if (autoDjEnabled.get() <= 0.0) {
            // Auto DJ refused (for example both decks are playing). Its own
            // message explains why; turn Away Mode back off to match.
            m_pShonAwayMode->set(0.0);
            return;
        }
        m_pShonAwayTimer->start(kShonAwayCheckIntervalMs);
    } else {
        m_pShonAwayTimer->stop();
        if (autoDjEnabled.get() > 0.0) {
            autoDjEnabled.set(0.0);
        }
    }
}

void Library::slotShonAwayTopUp() {
    if (!m_pTrackCollectionManager) {
        return;
    }
    PlaylistDAO& playlistDao = m_pTrackCollectionManager->internalCollection()->getPlaylistDAO();
    const int autoDjId = playlistDao.getPlaylistIdFromName(AUTODJ_TABLE);
    if (autoDjId >= 0 && playlistDao.tracksInPlaylist(autoDjId) < kShonAwayTopUpBelow) {
        shonAwayAddTracks(kShonAwayTopUpTracks);
    }
}

void Library::shonAwayAddTracks(int count) {
    if (!m_pTrackCollectionManager) {
        return;
    }
    TrackCollection* pCollection = m_pTrackCollectionManager->internalCollection();
    PlaylistDAO& playlistDao = pCollection->getPlaylistDAO();
    const int autoDjId = playlistDao.getPlaylistIdFromName(AUTODJ_TABLE);
    if (autoDjId < 0) {
        return;
    }
    const QSqlDatabase database = pCollection->database();

    // Tracks with both an IN and an OUT cue label count as prepped.
    QSet<qlonglong> preppedIds;
    {
        QSqlQuery query(database);
        if (query.exec(QStringLiteral(
                    "SELECT track_id FROM cues WHERE upper(label) IN ('IN', 'OUT') "
                    "GROUP BY track_id HAVING COUNT(DISTINCT upper(label)) = 2"))) {
            while (query.next()) {
                preppedIds.insert(query.value(0).toLongLong());
            }
        }
    }

    // Skip anything queued, loaded on a deck, or already played this session.
    QSet<qlonglong> excluded;
    const QList<TrackId> queued = playlistDao.getTrackIdsInPlaylistOrder(autoDjId);
    for (const TrackId& id : queued) {
        excluded.insert(id.toVariant().toLongLong());
    }
    for (int deck = 0; deck < PlayerInfo::instance().numDecks(); ++deck) {
        TrackPointer pLoaded = PlayerInfo::instance().getTrackInfo(
                PlayerManager::groupForDeck(deck));
        if (pLoaded) {
            excluded.insert(pLoaded->getId().toVariant().toLongLong());
        }
    }

    QList<ShonTrackInfo> candidates;
    QHash<qlonglong, ShonTrackInfo> byId;
    {
        QSqlQuery query(database);
        if (!query.exec(QStringLiteral(
                    "SELECT id, bpm, key_id, rating, played FROM library "
                    "WHERE mixxx_deleted = 0 AND bpm > 0"))) {
            return;
        }
        while (query.next()) {
            ShonTrackInfo info;
            info.id = query.value(0).toLongLong();
            info.bpm = query.value(1).toDouble();
            info.key = query.value(2).toInt();
            info.energy = query.value(3).toInt();
            info.prepped = preppedIds.contains(info.id);
            byId.insert(info.id, info);
            if (query.value(4).toInt() == 0 && !excluded.contains(info.id)) {
                candidates.append(info);
            }
        }
    }

    // Chain from the last queued track, or from what's playing.
    ShonTrackInfo reference;
    if (!queued.isEmpty()) {
        reference = byId.value(queued.last().toVariant().toLongLong());
    }
    if (reference.bpm <= 0.0) {
        TrackPointer pPlaying = PlayerInfo::instance().getCurrentPlayingTrack();
        if (!pPlaying || pPlaying->getBpm() <= 0.0) {
            return;
        }
        reference.id = pPlaying->getId().toVariant().toLongLong();
        reference.bpm = pPlaying->getBpm();
        reference.key = static_cast<int>(pPlaying->getKey());
        reference.energy = pPlaying->getRating();
    }

    QList<TrackId> picks;
    for (int i = 0; i < count && !candidates.isEmpty(); ++i) {
        int bestIndex = -1;
        double bestScore = -1.0;
        for (const bool strict : {true, false}) {
            for (int c = 0; c < candidates.size(); ++c) {
                const double score = shonChainScore(reference, candidates.at(c), strict);
                if (score > bestScore) {
                    bestScore = score;
                    bestIndex = c;
                }
            }
            if (bestIndex >= 0) {
                break;
            }
        }
        if (bestIndex < 0) {
            break; // nothing left that mixes
        }
        reference = candidates.takeAt(bestIndex);
        picks.append(TrackId(QVariant(reference.id)));
    }
    if (!picks.isEmpty()) {
        playlistDao.appendTracksToPlaylist(picks, autoDjId);
    }
}

namespace {

// Mono "all" band of a waveform, mean removed, first maxFrames visual frames.
std::vector<float> shonWaveformMono(const Waveform& waveform, int maxFrames) {
    const int frames = std::min(waveform.getDataSize() / 2, maxFrames);
    std::vector<float> out(std::max(frames, 0));
    if (frames <= 0) {
        return out;
    }
    const WaveformData* pData = waveform.data();
    double sum = 0;
    for (int i = 0; i < frames; ++i) {
        out[i] = 0.5f * (pData[2 * i].filtered.all + pData[2 * i + 1].filtered.all);
        sum += out[i];
    }
    const float mean = static_cast<float>(sum / frames);
    for (float& v : out) {
        v -= mean;
    }
    return out;
}

double shonCorrelate(const std::vector<float>& a, const std::vector<float>& b, int lag) {
    // b[i + lag] against a[i]
    double sum = 0;
    const int n = static_cast<int>(std::min(a.size(), b.size()));
    for (int i = std::max(0, -lag); i < n && i + lag < n; ++i) {
        sum += static_cast<double>(a[i]) * b[i + lag];
    }
    return sum;
}

} // namespace

void Library::shonAlignSwappedStem(TrackId trackId) {
    TrackCollection* pCollection = m_pTrackCollectionManager->internalCollection();
    QSqlQuery query(pCollection->database());
    query.prepare(QStringLiteral(
            "SELECT old_id, offset_ms FROM shon_stem_swaps WHERE new_id=:id AND aligned=0"));
    query.bindValue(QStringLiteral(":id"), trackId.toVariant());
    if (!query.exec() || !query.next()) {
        return; // not a swapped song, or already lined up
    }
    const TrackId oldId(query.value(0));
    // Cues may already carry an earlier shift (stem files fixed later on).
    const double appliedMillis = query.value(1).toDouble();

    const auto markAligned = [&](double millis) {
        QSqlQuery update(pCollection->database());
        update.prepare(QStringLiteral(
                "UPDATE shon_stem_swaps SET aligned=1, offset_ms=:ms WHERE new_id=:id"));
        update.bindValue(QStringLiteral(":ms"), millis);
        update.bindValue(QStringLiteral(":id"), trackId.toVariant());
        update.exec();
    };

    TrackPointer pTrack = m_pTrackCollectionManager->getTrackById(trackId);
    if (!pTrack) {
        return;
    }
    ConstWaveformPointer pNew = pTrack->getWaveform();
    if (!pNew || pNew->getVisualSampleRate() <= 0 ||
            pNew->getCompletion() < pNew->getDataSize()) {
        return; // waveform not finished, try again next time
    }
    const auto analyses = pCollection->getAnalysisDAO().getAnalysesForTrackByType(
            oldId, AnalysisDao::TYPE_WAVEFORM);
    if (analyses.isEmpty()) {
        markAligned(appliedMillis); // nothing to compare against
        return;
    }
    const Waveform oldWaveform(analyses.first().data);
    const double rate = pNew->getVisualSampleRate();
    if (std::abs(oldWaveform.getVisualSampleRate() - rate) > 0.5) {
        markAligned(appliedMillis);
        return;
    }

    // Compare the first minute, within 150 ms either way.
    const int frames = static_cast<int>(rate * 60);
    const std::vector<float> a = shonWaveformMono(oldWaveform, frames);
    const std::vector<float> b = shonWaveformMono(*pNew, frames);
    if (a.size() < 100 || b.size() < 100) {
        markAligned(appliedMillis);
        return;
    }
    const int maxLag = static_cast<int>(rate * 0.15);
    int bestLag = 0;
    double best = -1e300;
    for (int lag = -maxLag; lag <= maxLag; ++lag) {
        const double c = shonCorrelate(a, b, lag);
        if (c > best) {
            best = c;
            bestLag = lag;
        }
    }
    // Refine between visual frames.
    double lag = bestLag;
    if (bestLag > -maxLag && bestLag < maxLag) {
        const double y0 = shonCorrelate(a, b, bestLag - 1);
        const double y2 = shonCorrelate(a, b, bestLag + 1);
        const double denom = y0 - 2 * best + y2;
        if (denom != 0) {
            lag += 0.5 * (y0 - y2) / denom;
        }
    }
    if (bestLag <= -maxLag || bestLag >= maxLag) {
        // No clear match. Leave the cues alone rather than guess.
        qWarning() << "Shon stem swap:" << trackId << "no clear offset, cues left as they are";
        markAligned(appliedMillis);
        return;
    }
    const double measured = lag / rate * 1000.0;
    const double millis = measured - appliedMillis;
    qInfo() << "Shon stem swap:" << trackId << "offset" << measured << "ms, shifting" << millis;

    if (std::abs(millis) >= 1.0) {
        pTrack->shiftCuePositionsMillis(millis);
        const auto sampleRate = pTrack->getSampleRate();
        if (const mixxx::BeatsPointer pBeats = pTrack->getBeats(); pBeats && sampleRate.isValid()) {
            const auto frameOffset = static_cast<mixxx::audio::FrameDiff_t>(
                    millis * sampleRate.value() / 1000.0);
            if (const auto translated = pBeats->tryTranslate(frameOffset)) {
                const bool wasLocked = pTrack->isBpmLocked();
                if (wasLocked) {
                    pTrack->setBpmLocked(false);
                }
                pTrack->trySetBeats(*translated);
                if (wasLocked) {
                    pTrack->setBpmLocked(true);
                }
            }
        }
    }
    markAligned(measured);
}

void Library::shonUpdatePlannedTransitions(TrackPointer pTrack) {
    // A hot cue labeled with another song's name ("Low", "-> Low Flo Rida")
    // means "start the transition into that song here". Find those songs.
    static const QSet<QString> kSkip = {QStringLiteral("in"),
            QStringLiteral("out"),
            QStringLiteral("drop"),
            QStringLiteral("break"),
            QStringLiteral("intro"),
            QStringLiteral("outro"),
            QStringLiteral("verse"),
            QStringLiteral("chorus"),
            QStringLiteral("hook"),
            QStringLiteral("build"),
            QStringLiteral("buildup"),
            QStringLiteral("bridge"),
            QStringLiteral("vocal"),
            QStringLiteral("vocals"),
            QStringLiteral("start"),
            QStringLiteral("end"),
            QStringLiteral("mix"),
            QStringLiteral("loop")};
    QSet<qlonglong> plannedIds;
    QStringList searches;
    QStringList noticeParts;
    if (m_pTrackCollectionManager) {
        const QSqlDatabase database =
                m_pTrackCollectionManager->internalCollection()->database();
        const qlonglong playingId = pTrack->getId().toVariant().toLongLong();
        const QList<CuePointer> cues = pTrack->getCuePoints();
        for (const CuePointer& pCue : cues) {
            if (!pCue || pCue->getHotCue() < 0) {
                continue;
            }
            QString label = pCue->getLabel().toLower();
            // "from Blow" or "< Blow" marks where this song comes in.
            // That points back, so it is not a planned transition.
            if (label.trimmed().startsWith(QLatin1Char('<')) ||
                    label.trimmed().startsWith(QLatin1String("from "))) {
                continue;
            }
            label.replace(QRegularExpression(QStringLiteral("[^\\p{L}\\p{N}]+")),
                    QStringLiteral(" "));
            label = label.simplified();
            if (label.size() < 2 || kSkip.contains(label)) {
                continue;
            }
            QStringList words;
            const QStringList parts = label.split(QLatin1Char(' '), Qt::SkipEmptyParts);
            for (const QString& word : parts) {
                if (word.size() >= 2 && word != QStringLiteral("to") &&
                        word != QStringLiteral("into") && word != QStringLiteral("feat") &&
                        word != QStringLiteral("ft")) {
                    words << word;
                }
            }
            // "Drop 2", "Intro" and other section names are not songs.
            const bool onlySectionWords = std::all_of(words.cbegin(),
                    words.cend(),
                    [](const QString& word) {
                        return kSkip.contains(word);
                    });
            if (words.isEmpty() || onlySectionWords) {
                continue;
            }
            // Exact title first, then every word in title + artist.
            QSqlQuery query(database);
            QStringList conditions;
            for (int i = 0; i < words.size(); ++i) {
                conditions << QStringLiteral(
                        "lower(coalesce(title, '') || ' ' || coalesce(artist, '')) LIKE ?");
            }
            query.prepare(QStringLiteral(
                    "SELECT id, title, artist FROM library WHERE mixxx_deleted = 0 AND id != ? AND ") +
                    conditions.join(QStringLiteral(" AND ")) +
                    QStringLiteral(" ORDER BY (lower(title) = ?) DESC, timesplayed DESC LIMIT 25"));
            query.addBindValue(playingId);
            for (const QString& word : std::as_const(words)) {
                const QString likeWord = QStringLiteral("%") + word + QStringLiteral("%");
                query.addBindValue(likeWord);
            }
            query.addBindValue(words.join(QLatin1Char(' ')));
            if (!query.exec()) {
                continue;
            }
            // Whole words only, so "Low" finds "Low" and never "Blow".
            while (query.next()) {
                QString haystack = (query.value(1).toString() + QLatin1Char(' ') +
                        query.value(2).toString())
                                           .toLower();
                haystack.replace(QRegularExpression(QStringLiteral("[^\\p{L}\\p{N}]+")),
                        QStringLiteral(" "));
                const QStringList songWords = haystack.split(QLatin1Char(' '), Qt::SkipEmptyParts);
                const bool allWords = std::all_of(words.cbegin(),
                        words.cend(),
                        [&songWords](const QString& word) {
                            return songWords.contains(word);
                        });
                if (!allWords) {
                    continue;
                }
                plannedIds.insert(query.value(0).toLongLong());
                searches << words.join(QLatin1Char(' '));
                noticeParts << QStringLiteral("%1 at cue %2")
                                       .arg(query.value(1).toString())
                                       .arg(pCue->getHotCue() + 1);
                break;
            }
        }
    }
    m_shonPlannedSearch = searches;
    BaseTrackTableModel::shonSetPlannedTrackIds(plannedIds);
    const auto widgets = QApplication::allWidgets();
    for (QWidget* pWidget : widgets) {
        if (auto* pView = qobject_cast<QAbstractItemView*>(pWidget)) {
            pView->viewport()->update();
        }
    }
    if (!noticeParts.isEmpty()) {
        emit shonNotice(QStringLiteral("Planned: %1").arg(
                noticeParts.mid(0, 3).join(QStringLiteral(", "))));
    }
}

void Library::shonApplyMatchesFilter(TrackPointer pTrack) {
    if (!pTrack || pTrack->getBpm() <= 0.0) {
        return;
    }
    // Uses Mixxx's own search. The "~" goes before the field name:
    // "~bpm:" also finds half and double time, "~key:" finds compatible keys.
    QString query = QStringLiteral("~bpm:%1").arg(pTrack->getBpm(), 0, 'f', 1);
    const auto key = pTrack->getKey();
    if (key != mixxx::track::io::key::INVALID) {
        query += QStringLiteral(" ~key:%1").arg(
                KeyUtils::keyToString(key, KeyUtils::KeyNotation::OpenKey));
    }
    // Planned transitions always show up, even if BPM or key don't fit.
    for (const QString& planned : std::as_const(m_shonPlannedSearch)) {
        query += QStringLiteral(" | ") + planned;
    }
    searchTracksInCollection(query);
}

Library::~Library() {
    DateFormatChangedBroadcaster::destroy();
}

TrackCollectionManager* Library::trackCollectionManager() const {
    // Cannot be implemented inline due to forward declarations
    return m_pTrackCollectionManager;
}

namespace {
class TrackAnalysisSchedulerEnvironmentImpl final : public TrackAnalysisSchedulerEnvironment {
  public:
    explicit TrackAnalysisSchedulerEnvironmentImpl(const Library* pLibrary)
            : m_pLibrary(pLibrary) {
        DEBUG_ASSERT(m_pLibrary);
    }
    ~TrackAnalysisSchedulerEnvironmentImpl() final = default;

    TrackPointer loadTrackById(TrackId trackId) const final {
        return m_pLibrary->trackCollectionManager()->getTrackById(trackId);
    }

  private:
    // TODO: Use std::shared_ptr or std::weak_ptr instead of a plain pointer?
    const Library* const m_pLibrary;
};
} // namespace

TrackAnalysisScheduler::Pointer Library::createTrackAnalysisScheduler(
        int numWorkerThreads,
        AnalyzerModeFlags modeFlags) const {
    return TrackAnalysisScheduler::createInstance(
            std::make_unique<const TrackAnalysisSchedulerEnvironmentImpl>(this),
            numWorkerThreads,
            m_pDbConnectionPool,
            m_pConfig,
            modeFlags);
}

void Library::stopPendingTasks() {
    if (m_pAnalysisFeature) {
        m_pAnalysisFeature->stopAnalysis();
    }
    m_pBrowseFeature->releaseBrowseThread();
}

void Library::bindSearchboxWidget(WSearchLineEdit* pSearchboxWidget) {
    connect(pSearchboxWidget,
            &WSearchLineEdit::search,
            this,
            &Library::search);
    connect(this,
            &Library::disableSearch,
            pSearchboxWidget,
            &WSearchLineEdit::slotDisableSearch);
    connect(this,
            &Library::restoreSearch,
            pSearchboxWidget,
            &WSearchLineEdit::slotRestoreSearch);
    connect(this,
            &Library::setTrackTableFont,
            pSearchboxWidget,
            &WSearchLineEdit::slotSetFont);
    emit setTrackTableFont(m_trackTableFont);
    m_pLibraryControl->bindSearchboxWidget(pSearchboxWidget);
    connect(pSearchboxWidget,
            &WSearchLineEdit::setLibraryFocus,
            m_pLibraryControl,
            &LibraryControl::setLibraryFocus);
}

void Library::bindSidebarWidget(WLibrarySidebar* pSidebarWidget) {
    const auto sidebarHoverExpandDelay =
            m_pConfig->getValue(
                    kSidebarHoverExpandDelayConfigKey,
                    kSidebarHoverExpandDelayDefault);
    pSidebarWidget->slotSetExpandOnHoverDelay(sidebarHoverExpandDelay);

    m_pLibraryControl->bindSidebarWidget(pSidebarWidget);

    // Setup the sources view
    pSidebarWidget->setModel(m_pSidebarModel);
    connect(m_pSidebarModel,
            &SidebarModel::selectIndex,
            pSidebarWidget,
            &WLibrarySidebar::selectIndex);
    connect(pSidebarWidget,
            &WLibrarySidebar::pressed,
            m_pSidebarModel,
            &SidebarModel::pressed);
    connect(pSidebarWidget,
            &WLibrarySidebar::clicked,
            m_pSidebarModel,
            &SidebarModel::clicked);
    // Lazy model: Let triangle symbol increment the model
    connect(pSidebarWidget,
            &WLibrarySidebar::expanded,
            m_pSidebarModel,
            &SidebarModel::doubleClicked);

    connect(pSidebarWidget,
            &WLibrarySidebar::rightClicked,
            m_pSidebarModel,
            &SidebarModel::rightClicked);
    connect(pSidebarWidget,
            &WLibrarySidebar::renameItem,
            m_pSidebarModel,
            &SidebarModel::renameItem);
    connect(pSidebarWidget,
            &WLibrarySidebar::deleteItem,
            m_pSidebarModel,
            &SidebarModel::deleteItem);

    connect(pSidebarWidget,
            &WLibrarySidebar::setLibraryFocus,
            m_pLibraryControl,
            &LibraryControl::setLibraryFocus);

    pSidebarWidget->slotSetFont(m_trackTableFont);
    connect(this,
            &Library::setTrackTableFont,
            pSidebarWidget,
            &WLibrarySidebar::slotSetFont);

    connect(this,
            &Library::setSidebarHoverExpandDelay,
            pSidebarWidget,
            &WLibrarySidebar::slotSetExpandOnHoverDelay);

    for (const auto& feature : std::as_const(m_features)) {
        feature->bindSidebarWidget(pSidebarWidget);
    }
}

void Library::bindLibraryWidget(
        WLibrary* pLibraryWidget, KeyboardEventFilter* pKeyboard) {
    m_pLibraryWidget = pLibraryWidget;
    WTrackTableView* pTrackTableView = new WTrackTableView(m_pLibraryWidget,
            m_pConfig,
            this,
            m_pLibraryWidget->getTrackTableBackgroundColorOpacity());
    pTrackTableView->installEventFilter(pKeyboard);
    connect(this,
            &Library::showTrackModel,
            pTrackTableView,
            &WTrackTableView::loadTrackModel);
    connect(this,
            &Library::pasteFromSidebar,
            m_pLibraryWidget,
            &WLibrary::pasteFromSidebar);
    connect(pTrackTableView,
            &WTrackTableView::loadTrack,
            this,
            &Library::slotLoadTrack);
    connect(pTrackTableView,
            &WTrackTableView::loadTrackToPlayer,
            this,
            &Library::slotLoadTrackToPlayer);
    m_pLibraryWidget->registerView(m_sTrackViewName, pTrackTableView);

    connect(m_pLibraryWidget,
            &WLibrary::setLibraryFocus,
            m_pLibraryControl,
            &LibraryControl::setLibraryFocus);
    connect(this,
            &Library::switchToView,
            m_pLibraryWidget,
            &WLibrary::switchToView);
    connect(this,
            &Library::saveModelState,
            pTrackTableView,
            &WTrackTableView::slotSaveCurrentViewState);
    connect(this,
            &Library::restoreModelState,
            pTrackTableView,
            &WTrackTableView::slotRestoreCurrentViewState);
    connect(this,
            &Library::selectTrack,
            m_pLibraryWidget,
            &WLibrary::slotSelectTrackInActiveTrackView);
    connect(pTrackTableView,
            &WTrackTableView::trackSelected,
            this,
            &Library::trackSelected);

    connect(this,
            &Library::setTrackTableFont,
            pTrackTableView,
            &WTrackTableView::setTrackTableFont);
    connect(this,
            &Library::setTrackTableRowHeight,
            pTrackTableView,
            &WTrackTableView::setTrackTableRowHeight);
    connect(this,
            &Library::setSelectedClick,
            pTrackTableView,
            &WTrackTableView::setSelectedClick);

    m_pLibraryControl->bindLibraryWidget(m_pLibraryWidget, pKeyboard);

    connect(m_pLibraryControl,
            &LibraryControl::showHideTrackMenu,
            pTrackTableView,
            &WTrackTableView::slotShowHideTrackMenu);
    connect(pTrackTableView,
            &WTrackTableView::trackMenuVisible,
            m_pLibraryControl,
            &LibraryControl::slotUpdateTrackMenuControl);

    for (const auto& feature : std::as_const(m_features)) {
        feature->bindLibraryWidget(m_pLibraryWidget, pKeyboard);
    }

    // Set the current font and row height on all the WTrackTableViews that were
    // just connected to us.
    emit setTrackTableFont(m_trackTableFont);
    emit setTrackTableRowHeight(m_iTrackTableRowHeight);
    emit setSelectedClick(m_editMetadataSelectedClick);
}

void Library::addFeature(LibraryFeature* feature) {
    VERIFY_OR_DEBUG_ASSERT(feature) {
        return;
    }
    m_features.push_back(feature);
    m_pSidebarModel->addLibraryFeature(feature);
    connect(feature,
            &LibraryFeature::pasteFromSidebar,
            this,
            &Library::pasteFromSidebar);
    connect(feature,
            &LibraryFeature::showTrackModel,
            this,
            &Library::slotShowTrackModel);
    connect(feature,
            &LibraryFeature::switchToView,
            this,
            &Library::slotSwitchToView);
    connect(feature,
            &LibraryFeature::loadTrack,
            this,
            &Library::slotLoadTrack);
    connect(feature,
            &LibraryFeature::loadTrackToPlayer,
            this,
            &Library::slotLoadTrackToPlayer);
    connect(feature,
            &LibraryFeature::restoreSearch,
            this,
            &Library::restoreSearch); // forward signal
    connect(feature,
            &LibraryFeature::disableSearch,
            this,
            &Library::disableSearch); // forward signal
    connect(feature,
            &LibraryFeature::enableCoverArtDisplay,
            this,
            &Library::enableCoverArtDisplay);
    connect(feature,
            &LibraryFeature::trackSelected,
            this,
            &Library::trackSelected);
    connect(feature,
            &LibraryFeature::saveModelState,
            this,
            &Library::saveModelState);
    connect(feature,
            &LibraryFeature::restoreModelState,
            this,
            &Library::restoreModelState);
}

void Library::onPlayerManagerTrackAnalyzerProgress(
        TrackId /*trackId*/, AnalyzerProgress /*analyzerProgress*/) {
    if (m_pAnalysisFeature) {
        m_pAnalysisFeature->suspendAnalysis();
    }
}

void Library::onPlayerManagerTrackAnalyzerIdle() {
    if (m_pAnalysisFeature) {
        m_pAnalysisFeature->resumeAnalysis();
    }
}

void Library::slotShowTrackModel(QAbstractItemModel* model) {
    // qDebug() << "Library::slotShowTrackModel" << model;
    TrackModel* trackModel = dynamic_cast<TrackModel*>(model);
    VERIFY_OR_DEBUG_ASSERT(trackModel) {
        return;
    }
    emit showTrackModel(model);
    emit switchToView(m_sTrackViewName);
    emit restoreSearch(trackModel->currentSearch());
}

void Library::slotSwitchToView(const QString& view) {
    // qDebug() << "Library::slotSwitchToView" << view;
    emit switchToView(view);
}

void Library::slotLoadTrack(TrackPointer pTrack) {
    emit loadTrack(pTrack);
}

void Library::slotLoadLocationToPlayer(const QString& location, const QString& group, bool play) {
    auto trackRef = TrackRef::fromFilePath(location);
    TrackPointer pTrack = m_pTrackCollectionManager->getOrAddTrack(trackRef);
    if (pTrack) {
#ifdef __STEM__
        emit loadTrackToPlayer(pTrack, group, mixxx::StemChannelSelection(), play);
#else
        emit loadTrackToPlayer(pTrack, group, play);
#endif
    }
}

#ifdef __STEM__
void Library::slotLoadTrackToPlayer(TrackPointer pTrack,
        const QString& group,
        mixxx::StemChannelSelection stemMask,
        bool play) {
    emit loadTrackToPlayer(pTrack, group, stemMask, play);
}
#else
void Library::slotLoadTrackToPlayer(
        TrackPointer pTrack, const QString& group, bool play) {
    emit loadTrackToPlayer(pTrack, group, play);
}
#endif

void Library::slotRefreshLibraryModels() {
    m_pMixxxLibraryFeature->refreshLibraryModels();
    m_pAnalysisFeature->refreshLibraryModels();
}

void Library::slotCreatePlaylist() {
    m_pPlaylistFeature->slotCreatePlaylist();
}

void Library::slotCreateCrate() {
    m_pCrateFeature->slotCreateCrate();
}

void Library::onSkinLoadFinished() {
    // Enable the default selection when a new skin is loaded.
    m_pSidebarModel->activateDefaultSelection();
}

bool Library::requestAddDir(const QString& dir) {
    // We only call this method if the user has picked a new directory via a
    // file dialog. This means the system sandboxer (if we are sandboxed) has
    // granted us permission to this folder. Create a security bookmark while we
    // have permission so that we can access the folder on future runs. We need
    // to canonicalize the path so we first wrap the directory string with a
    // QDir.
    QDir directory(dir);
    Sandbox::createSecurityTokenForDir(directory);

    DirectoryDAO::AddResult result =
            m_pTrackCollectionManager->addDirectory(mixxx::FileInfo(dir));
    QString error;
    switch (result) {
    case DirectoryDAO::AddResult::Ok:
        break;
    case DirectoryDAO::AddResult::AlreadyWatching:
        error = tr("This or a parent directory is already in your library.");
        break;
    case DirectoryDAO::AddResult::InvalidOrMissingDirectory:
        error = tr(
                "This or a listed directory does not exist or is inaccessible.\n"
                "Aborting the operation to avoid library inconsistencies");
        break;
    case DirectoryDAO::AddResult::UnreadableDirectory:
        error = tr(
                "This directory can not be read.");
        break;
    case DirectoryDAO::AddResult::SqlError:
        error = tr(
                "An unknown error occurred.\n"
                "Aborting the operation to avoid library inconsistencies");
        break;
    default:
        return false;
    }
    if (!error.isEmpty()) {
        QMessageBox::information(nullptr,
                tr("Can't add Directory to Library"),
                tr("Could not add <b>%1</b> to your library.\n\n%2")
                        .arg(directory.absolutePath(), error));
        return false;
    }

    return true;
}

bool Library::requestRemoveDir(const QString& dir, LibraryRemovalType removalType) {
    // Remove the directory from the directory list.
    DirectoryDAO::RemoveResult result =
            m_pTrackCollectionManager->removeDirectory(mixxx::FileInfo(dir));
    if (result != DirectoryDAO::RemoveResult::Ok) {
        switch (result) {
        case DirectoryDAO::RemoveResult::NotFound:
        case DirectoryDAO::RemoveResult::SqlError:
            QMessageBox::information(nullptr,
                    tr("Can't remove Directory from Library"),
                    tr("An unknown error occurred."));
            break;
        default:
            DEBUG_ASSERT(!"unreachable");
        }
        return false;
    }

    switch (removalType) {
    case LibraryRemovalType::KeepTracks:
        break;
    case LibraryRemovalType::HideTracks:
        // Mark all tracks in this directory as deleted but DON'T purge them
        // in case the user re-adds them manually.
        m_pTrackCollectionManager->hideAllTracks(dir);
        break;
    case LibraryRemovalType::PurgeTracks:
        // The user requested that we purge all metadata.
        m_pTrackCollectionManager->purgeAllTracks(dir);
        break;
    default:
        DEBUG_ASSERT(!"unreachable");
    }

    return true;
}

bool Library::requestRelocateDir(const QString& oldDir, const QString& newDir) {
    DirectoryDAO::RelocateResult result =
            m_pTrackCollectionManager->relocateDirectory(oldDir, newDir);
    if (result == DirectoryDAO::RelocateResult::Ok) {
        return true;
    }

    QString error;
    switch (result) {
    case DirectoryDAO::RelocateResult::InvalidOrMissingDirectory:
        error = tr(
                "This directory does not exist or is inaccessible.");
        break;
    case DirectoryDAO::RelocateResult::UnreadableDirectory:
        error = tr(
                "This directory can not be read.");
        break;
    default:
        DEBUG_ASSERT(!"unreachable");
    }
    if (!error.isEmpty()) {
        QMessageBox::information(nullptr,
                tr("Relink Directory"),
                tr("Could not relink <b>%1</b> to <b>%2</b>.\n\n%3")
                        .arg(oldDir, newDir, error));
    }
    return false;
}

void Library::setFont(const QFont& font) {
    QFontMetrics currMetrics(m_trackTableFont);
    QFontMetrics newMetrics(font);
    double currFontHeight = currMetrics.height();
    double newFontHeight = newMetrics.height();

    m_trackTableFont = font;
    emit setTrackTableFont(font);

    // adapt the previous font height/row height ratio
    int scaledRowHeight = static_cast<int>(std::round(
            (newFontHeight / currFontHeight) * m_iTrackTableRowHeight));
    setRowHeight(scaledRowHeight);
}

void Library::setRowHeight(int rowHeight) {
    m_iTrackTableRowHeight = rowHeight;
    emit setTrackTableRowHeight(rowHeight);
}

void Library::setEditMetadataSelectedClick(bool enabled) {
    m_editMetadataSelectedClick = enabled;
    emit setSelectedClick(enabled);
}

void Library::slotSearchInCurrentView() {
    m_pLibraryControl->setLibraryFocus(FocusWidget::Searchbar, Qt::ShortcutFocusReason);
}

void Library::slotSearchInAllTracks() {
    searchTracksInCollection();
}

void Library::searchTracksInCollection() {
    VERIFY_OR_DEBUG_ASSERT(m_pMixxxLibraryFeature) {
        return;
    }
    m_pMixxxLibraryFeature->selectAndActivate();
    m_pLibraryControl->setLibraryFocus(FocusWidget::Searchbar, Qt::ShortcutFocusReason);
}

void Library::searchTracksInCollection(const QString& query) {
    VERIFY_OR_DEBUG_ASSERT(m_pMixxxLibraryFeature) {
        return;
    }
    m_pMixxxLibraryFeature->searchAndActivate(query);
}

void Library::showAutoDJ() {
    m_pAutoDJFeature->activate();
    emit switchToView(kAutoDJViewName);
    // Select it but don't scroll there
    m_pSidebarModel->slotFeatureSelect(m_pAutoDJFeature, QModelIndex(), false);
}

#ifdef __ENGINEPRIME__
std::unique_ptr<mixxx::LibraryExporter> Library::makeLibraryExporter(
        QWidget* parent) {
    return std::make_unique<mixxx::LibraryExporter>(
            parent, m_pConfig, m_pTrackCollectionManager);
}
#endif

bool Library::isTrackIdInCurrentLibraryView(const TrackId& trackId) {
    VERIFY_OR_DEBUG_ASSERT(trackId.isValid()) {
        return false;
    }
    if (m_pLibraryWidget) {
        return m_pLibraryWidget->isTrackInCurrentView(trackId);
    } else {
        return false;
    }
}

void Library::slotSaveCurrentViewState() const {
    if (m_pLibraryWidget) {
        return m_pLibraryWidget->saveCurrentViewState();
    }
}

void Library::slotRestoreCurrentViewState() const {
    if (m_pLibraryWidget) {
        return m_pLibraryWidget->restoreCurrentViewState();
    }
}

LibraryTableModel* Library::trackTableModel() const {
    VERIFY_OR_DEBUG_ASSERT(m_pMixxxLibraryFeature) {
        return nullptr;
    }

    return m_pMixxxLibraryFeature->trackTableModel();
}
