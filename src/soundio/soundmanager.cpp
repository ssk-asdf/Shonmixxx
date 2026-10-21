#include "soundio/soundmanager.h"

#include <qlogging.h>

#include <QLibrary>
#include <QStringList>
#include <QThread>
#include <QtGlobal>
#include <cstring> // for memcpy and strcmp
#include <memory>

#include "audio/types.h"
#include "control/controlobject.h"
#include "engine/enginemixer.h"
#include "moc_soundmanager.cpp"
#include "preferences/configobject.h"
#include "soundio/portaudioenumerator.h"
#include "soundio/sounddevice.h"
#include "soundio/sounddeviceenumerator.h"
#include "soundio/sounddevicenetwork.h"
#include "soundio/sounddevicenotfound.h"
#include "soundio/sounddeviceportaudio.h"
#include "soundio/soundmanagerconfig.h"
#include "soundio/soundmanagerutil.h"
#include "util/cmdlineargs.h"
#include "util/compatibility/qatomic.h"
#include "util/defs.h"
#include "util/sample.h"
#include "vinylcontrol/defs_vinylcontrol.h"

#ifdef __PIPEWIRE__
#include "soundio/pipewireenumerator.h"
#endif

namespace {

const QString kAppGroup = QStringLiteral("[App]");

#define CPU_OVERLOAD_DURATION 500 // in ms

struct DeviceMode {
    SoundDevicePointer pDevice;
    bool isInput;
    bool isOutput;
};

#ifdef __LINUX__
constexpr unsigned int kSleepSecondsAfterClosingDevice = 5;
#endif

// ShonDJ auto audio setup.
const ConfigKey kShonAudioAutoKey(QStringLiteral("[Shon]"), QStringLiteral("audio_auto"));
const ConfigKey kShonAudioAutoSigKey(
        QStringLiteral("[Shon]"), QStringLiteral("audio_auto_signature"));
const ConfigKey kShonAudioManualKey(QStringLiteral("[Shon]"), QStringLiteral("audio_manual"));
const ConfigKey kShonAudioManualSigKey(
        QStringLiteral("[Shon]"), QStringLiteral("audio_manual_signature"));

// API plus every output route, so two configs can be compared.
QString shonConfigSignature(const SoundManagerConfig& config) {
    QStringList parts;
    const auto outputs = config.getOutputs();
    for (auto it = outputs.constBegin(); it != outputs.constEnd(); ++it) {
        parts << it.key().debugName() + QLatin1Char('=') + it.value().getString();
    }
    parts.sort();
    return config.getAPI() + QLatin1Char('|') + parts.join(QLatin1Char(';'));
}

// How likely a device is a DJ controller or DJ sound card.
// 0 means skip, higher is better.
int shonDjDeviceScore(const QString& name, int outputChannels) {
    if (outputChannels < 4) {
        return 0;
    }
    const QString lower = name.toLower();
    static const char* const kSkip[] = {"realtek",
            "hdmi",
            "nvidia",
            "amd ",
            "intel",
            "display",
            "high definition audio",
            "asio4all",
            "flexasio",
            "generic low latency",
            "voicemeeter",
            "virtual",
            "steam",
            "oculus",
            "digital output",
            "spdif",
            "s/pdif",
            "microsoft",
            "bluetooth"};
    for (const char* pSkip : kSkip) {
        if (lower.contains(QLatin1String(pSkip))) {
            return 0;
        }
    }
    static const char* const kDj[] = {"ddj",
            "xdj",
            "pioneer",
            "alphatheta",
            "numark",
            "mixtrack",
            "party mix",
            "traktor",
            "kontrol",
            "native instruments",
            "hercules",
            "djcontrol",
            "inpulse",
            "rane",
            "denon",
            "reloop",
            "roland",
            "serato",
            "audio 2",
            "audio 4",
            "audio 6",
            "audio 10",
            "dj"};
    for (const char* pDj : kDj) {
        if (lower.contains(QLatin1String(pDj))) {
            return 2;
        }
    }
    // Unknown names: Bluetooth speakers and headphones often claim 8
    // channels (DirectSound's 7.1 mix) but only play 2, so they are not
    // DJ sound cards. Windows names them "Speakers (...)" or "Headphones (...)".
    static const char* const kNotDj[] = {"speakers (",
            "headphones (",
            "headset",
            "earphones",
            "earbuds",
            "airpods",
            "buds",
            "jbl",
            "bose",
            "beats",
            "soundcore",
            "anker",
            "sony",
            "marshall",
            "ultimate ears",
            "tribit",
            "hands-free",
            "a2dp"};
    for (const char* pNot : kNotDj) {
        if (lower.contains(QLatin1String(pNot))) {
            return 0;
        }
    }
    return 1;
}

// Outputs that are not real speakers or headphones.
[[maybe_unused]] bool shonIsVirtualOutput(const QString& lower) {
    static const char* const kVirtual[] = {"primary sound",
            "voicemeeter",
            "virtual",
            "vb-audio",
            "cable",
            "steam",
            "oculus",
            "digital output",
            "spdif",
            "s/pdif",
            "broadcast",
            "hands-free"};
    for (const char* pName : kVirtual) {
        if (lower.contains(QLatin1String(pName))) {
            return true;
        }
    }
    return false;
}

// The laptop's own speakers and headphone jack.
[[maybe_unused]] bool shonIsBuiltInOutput(const QString& lower) {
    static const char* const kBuiltIn[] = {"realtek",
            "conexant",
            "synaptics",
            "cirrus",
            "senary",
            "smart sound",
            "high definition audio device"};
    for (const char* pName : kBuiltIn) {
        if (lower.contains(QLatin1String(pName))) {
            return true;
        }
    }
    return false;
}
} // anonymous namespace

