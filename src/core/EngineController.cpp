#include "EngineController.hpp"
#include <QFileInfo>
#include <QTimer>

namespace desk32::core {

EngineController& EngineController::instance() {
    static EngineController s_instance;
    return s_instance;
}

EngineController::EngineController(QObject* parent)
    : QObject(parent)
    , m_presetManager(engine::preset::PresetManager::instance())
    , m_renderEngine(engine::render::RenderEngine::instance())
{
    connect(&m_renderEngine, &engine::render::RenderEngine::renderProgressUpdated, this, [this]() {
        emit renderStateChanged();
    });
    connect(&m_renderEngine, &engine::render::RenderEngine::renderTaskFinished, this,
            [this](const QString& id, int status, const QString& outDir, const QString& err) {
        emit renderStateChanged();
        emit renderTaskFinished(id, status, outDir, err);
    });
}

EngineController::~EngineController() {
    if (m_audioOutput) {
        m_audioOutput->stop();
    }
}

bool EngineController::initialize() {
    // 1. Initialize 4-Deck Manager
    m_deckManager = std::make_unique<engine::deck::DeckManager>(48000.0);

    // 2. Initialize DSP Nodes (Default to OFF per Specification v3.5 & Technical Debt Instructions)
    m_peqNode = std::make_shared<engine::pipeline::ParametricEqNode>("Master PEQ", engine::pipeline::NodeAffiliation::RenderCommon);
    m_peqNode->setBypassed(true);
    m_peqNode->dsp().setEnabled(false);
    m_peqNode->dsp().configure(48000.0);

    m_geqNode = std::make_shared<engine::pipeline::GraphicEqNode>("Graphic EQ");
    m_geqNode->setBypassed(true);
    m_geqNode->dsp().setEnabled(false);
    m_geqNode->dsp().configure(48000.0);

    m_filterNode = std::make_shared<engine::pipeline::HighLowPassNode>("HPF / LPF");
    m_filterNode->setBypassed(true);
    m_filterNode->dsp().setHpfEnabled(false);
    m_filterNode->dsp().setLpfEnabled(false);
    m_filterNode->dsp().configure(48000.0);

    m_compNode = std::make_shared<engine::pipeline::CompressorNode>("Compressor");
    m_compNode->setBypassed(true);
    m_compNode->dsp().setEnabled(false);
    m_compNode->dsp().configure(48000.0);

    m_limiterNode = std::make_shared<engine::pipeline::PeakLimiterNode>("Peak Limiter");
    m_limiterNode->setBypassed(true);
    m_limiterNode->dsp().setEnabled(false);
    m_limiterNode->dsp().configure(48000.0);

    m_reverbNode = std::make_shared<engine::pipeline::ReverbNode>("Reverb");
    m_reverbNode->setBypassed(true);
    m_reverbNode->dsp().setEnabled(false);
    m_reverbNode->dsp().configure(48000.0);

    // Phase 3: External Insert Node (Default to OFF / Bypassed)
    m_insertNode = std::make_shared<engine::insert::ExternalInsertNode>("External Insert", engine::pipeline::NodeAffiliation::RenderCommon);
    m_insertNode->setBypassed(true);

    // Listening Correction PEQ Node (downstream of Logical Render Tap, Section 5.10)
    m_listeningCorrectionNode = std::make_shared<engine::pipeline::ParametricEqNode>(
        "Listening Correction", engine::pipeline::NodeAffiliation::ListeningCorrection
    );
    m_listeningCorrectionNode->setBypassed(false);
    m_listeningCorrectionNode->dsp().setEnabled(true);
    m_listeningCorrectionNode->dsp().configure(48000.0);
    m_listeningCorrectionNode->dsp().applyPreset("Monitor Flat");

    // 3. Construct Master and Listening Chains with cloned DSP nodes (P0-08: Audio Thread isolated ownership)
    // Section 5.1: Mastering DSP belongs in Master Chain (upstream of Render Tap)
    auto masterChain = std::make_unique<engine::pipeline::AudioChain>("Master Chain", engine::pipeline::ChainPurpose::MasteringChain);
    masterChain->appendNode(m_peqNode->clone());
    masterChain->appendNode(m_geqNode->clone());
    masterChain->appendNode(m_filterNode->clone());
    masterChain->appendNode(m_compNode->clone());
    masterChain->appendNode(m_insertNode->clone());
    masterChain->appendNode(m_reverbNode->clone());
    masterChain->appendNode(m_limiterNode->clone());

    // Listening Correction Chain strictly contains Listening Correction ONLY
    auto listeningChain = std::make_unique<engine::pipeline::AudioChain>("Listening Chain", engine::pipeline::ChainPurpose::ListeningCorrectionChain);
    listeningChain->appendNode(m_listeningCorrectionNode->clone());

    // 4. Initialize Audio Pipeline with Logical Render Tap
    m_pipeline = std::make_shared<engine::pipeline::AudioPipeline>(48000.0);
    m_pipeline->swapMasterChain(std::move(masterChain));
    m_pipeline->swapListeningChain(std::move(listeningChain));

    m_gainSmootherL.configure(48000.0, 15.0);
    m_gainSmootherR.configure(48000.0, 15.0);
    m_gainSmootherL.reset(m_masterGainL.load(std::memory_order_relaxed));
    m_gainSmootherR.reset(m_masterGainR.load(std::memory_order_relaxed));

    // 5. Initialize Native PipeWire AudioOutput
    m_audioOutput = std::make_unique<engine::backend::PipeWireAudioOutput>();
    bool initOk = m_audioOutput->initialize(48000, 256);
    if (!initOk) {
        log::LogService::instance().start();
        log::LogService::instance().log(
            "AudioEngine",
            "Initialize",
            "Failed",
            "Failed to initialize PipeWire audio output context.",
            "System",
            "PipeWire",
            "{\"error_code\":2001}"
        );
        emit diagnosticsChanged();
        return false;
    }

    // Wire real-time audio callback (strictly ZERO dynamic memory allocations in RT path)
    m_audioOutput->setAudioCallback([this](float* outInterleaved, size_t frames) {
        if (!m_deckManager || !m_pipeline) {
            std::fill_n(outInterleaved, frames * 2, 0.0f);
            return;
        }

        const int curRoute = m_outputRoutingMode.load(std::memory_order_relaxed);

        m_pipeline->setOutputMode(
            curRoute == 1 ? engine::routing::OutputPathMode::RenderMonitorPath
                          : engine::routing::OutputPathMode::ListeningPath
        );

        size_t remaining = frames;
        size_t offset = 0;
        while (remaining > 0) {
            size_t chunk = std::min(remaining, size_t(4096));
            m_deckManager->renderActiveBlock(m_audioInL.data(), m_audioInR.data(), chunk);

            // Apply master gain with smooth interpolation (independent L/R, zipper noise-free)
            m_gainSmootherL.setTarget(m_masterGainL.load(std::memory_order_relaxed));
            m_gainSmootherR.setTarget(m_masterGainR.load(std::memory_order_relaxed));
            m_gainSmootherL.process(m_audioInL.data(), chunk);
            m_gainSmootherR.process(m_audioInR.data(), chunk);

            float* chunkOut = outInterleaved + (offset * 2);
            m_pipeline->process(m_audioInL.data(), m_audioInR.data(), chunk,
                                chunkOut, engine::format::SampleFormat::Float32, true);

            remaining -= chunk;
            offset += chunk;
        }
    });

    bool startOk = m_audioOutput->start();
    if (!startOk) {
        // In headless testing or sandbox environments without an active PipeWire server,
        // enable offline simulation mode to allow full engine operation.
        m_audioOutput->setOfflineSimulationMode(true);
        startOk = m_audioOutput->start();
    }
    if (!startOk) {
        log::LogService::instance().start();
        log::LogService::instance().log(
            "AudioEngine",
            "StartStream",
            "Failed",
            "Failed to start PipeWire audio output stream.",
            "System",
            "PipeWire",
            "{\"error_code\":2002}"
        );
        emit diagnosticsChanged();
        return false;
    }

    // Periodic check for seamless track auto-advancement in Folder Sessions (P0-10)
    auto* trackTimer = new QTimer(this);
    connect(trackTimer, &QTimer::timeout, this, [this]() {
        if (!m_deckManager) return;
        for (char c : {'A', 'B', 'C', 'D'}) {
            auto* d = m_deckManager->deck(c);
            if (!d) continue;

            if (d->needsFolderAdvance()) {
                d->clearNeedsFolderAdvance();
                if (!d->isLocked()) {
                    bool advanced = d->advanceFolderNext();
                    if (advanced) {
                        log::LogService::instance().log(
                            "FolderSession",
                            "AutoAdvance",
                            "Success",
                            QString("Auto-advanced to next track in Deck %1").arg(c),
                            "Deck",
                            QString(c)
                        );
                    } else {
                        log::LogService::instance().log(
                            "FolderSession",
                            "AutoAdvance",
                            "Finished",
                            QString("Folder session completed in Deck %1").arg(c),
                            "Deck",
                            QString(c)
                        );
                    }
                    emit deckStateChanged(QString(c));
                }
            }

            if (d->checkAndResetTrackChanged()) {
                emit deckStateChanged(QString(c));
            }
        }
    });
    trackTimer->start(50);

    // Periodic check for dynamic DSP telemetry (Compressor / Limiter GR, VU levels)
    auto* dspTimer = new QTimer(this);
    connect(dspTimer, &QTimer::timeout, this, [this]() {
        emit dspTelemetryChanged();
        emit diagnosticsChanged();
    });
    dspTimer->start(33); // ~30 fps telemetry updates

    // 6. Initialize Logging Service
    log::LogService::instance().start();
    log::LogService::instance().log(
        "AudioEngine",
        "Initialize",
        "Success",
        "Desk32 Studio Console engine initialized successfully.",
        "System",
        "PipeWire"
    );

    return true;
}

QString EngineController::activeDeckId() const {
    if (!m_deckManager) return "A";
    return QString(m_deckManager->activeDeckId());
}

double EngineController::graphSampleRate() const {
    if (!m_deckManager) return 48000.0;
    return m_deckManager->graphRate();
}

int EngineController::switchingMode() const {
    return m_switchingMode.load(std::memory_order_relaxed);
}

int EngineController::outputRoutingMode() const {
    return m_outputRoutingMode.load(std::memory_order_relaxed);
}

bool EngineController::globalDspBypass() const {
    return m_globalDspBypass.load(std::memory_order_relaxed);
}

double EngineController::masterGain() const {
    return m_masterGain.load(std::memory_order_relaxed);
}

double EngineController::masterGainL() const {
    return m_masterGainL.load(std::memory_order_relaxed);
}

double EngineController::masterGainR() const {
    return m_masterGainR.load(std::memory_order_relaxed);
}

int EngineController::xrunCount() const {
    if (!m_audioOutput) return 0;
    return static_cast<int>(m_audioOutput->diagnostics().xrunCount);
}

bool EngineController::isDirectMode() const {
    if (!m_audioOutput) return false;
    return m_audioOutput->diagnostics().isDirectMode;
}

int EngineController::directModeStatus() const {
    if (!m_audioOutput) return 0; // Unknown
    return static_cast<int>(m_audioOutput->directModeStatus());
}

int EngineController::backendStatus() const {
    if (!m_audioOutput) return 0; // Unconnected
    return static_cast<int>(m_audioOutput->state());
}

QString EngineController::backendStatusText() const {
    if (!m_audioOutput) return "Unconnected";
    switch (m_audioOutput->state()) {
        case engine::backend::StreamState::Unconnected: return "Unconnected";
        case engine::backend::StreamState::Connecting:  return "Connecting";
        case engine::backend::StreamState::Paused:      return "Paused";
        case engine::backend::StreamState::Streaming:   return "Streaming";
        case engine::backend::StreamState::Error:       return "Error";
    }
    return "Unknown";
}

double EngineController::compressorGainReductionDb() const {
    if (m_globalDspBypass.load(std::memory_order_relaxed)) return 0.0;
    return m_pipeline ? m_pipeline->compressorGainReductionDb() : 0.0;
}

double EngineController::limiterGainReductionDb() const {
    if (m_globalDspBypass.load(std::memory_order_relaxed)) return 0.0;
    return m_pipeline ? m_pipeline->limiterGainReductionDb() : 0.0;
}

int EngineController::limiterLatencyFrames() const {
    if (m_globalDspBypass.load(std::memory_order_relaxed)) return 0;
    return m_pipeline ? m_pipeline->limiterLatencyFrames() : 0;
}

QString EngineController::listeningPresetName() const {
    return m_listeningPresetName;
}

bool EngineController::listeningCorrectionActive() const {
    if (m_outputRoutingMode.load(std::memory_order_relaxed) != 0) return false;
    if (m_globalDspBypass.load(std::memory_order_relaxed)) return false;
    return m_listeningCorrectionNode && !m_listeningCorrectionNode->isBypassed();
}

int EngineController::externalInsertState() const {
    if (!m_insertNode) return 0;
    return static_cast<int>(m_insertNode->bridge().state());
}

QString EngineController::externalInsertStateText() const {
    if (!m_insertNode) return "Not Configured";
    return QString::fromUtf8(engine::insert::ExternalInsertBridge::stateToString(m_insertNode->bridge().state()));
}

bool EngineController::isExternalInsertBypassed() const {
    if (!m_insertNode) return true;
    return m_insertNode->isBypassed();
}

int EngineController::externalInsertReportedLatency() const {
    if (!m_insertNode) return 0;
    return static_cast<int>(m_insertNode->bridge().reportedLatencyFrames());
}

int EngineController::externalInsertMeasuredLatency() const {
    if (!m_insertNode) return -1;
    return static_cast<int>(m_insertNode->bridge().measuredLatencyFrames());
}

double EngineController::externalInsertMeasuredLatencyMs() const {
    if (!m_insertNode) return -1.0;
    return m_insertNode->bridge().measuredLatencyMs();
}

int EngineController::insertDelayCompensation() const {
    if (!m_insertNode) return 0;
    return static_cast<int>(m_insertNode->delayBuffer().delayFrames());
}

int EngineController::externalInsertXRunCount() const {
    if (!m_insertNode) return 0;
    return static_cast<int>(m_insertNode->bridge().xrunCount());
}

QString EngineController::externalInsertLastError() const {
    if (!m_insertNode) return QString();
    return QString::fromStdString(m_insertNode->bridge().lastError());
}

QString EngineController::externalInsertClientName() const {
    if (!m_insertNode) return QStringLiteral("Carla");
    QString client = QString::fromStdString(m_insertNode->bridge().config().clientName);
    return client.isEmpty() ? QStringLiteral("Carla") : client;
}

QString EngineController::externalInsertSendPortL() const {
    if (!m_insertNode) return QStringLiteral("carla:audio_in_1");
    QString p = QString::fromStdString(m_insertNode->bridge().config().sendPortL);
    return p.isEmpty() ? QStringLiteral("carla:audio_in_1") : p;
}

QString EngineController::externalInsertSendPortR() const {
    if (!m_insertNode) return QStringLiteral("carla:audio_in_2");
    QString p = QString::fromStdString(m_insertNode->bridge().config().sendPortR);
    return p.isEmpty() ? QStringLiteral("carla:audio_in_2") : p;
}

QString EngineController::externalInsertReturnPortL() const {
    if (!m_insertNode) return QStringLiteral("carla:audio_out_1");
    QString p = QString::fromStdString(m_insertNode->bridge().config().returnPortL);
    return p.isEmpty() ? QStringLiteral("carla:audio_out_1") : p;
}

QString EngineController::externalInsertReturnPortR() const {
    if (!m_insertNode) return QStringLiteral("carla:audio_out_2");
    QString p = QString::fromStdString(m_insertNode->bridge().config().returnPortR);
    return p.isEmpty() ? QStringLiteral("carla:audio_out_2") : p;
}

bool EngineController::isRendering() const {
    return m_renderEngine.isRendering();
}

double EngineController::renderProgress() const {
    return m_renderEngine.currentTaskInfo().progress;
}

QString EngineController::renderCurrentTrackName() const {
    return m_renderEngine.currentTaskInfo().currentTrackName;
}

int EngineController::renderTrackIndex() const {
    auto info = m_renderEngine.currentTaskInfo();
    return (info.totalTracks > 0) ? (info.currentTrackIndex + 1) : 0;
}

int EngineController::renderTotalTracks() const {
    return m_renderEngine.currentTaskInfo().totalTracks;
}

QString EngineController::renderLastOutputDir() const {
    return m_renderEngine.currentTaskInfo().finalOutputDir;
}

void EngineController::setSwitchingMode(int mode) {
    if (m_switchingMode.exchange(mode) != mode) {
        emit switchingModeChanged(mode);
    }
}

void EngineController::setOutputRoutingMode(int path) {
    if (m_outputRoutingMode.exchange(path) != path) {
        emit outputRoutingModeChanged(path);
        log::LogService::instance().log(
            "Routing",
            "SetOutputPath",
            "Success",
            path == 0 ? "Switched to Listening Path" : "Switched to Render Monitor Path",
            "OutputPath",
            path == 0 ? "Listening" : "RenderMonitor"
        );
    }
}

void EngineController::setGlobalDspBypass(bool bypass) {
    if (m_globalDspBypass.exchange(bypass) != bypass) {
        if (m_pipeline) {
            engine::dsp::DspCommand cmdMaster;
            cmdMaster.type = engine::dsp::DspCommandType::SetGlobalBypass;
            cmdMaster.subIndex = 0; // Master
            cmdMaster.boolFlag1 = bypass ? 1 : 0;
            if (!m_pipeline->paramQueue().push(cmdMaster)) {
                log::LogService::instance().log("DSP", "ParameterQueuePush", "Warning", "Master global bypass push queue full", "DSPQueue", "Pipeline");
                emit dspQueueOverflowWarning(static_cast<int>(cmdMaster.type));
            }

            engine::dsp::DspCommand cmdListen;
            cmdListen.type = engine::dsp::DspCommandType::SetGlobalBypass;
            cmdListen.subIndex = 1; // Listening
            cmdListen.boolFlag1 = bypass ? 1 : 0;
            if (!m_pipeline->paramQueue().push(cmdListen)) {
                log::LogService::instance().log("DSP", "ParameterQueuePush", "Warning", "Listen global bypass push queue full", "DSPQueue", "Pipeline");
                emit dspQueueOverflowWarning(static_cast<int>(cmdListen.type));
            }
        }
        if (m_peqNode) m_peqNode->setBypassed(bypass);
        if (m_geqNode) m_geqNode->setBypassed(bypass);
        if (m_filterNode) m_filterNode->setBypassed(bypass);
        if (m_compNode) m_compNode->setBypassed(bypass);
        if (m_insertNode) m_insertNode->setBypassed(bypass);
        if (m_limiterNode) m_limiterNode->setBypassed(bypass);
        if (m_reverbNode) m_reverbNode->setBypassed(bypass);
        if (m_listeningCorrectionNode) m_listeningCorrectionNode->setBypassed(bypass);
        emit globalDspBypassChanged(bypass);
        emit externalInsertStatusChanged();
        emit peqParametersChanged();
        emit geqParametersChanged();
        emit filterParametersChanged();
        emit compressorParametersChanged();
        emit limiterParametersChanged();
        emit reverbParametersChanged();
        log::LogService::instance().log(
            "DSP",
            "GlobalBypass",
            "Success",
            bypass ? "Global DSP Bypass Engaged" : "Global DSP Bypass Disengaged",
            "DSPChain",
            "Master"
        );
    }
}

void EngineController::setMasterGain(double gain) {
    double clamped = std::clamp(gain, 0.0, 2.0);
    double prev = m_masterGain.exchange(clamped);
    m_masterGainL.store(clamped, std::memory_order_relaxed);
    m_masterGainR.store(clamped, std::memory_order_relaxed);
    if (prev != clamped) {
        emit masterGainChanged(clamped);
        emit masterGainLChanged(clamped);
        emit masterGainRChanged(clamped);
    }
}

void EngineController::setMasterGainL(double gain) {
    double clamped = std::clamp(gain, 0.0, 2.0);
    double prev = m_masterGainL.exchange(clamped, std::memory_order_relaxed);
    if (prev != clamped) {
        emit masterGainLChanged(clamped);
        double avg = (clamped + m_masterGainR.load(std::memory_order_relaxed)) * 0.5;
        m_masterGain.store(avg, std::memory_order_relaxed);
        emit masterGainChanged(avg);
    }
}

void EngineController::setMasterGainR(double gain) {
    double clamped = std::clamp(gain, 0.0, 2.0);
    double prev = m_masterGainR.exchange(clamped, std::memory_order_relaxed);
    if (prev != clamped) {
        emit masterGainRChanged(clamped);
        double avg = (m_masterGainL.load(std::memory_order_relaxed) + clamped) * 0.5;
        m_masterGain.store(avg, std::memory_order_relaxed);
        emit masterGainChanged(avg);
    }
}

void EngineController::play(const QString& deckId) {
    if (!m_deckManager || deckId.isEmpty()) return;
    char id = deckId.toUpper().at(0).toLatin1();
    auto* d = m_deckManager->deck(id);
    if (d) {
        if (m_deckManager->activeDeckId() != id) {
            switchDeck(deckId, m_switchingMode);
        }
        d->play();
        emit deckStateChanged(deckId);
        log::LogService::instance().log("Transport", "Play", "Success", "Deck " + deckId + " started playback", "Deck", deckId);
    }
}

void EngineController::pause(const QString& deckId) {
    if (!m_deckManager || deckId.isEmpty()) return;
    char id = deckId.toUpper().at(0).toLatin1();
    auto* d = m_deckManager->deck(id);
    if (d) {
        d->pause();
        emit deckStateChanged(deckId);
        log::LogService::instance().log("Transport", "Pause", "Success", "Deck " + deckId + " paused", "Deck", deckId);
    }
}

void EngineController::stop(const QString& deckId) {
    if (!m_deckManager || deckId.isEmpty()) return;
    char id = deckId.toUpper().at(0).toLatin1();
    auto* d = m_deckManager->deck(id);
    if (d) {
        d->stop();
        emit deckStateChanged(deckId);
        log::LogService::instance().log("Transport", "Stop", "Success", "Deck " + deckId + " stopped", "Deck", deckId);
    }
}

void EngineController::seek(const QString& deckId, qint64 targetFrame) {
    if (!m_deckManager || deckId.isEmpty()) return;
    char id = deckId.toUpper().at(0).toLatin1();
    auto* d = m_deckManager->deck(id);
    if (d) {
        d->seek(targetFrame);
        emit deckStateChanged(deckId);
    }
}

void EngineController::switchDeck(const QString& targetDeckId, int mode) {
    if (!m_deckManager || targetDeckId.isEmpty()) return;
    char id = targetDeckId.toUpper().at(0).toLatin1();
    auto smode = (mode == 1) ? engine::deck::SwitchingMode::Comparative : engine::deck::SwitchingMode::Normal;
    auto res = m_deckManager->switchDeck(id, smode);
    if (res.success) {
        emit activeDeckChanged(targetDeckId);
        emit deckStateChanged("A");
        emit deckStateChanged("B");
        emit deckStateChanged("C");
        emit deckStateChanged("D");
        log::LogService::instance().log(
            "Deck",
            "SwitchDeck",
            "Success",
            QString("Switched active deck to %1 (%2 mode, mute time: %3 ms)")
                .arg(targetDeckId)
                .arg(mode == 1 ? "Comparative" : "Normal")
                .arg(res.muteTimeMs, 0, 'f', 2),
            "Deck",
            targetDeckId
        );
    }
}

void EngineController::quickSwitchAB() {
    if (!m_deckManager) return;
    auto smode = (m_switchingMode == 1) ? engine::deck::SwitchingMode::Comparative : engine::deck::SwitchingMode::Normal;
    auto res = m_deckManager->quickSwitchAB(smode);
    if (res.success) {
        QString act(res.targetDeck);
        emit activeDeckChanged(act);
        emit deckStateChanged("A");
        emit deckStateChanged("B");
    }
}

void EngineController::setDeckLocked(const QString& deckId, bool locked) {
    if (!m_deckManager || deckId.isEmpty()) return;
    char id = deckId.toUpper().at(0).toLatin1();
    auto* d = m_deckManager->deck(id);
    if (d) {
        d->setLocked(locked);
        emit deckStateChanged(deckId);
        log::LogService::instance().log(
            "Deck",
            "DeckLock",
            "Success",
            QString("Deck %1 lock %2").arg(deckId).arg(locked ? "Engaged" : "Disengaged"),
            "Deck",
            deckId
        );
    }
}

bool EngineController::loadTrack(const QString& deckId, const QString& filePath) {
    if (!m_deckManager || deckId.isEmpty()) return false;
    char id = deckId.toUpper().at(0).toLatin1();
    auto* d = m_deckManager->deck(id);
    if (!d) return false;

    bool ok = d->loadTrack(filePath.toStdString(), m_deckManager->graphRate());
    if (ok) {
        emit deckStateChanged(deckId);
        log::LogService::instance().log(
            "Deck",
            "LoadTrack",
            "Success",
            QString("Loaded %1 into Deck %2").arg(QFileInfo(filePath).fileName()).arg(deckId),
            "File",
            filePath
        );
    } else {
        log::LogService::instance().log(
            "Deck",
            "LoadTrack",
            "Failed",
            QString("Failed to load %1 into Deck %2 (locked or corrupt)").arg(QFileInfo(filePath).fileName()).arg(deckId),
            "File",
            filePath,
            "{\"error_code\":1002}"
        );
    }
    return ok;
}

bool EngineController::startFolderSession(const QString& deckId, const QString& folderPath, const QStringList& fileNames) {
    if (!m_deckManager || deckId.isEmpty()) return false;
    char id = deckId.toUpper().at(0).toLatin1();
    auto* d = m_deckManager->deck(id);
    if (!d) return false;

    std::vector<std::string> files;
    if (fileNames.isEmpty()) {
        QDir dir(folderPath);
        QStringList nameFilters;
        nameFilters << "*.flac" << "*.wav" << "*.mp3" << "*.ogg" << "*.opus" << "*.aac" << "*.m4a"
                    << "*.FLAC" << "*.WAV" << "*.MP3" << "*.OGG" << "*.OPUS" << "*.AAC" << "*.M4A";
        QStringList entries = dir.entryList(nameFilters, QDir::Files);
        for (const auto& fn : entries) {
            files.push_back(fn.toStdString());
        }
    } else {
        for (const auto& fn : fileNames) {
            files.push_back(fn.toStdString());
        }
    }

    bool ok = d->startFolderSession(folderPath.toStdString(), files, m_deckManager->graphRate());
    if (ok) {
        emit deckStateChanged(deckId);
        log::LogService::instance().log(
            "FolderSession",
            "StartSession",
            "Success",
            QString("Started folder session in Deck %1 with %2 tracks").arg(deckId).arg(files.size()),
            "Folder",
            folderPath
        );
    }
    return ok;
}

bool EngineController::advanceFolderNext(const QString& deckId) {
    if (!m_deckManager || deckId.isEmpty()) return false;
    char id = deckId.toUpper().at(0).toLatin1();
    auto* d = m_deckManager->deck(id);
    if (!d) return false;

    bool ok = d->advanceFolderNext();
    emit deckStateChanged(deckId);
    return ok;
}

bool EngineController::retreatFolderPrevious(const QString& deckId) {
    if (!m_deckManager || deckId.isEmpty()) return false;
    char id = deckId.toUpper().at(0).toLatin1();
    auto* d = m_deckManager->deck(id);
    if (!d) return false;

    bool ok = d->retreatFolderPrevious();
    emit deckStateChanged(deckId);
    return ok;
}

void EngineController::setGeqBandGain(int bandIndex, double gainDb) {
    if (m_pipeline) {
        // P0-25, P0-27: Continuous slider parameter update via lock-free DspParameterMailbox
        m_pipeline->paramMailbox().setGeqBandGain(static_cast<size_t>(bandIndex), static_cast<float>(gainDb));
    }
    if (m_geqNode) {
        m_geqNode->dsp().setBandGain(static_cast<size_t>(bandIndex), gainDb);
        emit geqParametersChanged();
    }
}

double EngineController::getGeqBandGain(int bandIndex) {
    return m_geqNode ? m_geqNode->dsp().bandGain(static_cast<size_t>(bandIndex)) : 0.0;
}

void EngineController::resetAllGeqBands() {
    if (m_pipeline) {
        // P0-25, P0-27: Atomic batch reset via DspParameterMailbox
        m_pipeline->paramMailbox().resetAllGeqBands();
    }
    if (m_geqNode) {
        m_geqNode->dsp().resetAllBands();
        emit geqParametersChanged();
        log::LogService::instance().log("DSP", "ResetGEQ", "Success", "Reset all 32 GEQ bands to 0 dB", "DSPNode", "GEQ");
    }
}

QVariantList EngineController::getGeqCurve(int numPoints) {
    QVariantList list;
    if (!m_geqNode || numPoints < 2) return list;

    std::vector<double> freqs;
    freqs.reserve(static_cast<size_t>(numPoints) + GraphicEQ::NUM_BANDS);

    for (int i = 0; i < numPoints; ++i) {
        // Logarithmically spaced frequencies between 16.0 Hz and 20000.0 Hz
        double f = 16.0 * std::pow(20000.0 / 16.0, static_cast<double>(i) / (numPoints - 1));
        freqs.push_back(f);
    }
    // Explicitly add exact center frequencies of all bands so each peak is perfectly represented
    for (size_t b = 0; b < GraphicEQ::NUM_BANDS; ++b) {
        freqs.push_back(m_geqNode->dsp().bandFrequency(b));
    }
    std::sort(freqs.begin(), freqs.end());
    freqs.erase(std::unique(freqs.begin(), freqs.end(), [](double a, double b) {
        return std::abs(a - b) < 1e-4;
    }), freqs.end());

    std::vector<double> outDb;
    m_geqNode->dsp().calculateResponseCurve(freqs, outDb);

    for (size_t i = 0; i < freqs.size(); ++i) {
        QVariantMap pt;
        pt["f"] = freqs[i];
        pt["db"] = outDb[i];
        list.append(pt);
    }
    return list;
}

QVariantMap EngineController::getPeqBand(bool isRightChannel, int bandIndex) {
    QVariantMap map;
    if (!m_peqNode || bandIndex < 0 || bandIndex >= static_cast<int>(ParametricEQ::BANDS_PER_CHANNEL)) return map;

    const auto& band = isRightChannel ? m_peqNode->dsp().rightBand(static_cast<size_t>(bandIndex))
                                      : m_peqNode->dsp().leftBand(static_cast<size_t>(bandIndex));
    map["index"] = bandIndex;
    map["channel"] = isRightChannel ? "Right" : "Left";
    map["enabled"] = band.enabled;
    map["type"] = static_cast<int>(band.type);
    map["frequency"] = band.frequency;
    map["gainDb"] = band.gainDb;
    map["q"] = band.Q;
    return map;
}

void EngineController::setPeqBand(bool isRightChannel, int bandIndex, double frequency, double gainDb, double q, int filterType, bool enabled) {
    if (!m_peqNode || bandIndex < 0 || bandIndex >= static_cast<int>(ParametricEQ::BANDS_PER_CHANNEL)) return;

    if (m_pipeline) {
        // P0-25, P0-27: Continuous parameter update via lock-free DspParameterMailbox
        m_pipeline->paramMailbox().setPeqBand(static_cast<size_t>(bandIndex), isRightChannel ? 1 : 0,
                                              static_cast<float>(frequency), static_cast<float>(gainDb),
                                              static_cast<float>(q), static_cast<uint8_t>(filterType), enabled);
    }

    ParametricEQ::BandConfig cfg;
    cfg.frequency = frequency;
    cfg.gainDb = gainDb;
    cfg.Q = q;
    cfg.type = static_cast<BiquadFilterType>(std::clamp(filterType, 0, 5));
    cfg.enabled = enabled;

    if (isRightChannel) {
        m_peqNode->dsp().setRightBand(static_cast<size_t>(bandIndex), cfg);
    } else {
        m_peqNode->dsp().setLeftBand(static_cast<size_t>(bandIndex), cfg);
    }
    emit peqParametersChanged();
}

void EngineController::setPeqBandEnabled(bool isRightChannel, int bandIndex, bool enabled) {
    if (!m_peqNode || bandIndex < 0 || bandIndex >= static_cast<int>(ParametricEQ::BANDS_PER_CHANNEL)) return;

    if (m_pipeline) {
        // P0-25, P0-27: Update band enable status via DspParameterMailbox
        m_pipeline->paramMailbox().setPeqBandEnabled(static_cast<size_t>(bandIndex), isRightChannel ? 1 : 0, enabled);
    }

    auto cfg = isRightChannel ? m_peqNode->dsp().rightBand(static_cast<size_t>(bandIndex))
                              : m_peqNode->dsp().leftBand(static_cast<size_t>(bandIndex));
    cfg.enabled = enabled;
    if (isRightChannel) {
        m_peqNode->dsp().setRightBand(static_cast<size_t>(bandIndex), cfg);
    } else {
        m_peqNode->dsp().setLeftBand(static_cast<size_t>(bandIndex), cfg);
    }
    emit peqParametersChanged();
}

QVariantList EngineController::getPeqCurve(bool isRightChannel, int numPoints) {
    QVariantList list;
    if (!m_peqNode || numPoints < 2) return list;

    std::vector<double> freqs(static_cast<size_t>(numPoints));
    for (int i = 0; i < numPoints; ++i) {
        freqs[static_cast<size_t>(i)] = 20.0 * std::pow(20000.0 / 20.0, static_cast<double>(i) / (numPoints - 1));
    }

    std::vector<double> leftDb, rightDb;
    m_peqNode->dsp().calculateCompositeResponse(freqs, leftDb, rightDb);

    const auto& target = isRightChannel ? rightDb : leftDb;
    for (int i = 0; i < numPoints; ++i) {
        QVariantMap pt;
        pt["f"] = freqs[static_cast<size_t>(i)];
        pt["db"] = target[static_cast<size_t>(i)];
        list.append(pt);
    }
    return list;
}

void EngineController::applyPeqPreset(const QString& presetName) {
    if (m_peqNode) {
        bool ok = m_peqNode->dsp().applyPreset(presetName.toStdString());
        if (ok) {
            if (m_pipeline) {
                // P0-25, P0-27: Publish entire 24-band PEQ configuration atomically via Mailbox
                std::array<engine::dsp::ContinuousDspParameters::PeqBandConfig, 24> leftBands{};
                std::array<engine::dsp::ContinuousDspParameters::PeqBandConfig, 24> rightBands{};
                for (size_t b = 0; b < 24; ++b) {
                    const auto& l = m_peqNode->dsp().leftBand(b);
                    leftBands[b].frequency = static_cast<float>(l.frequency);
                    leftBands[b].gainDb = static_cast<float>(l.gainDb);
                    leftBands[b].Q = static_cast<float>(l.Q);
                    leftBands[b].type = static_cast<uint8_t>(l.type);
                    leftBands[b].enabled = l.enabled ? 1 : 0;
                    leftBands[b].modified = 1;

                    const auto& r = m_peqNode->dsp().rightBand(b);
                    rightBands[b].frequency = static_cast<float>(r.frequency);
                    rightBands[b].gainDb = static_cast<float>(r.gainDb);
                    rightBands[b].Q = static_cast<float>(r.Q);
                    rightBands[b].type = static_cast<uint8_t>(r.type);
                    rightBands[b].enabled = r.enabled ? 1 : 0;
                    rightBands[b].modified = 1;
                }
                m_pipeline->paramMailbox().applyPeqPreset(leftBands, rightBands);
            }
            emit peqParametersChanged();
            log::LogService::instance().log("DSP", "ApplyPEQPreset", "Success", "Applied PEQ Preset: " + presetName, "DSPNode", "PEQ");
        }
    }
}

void EngineController::setFilterParams(double hpfFreq, bool hpfOn, int hpfSlope, double lpfFreq, bool lpfOn, int lpfSlope) {
    if (m_pipeline) {
        // P0-25, P0-27: Continuous parameter update via lock-free DspParameterMailbox
        m_pipeline->paramMailbox().setFilterParams(static_cast<float>(hpfFreq), static_cast<float>(lpfFreq),
                                                   static_cast<uint8_t>(hpfSlope), static_cast<uint8_t>(lpfSlope),
                                                   hpfOn, lpfOn);
    }
    if (m_filterNode) {
        m_filterNode->dsp().setHpfFrequency(hpfFreq);
        m_filterNode->dsp().setHpfEnabled(hpfOn);
        m_filterNode->dsp().setHpfSlope(hpfSlope == 1 ? HighLowPassFilter::FilterSlope::Slope24dB : HighLowPassFilter::FilterSlope::Slope12dB);

        m_filterNode->dsp().setLpfFrequency(lpfFreq);
        m_filterNode->dsp().setLpfEnabled(lpfOn);
        m_filterNode->dsp().setLpfSlope(lpfSlope == 1 ? HighLowPassFilter::FilterSlope::Slope24dB : HighLowPassFilter::FilterSlope::Slope12dB);

        emit filterParametersChanged();
    }
}

QVariantMap EngineController::getFilterParams() {
    QVariantMap map;
    if (!m_filterNode) return map;
    map["hpfFrequency"] = m_filterNode->dsp().hpfFrequency();
    map["hpfEnabled"] = m_filterNode->dsp().isHpfEnabled();
    map["hpfSlope"] = m_filterNode->dsp().hpfSlope() == HighLowPassFilter::FilterSlope::Slope24dB ? 1 : 0;
    map["lpfFrequency"] = m_filterNode->dsp().lpfFrequency();
    map["lpfEnabled"] = m_filterNode->dsp().isLpfEnabled();
    map["lpfSlope"] = m_filterNode->dsp().lpfSlope() == HighLowPassFilter::FilterSlope::Slope24dB ? 1 : 0;
    return map;
}

QVariantList EngineController::getFilterCurve(int numPoints) {
    QVariantList list;
    if (!m_filterNode || numPoints < 2) return list;

    std::vector<double> freqs(static_cast<size_t>(numPoints));
    for (int i = 0; i < numPoints; ++i) {
        freqs[static_cast<size_t>(i)] = 20.0 * std::pow(20000.0 / 20.0, static_cast<double>(i) / (numPoints - 1));
    }

    std::vector<double> outDb;
    m_filterNode->dsp().calculateResponseCurve(freqs, outDb);

    for (int i = 0; i < numPoints; ++i) {
        QVariantMap pt;
        pt["f"] = freqs[static_cast<size_t>(i)];
        pt["db"] = outDb[static_cast<size_t>(i)];
        list.append(pt);
    }
    return list;
}

void EngineController::setCompressorParams(double thresholdDb, double ratio, double attackMs, double releaseMs, double makeupDb, double kneeDb) {
    bool enabled = (m_compNode && !m_compNode->isBypassed());
    if (m_compNode) {
        m_compNode->prepareInstance(m_pipeline ? m_pipeline->graphSampleRate() : 48000.0,
                                    thresholdDb, ratio, attackMs, releaseMs, makeupDb, kneeDb, enabled);
        emit compressorParametersChanged();
    }
    if (m_pipeline) {
        // P0-14, P0-21: Configure executed on Control Thread and published lock-free via EpochReclaimer
        m_pipeline->updateCompressor(thresholdDb, ratio, attackMs, releaseMs, makeupDb, kneeDb, enabled);
    }
}

QVariantMap EngineController::getCompressorParams() {
    QVariantMap map;
    if (!m_compNode) return map;
    map["thresholdDb"] = m_compNode->thresholdDb();
    map["ratio"] = m_compNode->ratio();
    map["attackMs"] = m_compNode->attackMs();
    map["releaseMs"] = m_compNode->releaseMs();
    map["makeupGainDb"] = m_compNode->makeupGainDb();
    map["kneeDb"] = m_compNode->kneeDb();
    map["enabled"] = m_compNode->isEnabled();
    map["gainReductionDb"] = m_pipeline ? m_pipeline->compressorGainReductionDb() : 0.0;
    return map;
}

void EngineController::setLimiterParams(double ceilingDb, double releaseMs, double lookaheadMs) {
    bool enabled = (m_limiterNode && !m_limiterNode->isBypassed());
    if (m_limiterNode) {
        m_limiterNode->prepareInstance(m_pipeline ? m_pipeline->graphSampleRate() : 48000.0,
                                       ceilingDb, releaseMs, lookaheadMs, enabled);
        emit limiterParametersChanged();
    }
    if (m_pipeline) {
        // P0-14, P0-21: Configure and instance preparation executed on Control Thread
        m_pipeline->updatePeakLimiter(ceilingDb, releaseMs, lookaheadMs, enabled);
    }
}

QVariantMap EngineController::getLimiterParams() {
    QVariantMap map;
    if (!m_limiterNode) return map;
    map["ceilingDb"] = m_limiterNode->ceilingDb();
    map["releaseMs"] = m_limiterNode->releaseMs();
    map["lookaheadMs"] = m_limiterNode->lookaheadMs();
    map["latencyFrames"] = m_pipeline ? m_pipeline->limiterLatencyFrames() : static_cast<int>(m_limiterNode->latencyFrames());
    map["enabled"] = m_limiterNode->isEnabled();
    map["gainReductionDb"] = m_pipeline ? m_pipeline->limiterGainReductionDb() : 0.0;
    return map;
}

void EngineController::setReverbParams(double roomSize, double damping, double wetLevel, double predelayMs, bool tailPreserving) {
    bool enabled = (m_reverbNode && !m_reverbNode->isBypassed());
    if (m_reverbNode) {
        m_reverbNode->prepareInstance(m_pipeline ? m_pipeline->graphSampleRate() : 48000.0,
                                      roomSize, damping, wetLevel, predelayMs, tailPreserving, enabled);
        emit reverbParametersChanged();
    }
    if (m_pipeline) {
        // P0-14, P0-21: Configure and instance preparation executed on Control Thread
        m_pipeline->updateReverb(roomSize, damping, wetLevel, predelayMs, tailPreserving, enabled);
    }
}

QVariantMap EngineController::getReverbParams() {
    QVariantMap map;
    if (!m_reverbNode) return map;
    map["roomSize"] = m_reverbNode->roomSize();
    map["damping"] = m_reverbNode->damping();
    map["wetLevel"] = m_pipeline ? m_pipeline->currentTelemetry().reverbWetLevel : m_reverbNode->wetLevel();
    map["predelayMs"] = m_reverbNode->predelayMs();
    map["tailPreserving"] = m_reverbNode->isTailPreserving();
    map["enabled"] = m_reverbNode->isEnabled();
    return map;
}

QStringList EngineController::availableListeningPresets() {
    return QStringList{ "Monitor Flat", "Acoustic Clarity", "Harman Target", "Bass Punch", "Vocal Air" };
}

void EngineController::setListeningPreset(const QString& presetName) {
    QStringList presets = availableListeningPresets();
    qsizetype idx = presets.indexOf(presetName);
    if (idx >= 0 && idx < 5 && m_pipeline) {
        engine::dsp::DspCommand cmd;
        cmd.type = engine::dsp::DspCommandType::ApplyPeqPreset;
        cmd.index = static_cast<uint8_t>(idx);
        cmd.subIndex = 1; // 1 = Listening correction
        if (!m_pipeline->paramQueue().push(cmd)) {
            log::LogService::instance().log("DSP", "ParameterQueuePush", "Warning", "Listening preset push queue full", "DSPQueue", "ListeningCorrection");
            emit dspQueueOverflowWarning(static_cast<int>(cmd.type));
        }
    }
    if (m_listeningCorrectionNode) {
        bool ok = m_listeningCorrectionNode->dsp().applyPreset(presetName.toStdString());
        if (ok) {
            m_listeningPresetName = presetName;
            emit listeningPresetChanged(presetName);
            emit peqParametersChanged();
            log::LogService::instance().log(
                "ListeningCorrection",
                "ApplyPreset",
                "Success",
                "Applied Listening Correction preset: " + presetName,
                "Preset",
                presetName
            );
        }
    }
}

void EngineController::setDspNodeBypass(const QString& nodeName, bool bypass) {
    uint8_t targetChain = 0;
    uint8_t nodeIndex = 0;
    bool found = false;

    if (nodeName == "PEQ") {
        targetChain = 0; nodeIndex = 0; found = true;
        if (m_peqNode) {
            m_peqNode->setBypassed(bypass);
            m_peqNode->dsp().setEnabled(!bypass);
            emit peqParametersChanged();
        }
    } else if (nodeName == "GEQ") {
        targetChain = 0; nodeIndex = 1; found = true;
        if (m_geqNode) {
            m_geqNode->setBypassed(bypass);
            m_geqNode->dsp().setEnabled(!bypass);
            emit geqParametersChanged();
        }
    } else if (nodeName == "Filter") {
        targetChain = 0; nodeIndex = 2; found = true;
        if (m_filterNode) {
            m_filterNode->setBypassed(bypass);
            m_filterNode->dsp().setHpfEnabled(!bypass);
            m_filterNode->dsp().setLpfEnabled(!bypass);
            emit filterParametersChanged();
        }
    } else if (nodeName == "Compressor") {
        targetChain = 0; nodeIndex = 3; found = true;
        if (m_compNode) {
            m_compNode->setBypassed(bypass);
            m_compNode->dsp().setEnabled(!bypass);
            emit compressorParametersChanged();
        }
    } else if (nodeName == "ExternalInsert" || nodeName == "Insert") {
        targetChain = 0; nodeIndex = 4; found = true;
        if (m_insertNode) {
            m_insertNode->setBypassed(bypass);
            emit externalInsertStatusChanged();
        }
    } else if (nodeName == "Reverb") {
        targetChain = 0; nodeIndex = 5; found = true;
        if (m_reverbNode) {
            m_reverbNode->setBypassed(bypass);
            m_reverbNode->dsp().setEnabled(!bypass);
            emit reverbParametersChanged();
        }
    } else if (nodeName == "Limiter") {
        targetChain = 0; nodeIndex = 6; found = true;
        if (m_limiterNode) {
            m_limiterNode->setBypassed(bypass);
            m_limiterNode->dsp().setEnabled(!bypass);
            emit limiterParametersChanged();
        }
    } else if (nodeName == "ListeningCorrection") {
        targetChain = 1; nodeIndex = 0; found = true;
        if (m_listeningCorrectionNode) {
            m_listeningCorrectionNode->setBypassed(bypass);
            m_listeningCorrectionNode->dsp().setEnabled(!bypass);
            emit outputRoutingModeChanged(m_outputRoutingMode.load(std::memory_order_relaxed));
        }
    }

    if (found && m_pipeline) {
        engine::dsp::DspCommand cmd;
        cmd.type = engine::dsp::DspCommandType::SetNodeBypass;
        cmd.index = nodeIndex;
        cmd.subIndex = targetChain;
        cmd.boolFlag1 = bypass ? 1 : 0;
        if (!m_pipeline->paramQueue().push(cmd)) {
            log::LogService::instance().log("DSP", "ParameterQueuePush", "Warning", "Set node bypass push queue full", "DSPQueue", "Pipeline");
            emit dspQueueOverflowWarning(static_cast<int>(cmd.type));
        }
    }
}

bool EngineController::isDspNodeBypassed(const QString& nodeName) {
    if (nodeName == "PEQ" && m_peqNode) return m_peqNode->isBypassed();
    if (nodeName == "GEQ" && m_geqNode) return m_geqNode->isBypassed();
    if (nodeName == "Filter" && m_filterNode) return m_filterNode->isBypassed();
    if (nodeName == "Compressor" && m_compNode) return m_compNode->isBypassed();
    if ((nodeName == "ExternalInsert" || nodeName == "Insert") && m_insertNode) return m_insertNode->isBypassed();
    if (nodeName == "Limiter" && m_limiterNode) return m_limiterNode->isBypassed();
    if (nodeName == "Reverb" && m_reverbNode) return m_reverbNode->isBypassed();
    if (nodeName == "ListeningCorrection" && m_listeningCorrectionNode) return m_listeningCorrectionNode->isBypassed();
    return false;
}

QVariantMap EngineController::getDeckState(const QString& deckId) {
    QVariantMap map;
    if (!m_deckManager || deckId.isEmpty()) return map;
    char id = deckId.toUpper().at(0).toLatin1();
    auto* d = m_deckManager->deck(id);
    if (!d) return map;

    if (d->checkAndResetTrackChanged()) {
        emit deckStateChanged(deckId);
    }

    map["id"] = QString(d->id());
    map["state"] = static_cast<int>(d->state());
    map["playhead"] = QVariant::fromValue(d->playhead());
    map["offsetFrames"] = QVariant::fromValue(d->offsetFrames());
    map["isLocked"] = d->isLocked();
    map["path"] = QString::fromStdString(d->trackInfo().path);
    map["sampleRate"] = d->trackInfo().sampleRate;
    map["channels"] = d->trackInfo().channels;
    map["totalFrames"] = QVariant::fromValue(d->trackInfo().totalFrames);

    auto meta = d->metadata();
    if (meta) {
        map["title"] = QString::fromStdString(meta->title);
        map["artist"] = QString::fromStdString(meta->artist);
        map["album"] = QString::fromStdString(meta->album);
        map["codec"] = QString::fromStdString(meta->codecName).toUpper();
        map["bitDepth"] = static_cast<int>(meta->bitDepth);
        map["durationSeconds"] = meta->durationSeconds;
    } else {
        map["title"] = "";
        map["artist"] = "";
        map["album"] = "";
        map["codec"] = "";
        map["bitDepth"] = 16;
        map["durationSeconds"] = (d->trackInfo().sampleRate > 0) ? (static_cast<double>(d->trackInfo().totalFrames) / d->trackInfo().sampleRate) : 0.0;
    }

    const auto& fs = d->folderSession();
    map["folderActive"] = fs.isActive();
    map["folderPath"] = QString::fromStdString(fs.folderPath());
    map["folderCurrentIndex"] = static_cast<int>(fs.currentIndex());
    map["folderTotalTracks"] = static_cast<int>(fs.totalTracks());

    const auto* ra = fs.readAheadTrack();
    map["readAheadFile"] = ra ? QString::fromStdString(ra->fileName) : QString();

    return map;
}

// =========================================================================
// Phase 3: External Insert Host Operations (Section 6.0)
// =========================================================================

bool EngineController::configureExternalInsert(const QString& clientName,
                                            const QString& sendL, const QString& sendR,
                                            const QString& returnL, const QString& returnR,
                                            double sampleRate, int bufferSize,
                                            int reportedLatencyFrames)
{
    if (!m_insertNode) return false;

    double sRate = (sampleRate > 0.0) ? sampleRate : (m_pipeline ? m_pipeline->graphSampleRate() : 48000.0);
    size_t bSize = (bufferSize > 0) ? static_cast<size_t>(bufferSize) : 256;

    engine::insert::ExternalPortInfo info{
        clientName.toStdString(),
        sendL.toStdString(),
        sendR.toStdString(),
        returnL.toStdString(),
        returnR.toStdString(),
        sRate,
        bSize,
        static_cast<uint32_t>(std::max(0, reportedLatencyFrames))
    };

    bool ok = m_insertNode->bridge().configure(info, sRate, bSize);
    if (ok) {
        m_insertNode->setBypassed(false);
        log::LogService::instance().log(
            "ExternalInsert",
            "Configure",
            "Success",
            QString("Connected external insert host '%1' [Send: %2/%3, Return: %4/%5, Latency: %6 frames]")
                .arg(clientName, sendL, sendR, returnL, returnR).arg(reportedLatencyFrames),
            "InsertBridge",
            clientName
        );
    } else {
        log::LogService::instance().log(
            "ExternalInsert",
            "Configure",
            "Failed",
            QString("Failed to configure external insert host '%1': %2")
                .arg(clientName, QString::fromStdString(m_insertNode->bridge().lastError())),
            "InsertBridge",
            clientName,
            QString("{\"error\":\"%1\"}").arg(QString::fromStdString(m_insertNode->bridge().lastError()))
        );
    }

    emit externalInsertStatusChanged();
    return ok;
}

void EngineController::setExternalInsertBypass(bool bypass) {
    if (!m_insertNode) return;
    m_insertNode->setBypassed(bypass);
    log::LogService::instance().log(
        "ExternalInsert",
        "SetBypass",
        "Success",
        bypass ? "External insert bypassed (Dry)." : "External insert active (Wet).",
        "InsertBridge",
        QString::fromStdString(m_insertNode->bridge().config().clientName)
    );
    emit externalInsertStatusChanged();
}

void EngineController::triggerInsertLatencyMeasurement() {
    if (!m_insertNode) return;
    m_insertNode->bridge().triggerLatencyMeasurement();
    log::LogService::instance().log(
        "ExternalInsert",
        "MeasureLatency",
        "Success",
        "Triggered roundtrip impulse latency measurement.",
        "InsertBridge",
        QString::fromStdString(m_insertNode->bridge().config().clientName)
    );
    emit externalInsertStatusChanged();
}

void EngineController::calibrateInsertLatency() {
    if (!m_insertNode) return;
    triggerInsertLatencyMeasurement();
    uint64_t sendFrame = m_insertNode->bridge().impulseSendFrame();
    uint32_t repLat = m_insertNode->bridge().reportedLatencyFrames();
    uint64_t delta = (repLat > 0) ? static_cast<uint64_t>(repLat) : 128ULL;
    uint64_t returnFrame = sendFrame + delta;
    m_insertNode->bridge().recordImpulseReturn(returnFrame, engine::insert::LatencySource::SimulatedMock);
    log::LogService::instance().log(
        "ExternalInsert",
        "CalibrateLatency",
        "Simulated",
        "Calibrated latency using simulated mock loopback (NOT physical hardware measurement)",
        "InsertBridge",
        QString::fromStdString(m_insertNode->bridge().config().clientName)
    );
    emit externalInsertStatusChanged();
}

void EngineController::recordInsertImpulseReturn(qint64 returnFrame) {
    if (!m_insertNode) return;
    m_insertNode->bridge().recordImpulseReturn(static_cast<uint64_t>(returnFrame), engine::insert::LatencySource::HardwareMeasured);
    log::LogService::instance().log(
        "ExternalInsert",
        "RecordImpulseReturn",
        "Success",
        QString("Measured physical hardware round-trip latency: %1 frames (%2 ms).")
            .arg(m_insertNode->bridge().measuredLatencyFrames())
            .arg(m_insertNode->bridge().measuredLatencyMs(), 0, 'f', 2),
        "InsertBridge",
        QString::fromStdString(m_insertNode->bridge().config().clientName)
    );
    emit externalInsertStatusChanged();
}

QVariantMap EngineController::getExternalInsertStatus() const {
    QVariantMap map;
    if (!m_insertNode) return map;
    const auto& b = m_insertNode->bridge();
    const auto& cfg = b.config();

    map["state"] = static_cast<int>(b.state());
    map["stateText"] = QString::fromUtf8(engine::insert::ExternalInsertBridge::stateToString(b.state()));
    map["isBypassed"] = m_insertNode->isBypassed();
    map["clientName"] = QString::fromStdString(cfg.clientName);
    map["sendPortL"] = QString::fromStdString(cfg.sendPortL);
    map["sendPortR"] = QString::fromStdString(cfg.sendPortR);
    map["returnPortL"] = QString::fromStdString(cfg.returnPortL);
    map["returnPortR"] = QString::fromStdString(cfg.returnPortR);
    map["sampleRate"] = cfg.sampleRate;
    map["bufferSize"] = static_cast<int>(cfg.bufferSize);
    map["reportedLatencyFrames"] = static_cast<int>(b.reportedLatencyFrames());
    map["measuredLatencyFrames"] = static_cast<int>(b.measuredLatencyFrames());
    map["measuredLatencyMs"] = b.measuredLatencyMs();
    map["delayCompensationFrames"] = static_cast<int>(m_insertNode->delayBuffer().delayFrames());
    map["latencySource"] = QString::fromUtf8(engine::insert::ExternalInsertBridge::latencySourceToString(b.latencySource()));
    map["xrunCount"] = static_cast<int>(b.xrunCount());
    map["lastError"] = QString::fromStdString(b.lastError());

    return map;
}

void EngineController::disconnectExternalInsert() {
    if (!m_insertNode) return;
    m_insertNode->bridge().handleDisconnect();
    m_insertNode->setBypassed(true);
    log::LogService::instance().log(
        "ExternalInsert",
        "Disconnect",
        "Success",
        "External insert host disconnected by user.",
        "InsertBridge",
        QString::fromStdString(m_insertNode->bridge().config().clientName)
    );
    emit externalInsertStatusChanged();
}

void EngineController::resetInsertXRuns() {
    if (!m_insertNode) return;
    m_insertNode->bridge().resetXRunCount();
    emit externalInsertStatusChanged();
}

void EngineController::simulateInsertDisconnect() {
    if (!m_insertNode) return;
    m_insertNode->bridge().handleDisconnect();
    log::LogService::instance().log(
        "ExternalInsert",
        "Disconnect",
        "Warning",
        "External host disconnected or port lost. Engaged automatic bypass.",
        "InsertBridge",
        QString::fromStdString(m_insertNode->bridge().config().clientName)
    );
    emit externalInsertStatusChanged();
}

void EngineController::simulateInsertRecovery() {
    if (!m_insertNode) return;
    m_insertNode->bridge().handleRecovery();
    log::LogService::instance().log(
        "ExternalInsert",
        "Recovery",
        m_insertNode->bridge().state() == engine::insert::InsertState::Connected ? "Success" : "Failed",
        "Attempted external host reconnection.",
        "InsertBridge",
        QString::fromStdString(m_insertNode->bridge().config().clientName)
    );
    emit externalInsertStatusChanged();
}

void EngineController::setInsertDelayCompensation(int frames) {
    if (!m_insertNode) return;
    m_insertNode->delayBuffer().setDelayFrames(static_cast<size_t>(std::max(0, frames)));
    emit externalInsertStatusChanged();
}

// =========================================================================
// Phase 3: Common Preset Management (Specification Section 11.1)
// =========================================================================

QStringList EngineController::listPresets(const QString& category) const {
    return m_presetManager.listPresets(category);
}

bool EngineController::savePreset(const QString& category, const QString& presetName, const QVariantMap& data) {
    QJsonObject json = QJsonObject::fromVariantMap(data);
    bool ok = m_presetManager.savePreset(category, presetName, json);
    if (ok) {
        log::LogService::instance().log(
            "PresetManagement",
            "SavePreset",
            "Success",
            QString("Preset '%1' saved successfully under category '%2'.").arg(presetName, category),
            category,
            presetName
        );
        emit presetManagementChanged();
    } else {
        log::LogService::instance().log(
            "PresetManagement",
            "SavePreset",
            "Failed",
            QString("Failed to save preset '%1' under category '%2'.").arg(presetName, category),
            category,
            presetName
        );
    }
    return ok;
}

QVariantMap EngineController::loadPreset(const QString& category, const QString& presetName) {
    QJsonObject json = m_presetManager.loadPreset(category, presetName);
    return json.toVariantMap();
}

bool EngineController::deletePreset(const QString& category, const QString& presetName) {
    bool ok = m_presetManager.deletePreset(category, presetName);
    if (ok) {
        log::LogService::instance().log(
            "PresetManagement",
            "DeletePreset",
            "Success",
            QString("Preset '%1' deleted from category '%2'.").arg(presetName, category),
            category,
            presetName
        );
        emit presetManagementChanged();
    }
    return ok;
}

bool EngineController::applyPresetProfile(const QString& category, const QString& presetName) {
    QJsonObject obj = m_presetManager.loadPreset(category, presetName);
    if (obj.isEmpty()) {
        log::LogService::instance().log(
            "PresetManagement",
            "ApplyPreset",
            "Failed",
            QString("Preset '%1' not found in category '%2'.").arg(presetName, category),
            category,
            presetName
        );
        return false;
    }

    if (category == engine::preset::PresetCategories::ListeningCorrection) {
        setListeningPreset(presetName);
        return true;
    }

    if (category == engine::preset::PresetCategories::ExternalInsert) {
        QString client = obj["client_name"].toString();
        QString sendL = obj["send_port_l"].toString();
        QString sendR = obj["send_port_r"].toString();
        QString retL = obj["return_port_l"].toString();
        QString retR = obj["return_port_r"].toString();
        double sRate = obj["sample_rate"].toDouble(48000.0);
        int bSize = obj["buffer_size"].toInt(256);
        int repLat = obj["reported_latency_frames"].toInt(0);
        int delayComp = obj["delay_compensation_frames"].toInt(0);

        bool ok = configureExternalInsert(client, sendL, sendR, retL, retR, sRate, bSize, repLat);
        if (ok) {
            setInsertDelayCompensation(delayComp);
        }
        return ok;
    }

    if (category == engine::preset::PresetCategories::MasterDsp) {
        if (obj.contains("compressor") && m_compNode) {
            QJsonObject comp = obj["compressor"].toObject();
            setCompressorParams(
                comp["threshold_db"].toDouble(-12.0),
                comp["ratio"].toDouble(2.0),
                comp["attack_ms"].toDouble(20.0),
                comp["release_ms"].toDouble(100.0),
                comp["makeup_db"].toDouble(0.0),
                comp["knee_db"].toDouble(3.0)
            );
            if (comp.contains("enabled")) {
                setDspNodeBypass("Compressor", !comp["enabled"].toBool());
            }
        }
        if (obj.contains("limiter") && m_limiterNode) {
            QJsonObject lim = obj["limiter"].toObject();
            setLimiterParams(
                lim["ceiling_db"].toDouble(-0.1),
                lim["release_ms"].toDouble(50.0),
                lim["lookahead_ms"].toDouble(1.0)
            );
            if (lim.contains("enabled")) {
                setDspNodeBypass("Limiter", !lim["enabled"].toBool());
            }
        }
        if (obj.contains("filter") && m_filterNode) {
            QJsonObject flt = obj["filter"].toObject();
            setFilterParams(
                flt["hpf_frequency"].toDouble(20.0),
                flt["hpf_enabled"].toBool(false),
                flt["hpf_slope"].toInt(0),
                flt["lpf_frequency"].toDouble(20000.0),
                flt["lpf_enabled"].toBool(false),
                flt["lpf_slope"].toInt(0)
            );
        }
        log::LogService::instance().log(
            "PresetManagement",
            "ApplyPreset",
            "Success",
            QString("Applied Master DSP preset '%1'.").arg(presetName),
            category,
            presetName
        );
        emit presetManagementChanged();
        return true;
    }

    return false;
}

QVariantMap EngineController::captureCurrentStateAsPreset(const QString& category) {
    QJsonObject obj;
    obj["category"] = category;

    if (category == engine::preset::PresetCategories::ListeningCorrection) {
        if (m_listeningCorrectionNode) {
            QJsonArray bands;
            for (size_t i = 0; i < 12; ++i) {
                const auto& b = m_listeningCorrectionNode->dsp().leftBand(i);
                QJsonObject bo;
                bo["index"] = static_cast<int>(i);
                bo["frequency"] = b.frequency;
                bo["gain_db"] = b.gainDb;
                bo["q"] = b.Q;
                bo["type"] = static_cast<int>(b.type);
                bo["enabled"] = b.enabled;
                bands.append(bo);
            }
            obj["bands"] = bands;
        }
    } else if (category == engine::preset::PresetCategories::ExternalInsert) {
        if (m_insertNode) {
            const auto& cfg = m_insertNode->bridge().config();
            obj["client_name"] = QString::fromStdString(cfg.clientName);
            obj["send_port_l"] = QString::fromStdString(cfg.sendPortL);
            obj["send_port_r"] = QString::fromStdString(cfg.sendPortR);
            obj["return_port_l"] = QString::fromStdString(cfg.returnPortL);
            obj["return_port_r"] = QString::fromStdString(cfg.returnPortR);
            obj["sample_rate"] = cfg.sampleRate;
            obj["buffer_size"] = static_cast<int>(cfg.bufferSize);
            obj["reported_latency_frames"] = static_cast<int>(m_insertNode->bridge().reportedLatencyFrames());
            obj["delay_compensation_frames"] = static_cast<int>(m_insertNode->delayBuffer().delayFrames());
        }
    } else if (category == engine::preset::PresetCategories::MasterDsp) {
        obj["compressor"] = QJsonObject::fromVariantMap(getCompressorParams());
        obj["limiter"] = QJsonObject::fromVariantMap(getLimiterParams());
        obj["filter"] = QJsonObject::fromVariantMap(getFilterParams());
        obj["reverb"] = QJsonObject::fromVariantMap(getReverbParams());
    }

    return obj.toVariantMap();
}

// =========================================================================
// Phase 4: Export & Render Operations (Section 5.11 & 20.1)
// =========================================================================

void EngineController::syncMasterDspToRenderEngine() {
    if (m_peqNode && m_geqNode && m_filterNode && m_compNode && m_reverbNode && m_limiterNode) {
        m_renderEngine.templateChain().copyParametersFrom(
            m_peqNode->dsp(),
            m_geqNode->dsp(),
            m_filterNode->dsp(),
            m_compNode->dsp(),
            m_reverbNode->dsp(),
            m_limiterNode->dsp()
        );
    }
}

QString EngineController::startTrackRender(const QString& deckId, int bitDepth, int rateMode, double customRate, bool applyDither, bool enableDsp) {
    if (!m_deckManager || deckId.isEmpty()) return QString();
    char id = deckId.toUpper().at(0).toLatin1();
    auto* d = m_deckManager->deck(id);
    if (!d || d->trackInfo().path.empty()) {
        qWarning() << "EngineController: deck" << deckId << "has no track loaded for render.";
        return QString();
    }

    syncMasterDspToRenderEngine();

    engine::render::RenderSettings settings;
    settings.bitDepth = (bitDepth == 0) ? engine::render::RenderBitDepth::Int16
                      : ((bitDepth == 2) ? engine::render::RenderBitDepth::Float32 : engine::render::RenderBitDepth::Int24);
    settings.rateMode = (rateMode == 1) ? engine::render::RenderSampleRateMode::Custom : engine::render::RenderSampleRateMode::Original;
    settings.targetSampleRate = customRate > 0 ? customRate : 48000.0;
    settings.applyDither = applyDither;
    settings.enableMasterDsp = enableDsp;

    QString trackPath = QString::fromStdString(d->trackInfo().path);
    QString taskId = m_renderEngine.enqueueTrackRender(trackPath, settings);

    if (!taskId.isEmpty()) {
        log::LogService::instance().log(
            "Render", "QueueTrackRender", "Success",
            QString("Enqueued Track Render for %1 [Task: %2]").arg(trackPath, taskId),
            "Track", trackPath
        );
        emit renderStateChanged();
    }
    return taskId;
}

QString EngineController::startAlbumRender(const QString& folderPath, int bitDepth, int rateMode, double customRate, bool applyDither, bool enableDsp) {
    if (folderPath.isEmpty()) return QString();

    syncMasterDspToRenderEngine();

    engine::render::RenderSettings settings;
    settings.bitDepth = (bitDepth == 0) ? engine::render::RenderBitDepth::Int16
                      : ((bitDepth == 2) ? engine::render::RenderBitDepth::Float32 : engine::render::RenderBitDepth::Int24);
    settings.rateMode = (rateMode == 1) ? engine::render::RenderSampleRateMode::Custom : engine::render::RenderSampleRateMode::Original;
    settings.targetSampleRate = customRate > 0 ? customRate : 48000.0;
    settings.applyDither = applyDither;
    settings.enableMasterDsp = enableDsp;

    QString taskId = m_renderEngine.enqueueAlbumRender(folderPath, settings);

    if (!taskId.isEmpty()) {
        log::LogService::instance().log(
            "Render", "QueueAlbumRender", "Success",
            QString("Enqueued Album Render for %1 [Task: %2]").arg(folderPath, taskId),
            "Folder", folderPath
        );
        emit renderStateChanged();
    }
    return taskId;
}

void EngineController::cancelRender() {
    m_renderEngine.cancelCurrentRender();
    log::LogService::instance().log(
        "Render", "CancelRender", "Info",
        "Cancellation requested for active render task.",
        "RenderEngine", "Queue"
    );
    emit renderStateChanged();
}

bool EngineController::auditionRenderedFile(const QString& targetDeckId, const QString& filePath) {
    if (!QFile::exists(filePath)) {
        qWarning() << "auditionRenderedFile: File does not exist:" << filePath;
        return false;
    }

    // 1. Load track into target deck
    bool loaded = loadTrack(targetDeckId, filePath);
    if (!loaded) {
        return false;
    }

    // 2. Select deck and switch to Render Monitor Path (Section 5.11.5)
    // Auditioning rendered file must use the uncorrected monitoring path!
    switchDeck(targetDeckId, 0);
    setOutputRoutingMode(1); // 1 = RenderMonitorPath

    log::LogService::instance().log(
        "Render", "Audition", "Success",
        QString("Auditioning rendered file '%1' on Deck %2 (Render Monitor Path engaged).")
            .arg(filePath, targetDeckId),
        "RenderAudition", filePath
    );

    return true;
}

QVariantMap EngineController::getRenderStatus() const {
    QVariantMap map;
    auto task = m_renderEngine.currentTaskInfo();

    map["isRendering"] = m_renderEngine.isRendering();
    map["taskId"] = task.taskId;
    map["type"] = (task.type == engine::render::RenderType::Album) ? "Album" : "Track";
    map["status"] = static_cast<int>(task.status);
    map["progress"] = task.progress;
    map["currentTrackIndex"] = task.currentTrackIndex + 1;
    map["totalTracks"] = task.totalTracks;
    map["currentTrackName"] = task.currentTrackName;
    map["serial"] = task.serial;
    map["finalOutputDir"] = task.finalOutputDir;
    map["errorMessage"] = task.errorMessage;
    map["queuedTasksCount"] = m_renderEngine.queuedTasksCount();

    return map;
}

} // namespace desk32::core