SoundManager::SoundManager(
        UserSettingsPointer pConfig, EngineMixer* pEngineMixer)
        : m_pEngineMixer(pEngineMixer),
          m_pConfig(pConfig),
          m_config(this),
          m_pErrorDevice(nullptr),
          m_underflowHappened(0),
          m_underflowUpdateCount(0),
          m_audioLatencyOverloadCount(
                  kAppGroup, QStringLiteral("audio_latency_overload_count")),
          m_audioLatencyOverload(
                  kAppGroup, QStringLiteral("audio_latency_overload")),
          m_pNetworkStream(QSharedPointer<EngineNetworkStream>::create(2, 0)),
          m_pNetworkDevice(QSharedPointer<SoundDeviceNetwork>::create(
                  pConfig, this, m_pNetworkStream)),
          m_pipewireEnabled(m_pConfig->getValue(
                  ConfigKey(kAppGroup, QStringLiteral("pipewire")), false)) {
    // TODO(xxx) some of these ControlObject are not needed by soundmanager, or are unused here.
    // It is possible to take them out?
    m_pControlObjectSoundStatusCO = new ControlObject(
            ConfigKey("[SoundManager]", "status"));
    m_pControlObjectSoundStatusCO->set(SOUNDMANAGER_DISCONNECTED);

    m_pControlObjectVinylControlGainCO = new ControlObject(
            ConfigKey(VINYL_PREF_KEY, "gain"));

#ifdef __PIPEWIRE__
    if (isPipewireSelected()) {
        m_pEnumerator = std::make_unique<PipewireEnumerator>(m_pConfig, this);
    } else
#endif
    {
        m_pEnumerator = std::make_unique<PortAudioEnumerator>(m_pConfig, this);
    }

    queryDevices();

    if (!m_config.readFromDisk()) {
        m_config.loadDefaults(this, SoundManagerConfig::API | SoundManagerConfig::OTHER);
    }

    if (!isPipewireSelected()) {
        devicesEnumerated();
    }

    checkConfig();

    // Don't write config to disk, yet -- it may be reset to defaults in case
    // previously configured devices were not found.
    // Write new config after MixxxMainWindow::noOutputDlg where the user has
    // a chance to keep the previous sound config (exit).
}

SoundManager::~SoundManager() {
    // Clean up devices.
    const bool sleepAfterClosing = false;
    clearDeviceList(sleepAfterClosing);

    // vinyl control proxies and input buffers are freed in closeDevices, called
    // by clearDeviceList -- bkgood

    delete m_pControlObjectSoundStatusCO;
    delete m_pControlObjectVinylControlGainCO;

    const QList<CSAMPLE*> buffers = m_inputBuffers.values();
    for (CSAMPLE* pBuffer : buffers) {
        if (pBuffer != nullptr) {
            SampleUtil::free(pBuffer);
        }
    }

    m_inputBuffers.clear();
}

QList<SoundDevicePointer> SoundManager::getDeviceList(
        const QString& filterAPI, bool bOutputDevices, bool bInputDevices) const {
    //qDebug() << "SoundManager::getDeviceList";

    if (filterAPI == SoundManagerConfig::kAPINone) {
        return QList<SoundDevicePointer>();
    }

    // Create a list of sound devices filtered to match given API and
    // input/output.
    QList<SoundDevicePointer> filteredDeviceList;

    for (const auto& pDevice : m_pEnumerator->queryDevices()) {
        // Skip devices that don't match the API, don't have input channels when
        // we want input devices, or don't have output channels when we want
        // output devices. If searching for both input and output devices,
        // make sure to include any devices that have >0 channels.
        const bool hasOutputs = pDevice->getNumOutputChannels().isValid();
        const bool hasInputs = pDevice->getNumInputChannels().isValid();
        qDebug() << "SoundManager::getDeviceList" << pDevice->getHostAPI()
                 << pDevice->getDeviceId().debugName() << filterAPI
                 << pDevice->getNumOutputChannels()
                 << pDevice->getNumInputChannels();
        if (pDevice->getHostAPI() != filterAPI ||
                (bOutputDevices && !bInputDevices && !hasOutputs) ||
                (bInputDevices && !bOutputDevices && !hasInputs) ||
                (!hasInputs && !hasOutputs)) {
            continue;
        }

        filteredDeviceList.push_back(pDevice);
    }

    return filteredDeviceList;
}

QList<QString> SoundManager::getHostAPIList() const {
    return m_pEnumerator->getAPIs();
}

void SoundManager::closeDevices(
        [[maybe_unused]] bool sleepAfterClosing, [[maybe_unused]] bool async) {
    // sleepAfterClosing and async maybe unused depending on platform support
    // qDebug() << "SoundManager::closeDevices()";

#ifdef __LINUX__
    bool closed = false;
#endif
    if (m_pNetworkDevice->isOpen()) {
        m_pNetworkDevice->close();
    }

    for (const auto& pDevice : m_pEnumerator->queryDevices()) {
        if (pDevice->isOpen()) {
            // NOTE(rryan): As of 2009 (?) it has been safe to close() a SoundDevice
            // while callbacks are active.
            pDevice->close();
#ifdef __LINUX__
            closed = true;
#endif
        }
    }

#ifdef __LINUX__
    if (closed && sleepAfterClosing) {
        // Sleep for 5 sec to allow asynchronously sound APIs like "pulse" to free
        // its resources as well
        if (async) {
            // Async mode - the caller will wait for `devicesClosed` before
            // trying to reconfigure or reopen audio devices
            QTimer::singleShot(
                    std::chrono::seconds(kSleepSecondsAfterClosingDevice),
                    this,
                    &SoundManager::completeDevicesClosing);
            return;
        }
        // Sync mode, legacy - we sleep the current thread for 5 seconds
        QThread::sleep(kSleepSecondsAfterClosingDevice);
    } else if (!closed)
#endif
    {
        completeDevicesClosing();
    }
}

void SoundManager::completeDevicesClosing() {
    // TODO(rryan): Should we do this before SoundDevice::close()? No! Because
    // then the callback may be running when we call
    // onInputDisconnected/onOutputDisconnected.
    std::vector<SoundDevicePointer> devices = m_pEnumerator->queryDevices();
    devices.push_back(m_pNetworkDevice);

    for (const auto& pDevice : devices) {
        for (const auto& in : pDevice->inputs()) {
            // Need to tell all registered AudioDestinations for this AudioInput
            // that the input was disconnected.
            if (!isPipewireSelected()) {
                // PipeWire manages input/output configuration itself
                unconfigureInput(in);
            }
        }
        for (const auto& out: pDevice->outputs()) {
            // Need to tell all registered AudioSources for this AudioOutput
            // that the output was disconnected.
            if (!isPipewireSelected()) {
                // PipeWire manages input/output configuration itself
                unconfigureOutput(out);
            }
        }
    }

    // Indicate to the rest of Mixxx that sound is disconnected.
    m_pControlObjectSoundStatusCO->set(SOUNDMANAGER_DISCONNECTED);
    emit devicesClosed();
}

void SoundManager::clearDeviceList(bool sleepAfterClosing) {
    //qDebug() << "SoundManager::clearDeviceList()";

    // Close the devices first.
    closeDevices(sleepAfterClosing);

    // Empty out the list of devices we currently have.
    m_pErrorDevice.clear();

    // deinitialize in case of PortAudio so the devices are updated
    if (isPipewireSelected()) {
        m_pEnumerator->deinitialize();
    }
}

QList<mixxx::audio::SampleRate> SoundManager::getSampleRates(const QString& api) const {
    QList<mixxx::audio::SampleRate> samplerates =
            m_pEnumerator->getSampleRates(api == SoundManagerConfig::kAPIJack);
    if (!samplerates.empty()) {
        return samplerates;
    }

    return {
            mixxx::audio::SampleRate(44100),
            mixxx::audio::SampleRate(48000),
            mixxx::audio::SampleRate(96000),
    };
}

QList<mixxx::audio::SampleRate> SoundManager::getSampleRates() const {
    return getSampleRates(m_config.getAPI());
}

void SoundManager::queryDevices() {
    qDebug() << "SoundManager::queryDevices()";
    m_pEnumerator->initialize();
    // now tell the prefs that we updated the device list -- bkgood
#ifndef __PIPEWIRE__
    emit devicesUpdated();
#endif
}

void SoundManager::clearAndQueryDevices() {
    const bool sleepAfterClosing = true;
    clearDeviceList(sleepAfterClosing);
    queryDevices();
}

SoundDeviceStatus SoundManager::setupDevices() {
    // NOTE(rryan): Big warning: This function is concurrent with calls to
    // pushBuffer and onDeviceOutputCallback until closeDevices() below.

    qDebug() << "SoundManager::setupDevices()";
    m_pControlObjectSoundStatusCO->set(SOUNDMANAGER_CONNECTING);
    SoundDeviceStatus status = SoundDeviceStatus::Ok;
    // NOTE(rryan): Do not clear m_pClkRefDevice here. If we didn't touch the
    // SoundDevice that is the clock reference, then it is safe to leave it as
    // it was. Clearing it causes the engine to stop being processed which
    // results in a stuttering noise (sometimes a loud buzz noise at low
    // latencies) when changing devices.
    //m_pClkRefDevice = NULL;
    m_pErrorDevice.clear();
    int outputDevicesOpened = 0;
    int inputDevicesOpened = 0;

    // NOTE(rryan): Documenting for future people touching this class. If you
    // would like to remove the fact that we close all the devices first and
    // then re-open them, I'm with you! The problem is that SoundDevicePortAudio
    // and SoundManager are not thread safe and the way that mutual exclusion
    // between the Qt main thread and the PortAudio callback thread is achieved
    // is that we shut off the PortAudio callbacks for all devices by closing
    // every device first. We then update all the SoundDevice settings
    // (configured AudioInputs/AudioOutputs) and then we re-open them.
    //
    // If you want to solve this issue, you should start by separating the PA
    // callback from the logic in SoundDevicePortAudio. They should communicate
    // via message passing over a request/response FIFO.

    // Instead of clearing m_pClkRefDevice and then assigning it directly,
    // compute the new one then atomically hand off below.
    SoundDevicePointer pNewMainClockRef;

    m_audioLatencyOverloadCount.set(0);

    // load with all configured devices.
    // all found devices are removed below
    QSet<SoundDeviceId> devicesNotFound = m_config.getDevices();

    // pair is isInput, isOutput
    QVector<DeviceMode> toOpen;
    bool haveOutput = false;
    // loop over all available devices

    std::vector<SoundDevicePointer> devices = m_pEnumerator->queryDevices();

    // here some network device conditions can be separated, currently simply
    // add it to the list of other devices
    devices.push_back(m_pNetworkDevice);
    for (const auto& pDevice : devices) {
        DeviceMode mode = {pDevice, false, false};
        pDevice->clearInputs();
        pDevice->clearOutputs();
        m_pErrorDevice = pDevice;
        const auto inputs = m_config.getInputs().values(pDevice->getDeviceId());
        for (const auto& in : inputs) {
            mode.isInput = true;
            // TODO(bkgood) look into allocating this with the frames per
            // buffer value from SMConfig
            status = pDevice->addInput(AudioInputBuffer{in, m_inputBuffers.value(in)});
            if (status != SoundDeviceStatus::Ok) {
                goto closeAndError;
            }

            if (!isPipewireSelected()) {
                // PipeWire manages input/output configuration itself
                configureInput(in);
            }
        }
        QList<AudioOutput> outputs =
                m_config.getOutputs().values(pDevice->getDeviceId());

        // Statically connect the Network Device to the Sidechain
        if (pDevice->getDeviceId().name == kNetworkDeviceInternalName) {
            AudioOutput out(AudioPathType::RecordBroadcast,
                    0,
                    mixxx::audio::ChannelCount::stereo(),
                    0);
            outputs.append(out);
            if (m_config.getForceNetworkClock() && !jackApiUsed()) {
                pNewMainClockRef = pDevice;
            }
        }

        for (const auto& out : std::as_const(outputs)) {
            mode.isOutput = true;
            if (pDevice->getDeviceId().name != kNetworkDeviceInternalName) {
                haveOutput = true;
            }
            // following keeps us from asking for a channel buffer EngineMixer
            // doesn't have -- bkgood
            const CSAMPLE* pBuffer = m_registeredSources.value(out)->buffer(out).data();
            if (pBuffer == nullptr) {
                qDebug() << "AudioSource returned null for" << out.getString();
                continue;
            }

            AudioOutputBuffer aob(out, pBuffer);
            status = pDevice->addOutput(aob);
            if (status != SoundDeviceStatus::Ok) {
                goto closeAndError;
            }

            if (!m_config.getForceNetworkClock() || jackApiUsed()) {
                if (out.getType() == AudioPathType::Main) {
                    pNewMainClockRef = pDevice;
                } else if ((out.getType() == AudioPathType::Deck ||
                                   out.getType() == AudioPathType::Bus) &&
                        !pNewMainClockRef) {
                    pNewMainClockRef = pDevice;
                }
            }

            if (!isPipewireSelected()) {
                // PipeWire manages input/output configuration itself
                configureOutput(out);
            }
        }

        if (mode.isInput || mode.isOutput) {
            pDevice->setSampleRate(m_config.getSampleRate());
            pDevice->setConfigFramesPerBuffer(m_config.getFramesPerBuffer());
            toOpen.append(mode);
        }
    }

    for (const auto& mode: toOpen) {
        SoundDevicePointer pDevice = mode.pDevice;
        m_pErrorDevice = pDevice;

        // Don't use network clock unless forced with PipeWire
        // as PipeWire callbacks run even without any devices configured
        const bool isNetworkDevice = pDevice->getDeviceId().name == kNetworkDeviceInternalName;
        const bool deviceAllowedClockReference = isPipewireSelected() != isNetworkDevice;

        // If we have not yet set a clock source then we use the first
        // output pDevice
        if (pNewMainClockRef.isNull() &&
                (!haveOutput || mode.isOutput) && deviceAllowedClockReference) {
            pNewMainClockRef = pDevice;
            qWarning() << "Output sound device clock reference not set! Using"
                       << pDevice->getDisplayName();
        }

        int syncBuffers = m_config.getSyncBuffers();
        // If we are in safe mode and using experimental polling support, use
        // the default of 2 sync buffers instead.
        if (CmdlineArgs::Instance().getSafeMode() && syncBuffers == 0) {
            syncBuffers = 2;
        }
        status = pDevice->open(pNewMainClockRef == pDevice, syncBuffers);
        if (status != SoundDeviceStatus::Ok) {
            goto closeAndError;
        }
        devicesNotFound.remove(pDevice->getDeviceId());
        if (mode.isOutput) {
            ++outputDevicesOpened;
        }
        if (mode.isInput) {
            ++inputDevicesOpened;
        }
    }

    if (pNewMainClockRef) {
        qDebug() << "Using" << pNewMainClockRef->getDisplayName()
                 << "as output sound device clock reference";
    } else {
        qWarning() << "No output devices opened, no clock reference device set";
    }

    qDebug() << outputDevicesOpened << "output sound devices opened";
    qDebug() << inputDevicesOpened << "input sound devices opened";
    for (const auto& device : std::as_const(devicesNotFound)) {
        qWarning() << device << "not found";
    }

    m_pControlObjectSoundStatusCO->set(
            outputDevicesOpened > 0 ?
                    SOUNDMANAGER_CONNECTED : SOUNDMANAGER_DISCONNECTED);

    // returns OK if we were able to open all the devices the user wanted
    if (devicesNotFound.isEmpty() || pipewireSkipConfig()) {
        emit devicesSetup();
        return SoundDeviceStatus::Ok;
    }
    m_pErrorDevice = SoundDevicePointer(
            new SoundDeviceNotFound(devicesNotFound.constBegin()->name));
    return SoundDeviceStatus::ErrorDeviceCount;

closeAndError:
    const bool sleepAfterClosing = false;
    closeDevices(sleepAfterClosing);
    return status;
}

SoundDevicePointer SoundManager::getErrorDevice() const {
    return m_pErrorDevice;
}

QString SoundManager::getErrorDeviceName() const {
    SoundDevicePointer pDevice = getErrorDevice();
    if (pDevice) {
        return pDevice->getDisplayName();
    }
    return tr("a device");
}

QString SoundManager::getLastErrorMessage(SoundDeviceStatus status) const {
    QString error;
    QString deviceName(tr("a device"));
    QString detailedError(tr("An unknown error occurred"));
    SoundDevicePointer pDevice = getErrorDevice();
    if (pDevice) {
        deviceName = pDevice->getDisplayName();
        detailedError = pDevice->getError();
    }
    switch (status) {
    case SoundDeviceStatus::ErrorDuplicateOutputChannel:
        error = tr("Two outputs cannot share channels on \"%1\"").arg(deviceName);
        break;
    default:
        error = tr("Error opening \"%1\"").arg(deviceName) + "\n" + detailedError;
        break;
    }
    return error;
}

SoundManagerConfig SoundManager::getConfig() const {
    return m_config;
}

void SoundManager::closeActiveConfig(bool async) {
    // Close open devices. After this call we will not get any more
    // onDeviceOutputCallback() or pushBuffer() calls because all the
    // SoundDevices are closed. closeDevices() blocks and can take a while.
    const bool sleepAfterClosing = true;
    closeDevices(sleepAfterClosing, async);
}

SoundDeviceStatus SoundManager::setConfig(const SoundManagerConfig& config) {
    SoundDeviceStatus status = SoundDeviceStatus::Ok;
    m_config = config;
    checkConfig();

    closeActiveConfig();

    status = setupDevices();
    if (status == SoundDeviceStatus::Ok) {
        m_config.writeToDisk();
        if (!m_shonApplying) {
            // Set by hand in Preferences. Auto setup leaves it alone until
            // one of these devices goes away.
            const QString signature = shonConfigSignature(m_config);
            const bool manual = signature !=
                    m_pConfig->getValueString(kShonAudioAutoSigKey);
            m_pConfig->setValue(kShonAudioManualKey, manual ? 1 : 0);
            m_pConfig->set(kShonAudioManualSigKey, ConfigValue(signature));
        }
    }
    return status;
}

SoundManagerConfig SoundManager::shonBuildAutoConfig(QString* pDescription, bool laptopOnly) {
    SoundManagerConfig config = m_config;
    const QList<QString> apis = getHostAPIList();

    SoundDevicePointer pBest;
    QString bestApi;
    if (!laptopOnly) {
        // Low latency APIs first.
        QStringList order = {SoundManagerConfig::kAPIAsio,
                QStringLiteral("Windows WASAPI"),
                SoundManagerConfig::kAPIDirectSound,
                QStringLiteral("MME"),
                SoundManagerConfig::kAPICoreAudio,
                SoundManagerConfig::kAPIJack,
                SoundManagerConfig::kAPIAlsa};
        for (const auto& api : apis) {
            if (!order.contains(api)) {
                order << api;
            }
        }
        int bestScore = 0;
        for (const auto& api : std::as_const(order)) {
            if (!apis.contains(api)) {
                continue;
            }
            const QList<SoundDevicePointer> devices = getDeviceList(api, true, false);
            for (const auto& pDevice : devices) {
                const int score = shonDjDeviceScore(pDevice->getDisplayName(),
                        static_cast<int>(pDevice->getNumOutputChannels().value()));
                if (score > bestScore) {
                    bestScore = score;
                    pBest = pDevice;
                    bestApi = api;
                }
            }
        }
    }

    if (pBest) {
        // DJ controller or sound card: Main on 1-2, Headphones on 3-4.
        config.setAPI(bestApi);
        config.clearOutputs();
        config.clearInputs();
        config.addOutput(pBest->getDeviceId(),
                AudioOutput(AudioPathType::Main,
                        0,
                        mixxx::audio::ChannelCount::stereo(),
                        0));
        config.addOutput(pBest->getDeviceId(),
                AudioOutput(AudioPathType::Headphones,
                        2,
                        mixxx::audio::ChannelCount::stereo(),
                        0));
        const QList<mixxx::audio::SampleRate> rates = getSampleRates(bestApi);
        const mixxx::audio::SampleRate deviceRate = pBest->getDefaultSampleRate();
        if (rates.contains(deviceRate)) {
            config.setSampleRate(deviceRate);
        } else if (!rates.isEmpty() && !rates.contains(config.getSampleRate())) {
            config.setSampleRate(rates.first());
        }
        if (pDescription) {
            *pDescription = QStringLiteral("Audio: %1 (Main 1-2, Headphones 3-4)")
                                    .arg(pBest->getDisplayName());
        }
        return config;
    }

#ifdef __WINDOWS__
    // Laptop plus outside speakers (USB sound card, HDMI TV): Main goes to
    // the speakers and the laptop's own jack becomes the headphone cue.
    if (!laptopOnly && apis.contains(SoundManagerConfig::kAPIDirectSound)) {
        SoundDevicePointer pBuiltIn;
        SoundDevicePointer pExternal;
        const QList<SoundDevicePointer> devices =
                getDeviceList(SoundManagerConfig::kAPIDirectSound, true, false);
        for (const auto& pDevice : devices) {
            if (pDevice->getNumOutputChannels().value() < 2) {
                continue;
            }
            const QString lower = pDevice->getDisplayName().toLower();
            if (shonIsVirtualOutput(lower)) {
                continue;
            }
            if (shonIsBuiltInOutput(lower)) {
                if (!pBuiltIn) {
                    pBuiltIn = pDevice;
                }
            } else if (!pExternal) {
                pExternal = pDevice;
            }
        }
        if (pBuiltIn && pExternal) {
            config.setAPI(SoundManagerConfig::kAPIDirectSound);
            config.clearOutputs();
            config.clearInputs();
            config.addOutput(pExternal->getDeviceId(),
                    AudioOutput(AudioPathType::Main,
                            0,
                            mixxx::audio::ChannelCount::stereo(),
                            0));
            config.addOutput(pBuiltIn->getDeviceId(),
                    AudioOutput(AudioPathType::Headphones,
                            0,
                            mixxx::audio::ChannelCount::stereo(),
                            0));
            const QList<mixxx::audio::SampleRate> rates =
                    getSampleRates(SoundManagerConfig::kAPIDirectSound);
            const mixxx::audio::SampleRate rate = pExternal->getDefaultSampleRate();
            if (rates.contains(rate)) {
                config.setSampleRate(rate);
            }
            if (pDescription) {
                *pDescription = QStringLiteral("Audio: %1 (cue on the laptop jack)")
                                        .arg(pExternal->getDisplayName());
            }
            return config;
        }
    }
#endif

    // Laptop only: Main on the system output, no headphone cue.
#ifdef __WINDOWS__
    if (apis.contains(SoundManagerConfig::kAPIDirectSound)) {
        config.setAPI(SoundManagerConfig::kAPIDirectSound);
        config.loadDefaults(this, SoundManagerConfig::DEVICES | SoundManagerConfig::OTHER);
    } else {
        config.loadDefaults(this,
                SoundManagerConfig::API | SoundManagerConfig::DEVICES |
                        SoundManagerConfig::OTHER);
    }
#else
    config.loadDefaults(this,
            SoundManagerConfig::API | SoundManagerConfig::DEVICES |
                    SoundManagerConfig::OTHER);
#endif
    if (pDescription) {
        QString name;
        const auto outputs = config.getOutputs();
        if (!outputs.isEmpty()) {
            name = outputs.constBegin().key().name;
        }
        *pDescription = name.isEmpty()
                ? QStringLiteral("Audio: no output found")
                : QStringLiteral("Audio: %1 (laptop mode, no headphone cue)").arg(name);
    }
    return config;
}

bool SoundManager::shonConfigDevicesExist(const SoundManagerConfig& config) const {
    const auto outputs = config.getOutputs();
    if (outputs.isEmpty()) {
        return false;
    }
    const QList<SoundDevicePointer> devices = getDeviceList(config.getAPI(), true, true);
    for (auto it = outputs.constBegin(); it != outputs.constEnd(); ++it) {
        bool found = false;
        for (const auto& pDevice : devices) {
            if (pDevice->getDeviceId() == it.key()) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

void SoundManager::shonRememberAutoConfig() {
    m_pConfig->set(kShonAudioAutoSigKey, ConfigValue(shonConfigSignature(m_config)));
    m_pConfig->setValue(kShonAudioManualKey, 0);
}

QString SoundManager::shonAutoConfigureAtStartup() {
    if (!m_pConfig->getValue(kShonAudioAutoKey, true) || isPipewireSelected()) {
        return QString();
    }
    const QString current = shonConfigSignature(m_config);
    if (m_pConfig->getValue(kShonAudioManualKey, false) &&
            current == m_pConfig->getValueString(kShonAudioManualSigKey)) {
        // Hand picked setup is still there, keep it.
        return QString();
    }
    QString description;
    const SoundManagerConfig config = shonBuildAutoConfig(&description, false);
    const bool changed = shonConfigSignature(config) != current;
    if (changed) {
        m_config = config;
        checkConfig();
        m_config.writeToDisk();
    }
    shonRememberAutoConfig();
    return changed ? description : QString();
}

QString SoundManager::shonFallbackToLaptop() {
    if (!m_pConfig->getValue(kShonAudioAutoKey, true) || isPipewireSelected()) {
        return QString();
    }
    const SoundManagerConfig saved = m_config;
    QString description;
    closeDevices(false);
    m_pErrorDevice.clear();
    m_config = shonBuildAutoConfig(&description, true);
    checkConfig();
    if (setupDevices() != SoundDeviceStatus::Ok) {
        m_config = saved;
        return QString();
    }
    // Not written to disk: the saved setup stays the one to come back to.
    m_shonPendingConfig = saved;
    return description.isEmpty() ? QStringLiteral("Audio: laptop speakers") : description;
}

QString SoundManager::shonHandleDeviceChange() {
    if (!m_pConfig->getValue(kShonAudioAutoKey, true) || isPipewireSelected()) {
        return QString();
    }
    const QString before = shonConfigSignature(m_config);

    // PortAudio only sees new devices after a restart of its device list.
    closeDevices(false);
    m_pErrorDevice.clear();
    m_pEnumerator->deinitialize();
    m_pEnumerator->initialize();
    emit devicesUpdated();

    // Playing on the laptop because the saved speaker was off: try the
    // saved setup again now that something changed.
    if (m_shonPendingConfig) {
        const SoundManagerConfig laptop = m_config;
        m_config = *m_shonPendingConfig;
        checkConfig();
        if (setupDevices() == SoundDeviceStatus::Ok) {
            m_shonPendingConfig.reset();
            return QStringLiteral("Audio: back on your saved speaker");
        }
        closeDevices(false);
        m_pErrorDevice.clear();
        m_config = laptop;
        setupDevices();
        return QString();
    }

    if (m_pConfig->getValue(kShonAudioManualKey, false) && shonConfigDevicesExist(m_config)) {
        // Hand picked devices are all still here, just reopen them.
        setupDevices();
        return QString();
    }

    QString description;
    SoundManagerConfig config = shonBuildAutoConfig(&description, false);
    m_shonApplying = true;
    SoundDeviceStatus status = setConfig(config);
    if (status != SoundDeviceStatus::Ok) {
        // Could not open the DJ device, fall back to the laptop output.
        config = shonBuildAutoConfig(&description, true);
        status = setConfig(config);
    }
    m_shonApplying = false;
    if (status != SoundDeviceStatus::Ok) {
        return QStringLiteral("Audio: could not open a device, check Preferences > Sound Hardware");
    }
    shonRememberAutoConfig();
    return shonConfigSignature(m_config) != before ? description : QString();
}

void SoundManager::checkConfig() {
    if (!m_config.checkAPI()) {
        m_config.setAPI(SoundManagerConfig::kAPINone);
        m_config.loadDefaults(this, SoundManagerConfig::API | SoundManagerConfig::DEVICES);
    }
    if (!m_config.checkSampleRate(*this)) {
        m_config.setSampleRate(SoundManagerConfig::kFallbackSampleRate);
        m_config.loadDefaults(this, SoundManagerConfig::OTHER);
    }

    // Even if we have a two-deck skin, if someone has configured a deck > 2
    // then the configuration needs to know about that extra deck.
    m_config.setCorrectDeckCount(getConfiguredDeckCount());
    // latency checks itself for validity on SMConfig::setLatency()
}

void SoundManager::onDeviceOutputCallback(const SINT iFramesPerBuffer) {
    // Produce a block of samples for output. EngineMixer expects stereo
    // samples so multiply iFramesPerBuffer by 2.
    m_pEngineMixer->process(iFramesPerBuffer * 2);
}

void SoundManager::pushInputBuffer(const AudioInput& input, const SINT iFramesPerBuffer) {
    CSAMPLE* pInputBuffer = m_inputBuffers.value(input);
    for (auto it = m_registeredDestinations.constFind(input);
            it != m_registeredDestinations.constEnd() && it.key() == input;
            ++it) {
        it.value()->receiveBuffer(input, pInputBuffer, iFramesPerBuffer);
    }
}
void SoundManager::pushInputBuffers(const QList<AudioInputBuffer>& inputs,
                                    const SINT iFramesPerBuffer) {
   for (QList<AudioInputBuffer>::ConstIterator i = inputs.begin(),
                 e = inputs.end(); i != e; ++i) {
        const AudioInputBuffer& in = *i;
        CSAMPLE* pInputBuffer = in.getBuffer();
        for (auto it = m_registeredDestinations.constFind(in);
                it != m_registeredDestinations.constEnd() && it.key() == in;
                ++it) {
            it.value()->receiveBuffer(in, pInputBuffer, iFramesPerBuffer);
        }
   }
}

void SoundManager::writeProcess(SINT framesPerBuffer) const {
    for (const auto& pDevice : m_pEnumerator->queryDevices()) {
        if (pDevice) {
            pDevice->writeProcess(framesPerBuffer);
        }
    }
    m_pNetworkDevice->writeProcess(framesPerBuffer);
}

void SoundManager::readProcess(SINT framesPerBuffer) const {
    for (const auto& pDevice : m_pEnumerator->queryDevices()) {
        if (pDevice) {
            pDevice->readProcess(framesPerBuffer);
        }
    }
    m_pNetworkDevice->readProcess(framesPerBuffer);
}

void SoundManager::registerOutput(const AudioOutput& output, AudioSource* src) {
    VERIFY_OR_DEBUG_ASSERT(!m_registeredSources.contains(output)) {
        return;
    }
    m_registeredSources.insert(output, src);
    emit outputRegistered(output, src);
}

void SoundManager::registerInput(const AudioInput& input, AudioDestination* dest) {
    // Vinyl control inputs are registered twice, once for timecode and once for
    // passthrough, each with different outputs. So unlike outputs, do not assert
    // that the input has not been registered yet.
    m_registeredDestinations.insert(input, dest);

    if (!m_inputBuffers.contains(input)) {
        m_inputBuffers.insert(input, SampleUtil::alloc(kMaxEngineSamples));
    }

    emit inputRegistered(input, dest);
}

QList<AudioOutput> SoundManager::registeredOutputs() const {
    return m_registeredSources.keys();
}

QList<AudioInput> SoundManager::registeredInputs() const {
    return m_registeredDestinations.keys();
}

void SoundManager::setConfiguredDeckCount(int count) {
    if (getConfiguredDeckCount() == count) {
        // Unchanged
        return;
    }
    m_config.setDeckCount(count);
    checkConfig();
    m_config.writeToDisk();
}

int SoundManager::getConfiguredDeckCount() const {
    return m_config.getDeckCount();
}

void SoundManager::processUnderflowHappened(SINT framesPerBuffer) {
    if (m_underflowUpdateCount == 0) {
        if (atomicLoadRelaxed(m_underflowHappened)) {
            m_audioLatencyOverload.set(1.0);
            m_audioLatencyOverloadCount.set(
                    m_audioLatencyOverloadCount.get() + 1);
            m_underflowUpdateCount = CPU_OVERLOAD_DURATION *
                    m_config.getSampleRate() / framesPerBuffer / 1000;

            m_underflowHappened = 0; // resetting here is not thread safe,
                                     // but that is OK, because we count only
                                     // 1 underflow each 500 ms
        } else {
            m_audioLatencyOverload.set(0.0);
        }
    } else {
        --m_underflowUpdateCount;
    }
}

void SoundManager::addDevice(SoundDevicePointer pDevice) {
    qDebug() << "SoundManager::addDevice" << pDevice->getDisplayName();
    emit deviceAdded(pDevice);
}

void SoundManager::removeDevice(SoundDevicePointer pDevice) {
    qDebug() << "SoundManager::removeDevice" << pDevice->getDisplayName();
    emit deviceRemoved(pDevice);
}

void SoundManager::updateDeviceChannels(SoundDevicePointer pDevice) {
    emit deviceChannelsUpdated(pDevice);
}

void SoundManager::configureInput(const AudioInput& input) {
    // Check if any AudioDestination is registered for this AudioInput
    // and call the onInputConnected method.
    qWarning() << "SoundManager::configureInput";
    auto [first, last] = m_registeredDestinations.equal_range(input);

    for (auto it = first; it != last; ++it) {
        it.value()->onInputConfigured(input);
        m_pEngineMixer->onInputConnected(input);
    }
}

void SoundManager::unconfigureInput(const AudioInput& input) {
    qWarning() << "SoundManager::unconfigureInput";
    auto [first, last] = m_registeredDestinations.equal_range(input);
    for (auto it = first; it != last; ++it) {
        it.value()->onInputUnconfigured(input);
        m_pEngineMixer->onInputDisconnected(input);
    }
}

void SoundManager::configureOutput(const AudioOutput& output) {
    // Check if any AudioSource is registered for this AudioOutput and
    // call the onOutputConnected method.
    qWarning() << "SoundManager::configureOutput";
    m_registeredSources.value(output)->onOutputConnected(output);
}

void SoundManager::unconfigureOutput(const AudioOutput& output) {
    qWarning() << "SoundManager::unconfigureOutput";
    m_registeredSources.value(output)->onOutputDisconnected(output);
}

void SoundManager::devicesEnumerated() {
    qDebug() << "SoundManager::devicesEnumerated";
    for (const auto& device : m_pEnumerator->queryDevices()) {
        qDebug() << device->getDisplayName();
    }

    // currently PipeWire has no sane default device options, we need to obtain
    // it from metadata, and select it in SoundManagerConfig::loadDefaults
    if (!m_config.validateDevices() && !isPipewireSelected()) {
        qDebug() << "!m_config.validateDevices()";
        m_config.loadDefaults(this, SoundManagerConfig::DEVICES);
    }
}

void SoundManager::invalidateConfig() {
    qDebug() << "SoundManager::invalidateConfig";
    emit configInvalidated();
}
