#include "EngineController.hpp"
#include "core/log/LogErrorCodes.hpp"
#include <QClipboard>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSysInfo>
#include <QTimer>
#include <cerrno>
#include <chrono>
#include <cstring>

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
    connect(&m_renderEngine, &engine::render::RenderEngine::renderTaskStarted, this,
            [this](const QString& id, const QString& name, int totalTracks) {
        emit renderStateChanged();
        emit renderTaskStarted(id, name, totalTracks);
    });
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

bool EngineController::initialize(bool offlineMode) {
    // 1. Initialize 4-Deck Manager
    m_deckManager = std::make_unique<engine::deck::DeckManager>(48000.0);

    // 2. Initialize Control Thread DSP Parameter Snapshots (P0-21)
    m_uiPeq.configure(48000.0);
    m_uiPeq.setEnabled(false);

    m_uiGeq.configure(48000.0);
    m_uiGeq.setEnabled(false);

    m_uiFilter.configure(48000.0);
    m_uiFilter.setHpfEnabled(false);
    m_uiFilter.setLpfEnabled(false);

    m_uiComp.configure(48000.0);
    m_uiComp.setEnabled(false);

    m_uiLimiter.configure(48000.0);
    m_uiLimiter.setEnabled(false);

    m_uiReverb.configure(48000.0);
    m_uiReverb.setEnabled(false);

    m_uiListeningCorrection.configure(48000.0);
    m_uiListeningCorrection.setEnabled(true);
    m_uiListeningCorrection.applyPreset("Monitor Flat");

    // Phase 3: External Insert Node (Default to OFF / Bypassed)
    m_insertNode = std::make_shared<engine::insert::ExternalInsertNode>("External Insert", engine::pipeline::NodeAffiliation::RenderCommon);
    m_insertNode->setBypassed(true);

    // 3. Construct Master and Listening Chains with isolated DSP nodes (P0-08, P0-21)
    // Section 5.1: Mastering DSP belongs in Master Chain (upstream of Render Tap)
    // Initial state: optional master DSPs start bypassed matching spec §5.3-5.6 and EngineController model.
    auto masterChain = std::make_unique<engine::pipeline::AudioChain>("Master Chain", engine::pipeline::ChainPurpose::MasteringChain);
    auto peqNode = std::make_shared<engine::pipeline::ParametricEqNode>("Master PEQ", engine::pipeline::NodeAffiliation::RenderCommon);
    peqNode->setBypassed(true);
    masterChain->appendNode(std::move(peqNode));

    auto geqNode = std::make_shared<engine::pipeline::GraphicEqNode>("Graphic EQ");
    geqNode->setBypassed(true);
    masterChain->appendNode(std::move(geqNode));

    auto filterNode = std::make_shared<engine::pipeline::HighLowPassNode>("HPF / LPF");
    filterNode->setBypassed(true);
    masterChain->appendNode(std::move(filterNode));

    auto compNode = std::make_shared<engine::pipeline::CompressorNode>("Compressor");
    compNode->setBypassed(true);
    masterChain->appendNode(std::move(compNode));

    masterChain->appendNode(m_insertNode->clone());

    auto reverbNode = std::make_shared<engine::pipeline::ReverbNode>("Reverb");
    reverbNode->setBypassed(true);
    masterChain->appendNode(std::move(reverbNode));

    auto limiterNode = std::make_shared<engine::pipeline::PeakLimiterNode>("Peak Limiter");
    limiterNode->setBypassed(true);
    masterChain->appendNode(std::move(limiterNode));

    // Listening Correction Chain strictly contains Listening Correction ONLY
    auto listeningChain = std::make_unique<engine::pipeline::AudioChain>("Listening Chain", engine::pipeline::ChainPurpose::ListeningCorrectionChain);
    auto listenNode = std::make_shared<engine::pipeline::ParametricEqNode>(
        "Listening Correction", engine::pipeline::NodeAffiliation::ListeningCorrection
    );
    listenNode->setBypassed(false);
    listenNode->configure(48000.0);
    listeningChain->appendNode(std::move(listenNode));

    // 4. Initialize Audio Pipeline with Logical Render Tap
    m_pipeline = std::make_shared<engine::pipeline::AudioPipeline>(48000.0);
    m_pipeline->swapMasterChain(std::move(masterChain));
    m_pipeline->swapListeningChain(std::move(listeningChain));
    m_pipeline->paramMailbox().setListeningPreset("Monitor Flat");

    m_gainSmootherL.configure(48000.0, 15.0);
    m_gainSmootherR.configure(48000.0, 15.0);
    m_gainSmootherL.reset(m_masterGainL.load(std::memory_order_relaxed));
    m_gainSmootherR.reset(m_masterGainR.load(std::memory_order_relaxed));

    // 5. Initialize Native PipeWire AudioOutput
    m_audioOutput = std::make_unique<engine::backend::PipeWireAudioOutput>();
    if (offlineMode) {
        m_audioOutput->setOfflineSimulationMode(true);
    }
    bool initOk = m_audioOutput->initialize(48000, 256);
    if (!initOk) {
        log::LogService::instance().start();
        QJsonObject det;
        det.insert(QStringLiteral("stage"), QStringLiteral("pipewire_context_init"));
        log::LogService::instance().log(
            "AudioEngine",
            "Initialize",
            "Failed",
            "Failed to initialize PipeWire audio output context.",
            "System",
            "PipeWire",
            QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)),
            QString(),
            QString(),
            log::ErrorCode::PipeWireInitFailed,
            0.0
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

            // Apply master gain with smooth interpolation (independent L/R, zipper noise-free, click-free mute)
            const bool isMuted = m_masterMute.load(std::memory_order_relaxed);
            m_gainSmootherL.setTarget(isMuted ? 0.0 : m_masterGainL.load(std::memory_order_relaxed));
            m_gainSmootherR.setTarget(isMuted ? 0.0 : m_masterGainR.load(std::memory_order_relaxed));
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
        QJsonObject det;
        det.insert(QStringLiteral("stage"), QStringLiteral("pipewire_stream_start"));
        log::LogService::instance().log(
            "AudioEngine",
            "StartStream",
            "Failed",
            "Failed to start PipeWire audio output stream.",
            "System",
            "PipeWire",
            QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)),
            QString(),
            QString(),
            log::ErrorCode::PipeWireStartFailed,
            0.0
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

            const auto eofResults = d->processPendingEof();
            for (const auto& res : eofResults) {
                auto rejectErrorCode = [](engine::deck::AutoAdvanceRejectReason r) -> int32_t {
                    using R = engine::deck::AutoAdvanceRejectReason;
                    switch (r) {
                    case R::DuplicateEof: return log::ErrorCode::AutoAdvanceDuplicateEof;
                    case R::StalePlaybackGeneration: return log::ErrorCode::AutoAdvanceStaleGeneration;
                    case R::StaleDecoderRequest: return log::ErrorCode::AutoAdvanceStaleRequest;
                    case R::TransitionInProgress: return log::ErrorCode::AutoAdvanceTransitionInProgress;
                    case R::SessionInactive:
                    case R::DeckLocked: return log::ErrorCode::AutoAdvanceSessionInactive;
                    case R::TransportNotPlaying: return log::ErrorCode::AutoAdvanceNotPlaying;
                    case R::TrackIdentityMismatch: return log::ErrorCode::AutoAdvanceTrackMismatch;
                    case R::OpenTrackFailed: return log::ErrorCode::TrackOpenFailed;
                    default: return 0;
                    }
                };

                const QString eventSourceStr = QString::fromLatin1(engine::deck::toSourceString(res.source));
                const QString rejectReasonStr = QString::fromLatin1(engine::deck::toReasonString(res.rejectReason));

                // Log EOF notification diagnostics (both valid and rejected)
                {
                    QJsonObject eofDet;
                    eofDet.insert(QStringLiteral("deck"), QString(c));
                    eofDet.insert(QStringLiteral("session_id"), log::LogService::instance().sessionId());
                    eofDet.insert(QStringLiteral("folder_session_id"), static_cast<qint64>(res.folderSessionId));
                    eofDet.insert(QStringLiteral("playback_generation"), static_cast<qint64>(res.playbackGenerationBefore));
                    eofDet.insert(QStringLiteral("decoder_request_id"), static_cast<qint64>(res.decoderRequestIdBefore));
                    eofDet.insert(QStringLiteral("track_identity"), QString::fromStdString(res.pathFrom));
                    eofDet.insert(QStringLiteral("event_source"), eventSourceStr);
                    eofDet.insert(QStringLiteral("event_timestamp"), static_cast<qint64>(res.eventTimestampMs));
                    eofDet.insert(QStringLiteral("received_timestamp"), static_cast<qint64>(res.receivedTimestampMs));
                    eofDet.insert(QStringLiteral("valid"), res.validNotification);
                    if (d->eofQueueOverflowCount() > 0) {
                        eofDet.insert(QStringLiteral("eof_queue_overflow_count"), static_cast<qint64>(d->eofQueueOverflowCount()));
                    }
                    if (!res.validNotification) {
                        eofDet.insert(QStringLiteral("invalid_reason"), rejectReasonStr);
                    }
                    log::LogService::instance().log(
                        QStringLiteral("FolderSession"),
                        QStringLiteral("EofNotification"),
                        res.validNotification ? QStringLiteral("Success") : QStringLiteral("Ignored"),
                        res.validNotification
                            ? QString("Valid EOF notification on Deck %1 (gen=%2, req=%3)")
                                  .arg(c)
                                  .arg(res.playbackGenerationBefore)
                                  .arg(res.decoderRequestIdBefore)
                            : QString("Ignored EOF notification on Deck %1 (%2)")
                                  .arg(c, rejectReasonStr),
                        QStringLiteral("Deck"),
                        QString(c),
                        QString::fromUtf8(QJsonDocument(eofDet).toJson(QJsonDocument::Compact)),
                        QString(),
                        QString(),
                        res.validNotification ? 0 : rejectErrorCode(res.rejectReason),
                        0.0
                    );
                }

                if (!res.executed) {
                    continue;
                }

                if (res.advanced) {
                    ++m_advanceSeq;
                    QJsonObject det;
                    det.insert(QStringLiteral("session_id"), log::LogService::instance().sessionId());
                    det.insert(QStringLiteral("folder_session_id"), static_cast<qint64>(res.folderSessionId));
                    det.insert(QStringLiteral("deck"), QString(c));
                    det.insert(QStringLiteral("advance_seq"), static_cast<qint64>(m_advanceSeq));
                    det.insert(QStringLiteral("playback_generation"), static_cast<qint64>(res.playbackGenerationAfter));
                    det.insert(QStringLiteral("playback_generation_before"), static_cast<qint64>(res.playbackGenerationBefore));
                    det.insert(QStringLiteral("decoder_request_id"), static_cast<qint64>(res.decoderRequestIdAfter));
                    det.insert(QStringLiteral("decoder_request_id_before"), static_cast<qint64>(res.decoderRequestIdBefore));
                    det.insert(QStringLiteral("track_index_from"), res.trackIndexFrom);
                    det.insert(QStringLiteral("track_index_to"), res.trackIndexTo);
                    det.insert(QStringLiteral("path_from"), QString::fromStdString(res.pathFrom));
                    det.insert(QStringLiteral("path_to"), QString::fromStdString(res.pathTo));
                    det.insert(QStringLiteral("trigger"), QStringLiteral("auto_eof"));
                    det.insert(QStringLiteral("event_source"), eventSourceStr);
                    det.insert(QStringLiteral("event_timestamp"), static_cast<qint64>(res.eventTimestampMs));
                    det.insert(QStringLiteral("transition_start_timestamp"), static_cast<qint64>(res.transitionStartTimestampMs));
                    det.insert(QStringLiteral("transition_complete_timestamp"), static_cast<qint64>(res.transitionCompleteTimestampMs));
                    det.insert(QStringLiteral("result"), QStringLiteral("success"));
                    det.insert(QStringLiteral("failure_reason"), QString());
                    log::LogService::instance().log(
                        QStringLiteral("FolderSession"),
                        QStringLiteral("AutoAdvance"),
                        QStringLiteral("Success"),
                        QString("Auto-advanced Deck %1 [%2 -> %3] (seq=%4)").arg(c).arg(res.trackIndexFrom).arg(res.trackIndexTo).arg(m_advanceSeq),
                        QStringLiteral("Deck"),
                        QString(c),
                        QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact))
                    );
                } else {
                    const bool completed = (res.rejectReason == engine::deck::AutoAdvanceRejectReason::EndOfFolder);
                    QJsonObject det;
                    det.insert(QStringLiteral("session_id"), log::LogService::instance().sessionId());
                    det.insert(QStringLiteral("folder_session_id"), static_cast<qint64>(res.folderSessionId));
                    det.insert(QStringLiteral("deck"), QString(c));
                    det.insert(QStringLiteral("advance_seq"), static_cast<qint64>(m_advanceSeq));
                    det.insert(QStringLiteral("playback_generation"), static_cast<qint64>(res.playbackGenerationAfter));
                    det.insert(QStringLiteral("decoder_request_id"), static_cast<qint64>(res.decoderRequestIdAfter));
                    det.insert(QStringLiteral("track_index_from"), res.trackIndexFrom);
                    det.insert(QStringLiteral("track_index_to"), res.trackIndexTo);
                    det.insert(QStringLiteral("path_from"), QString::fromStdString(res.pathFrom));
                    det.insert(QStringLiteral("path_to"), QString::fromStdString(res.pathTo));
                    det.insert(QStringLiteral("trigger"), QStringLiteral("auto_eof"));
                    det.insert(QStringLiteral("event_source"), eventSourceStr);
                    det.insert(QStringLiteral("event_timestamp"), static_cast<qint64>(res.eventTimestampMs));
                    det.insert(QStringLiteral("transition_start_timestamp"), static_cast<qint64>(res.transitionStartTimestampMs));
                    det.insert(QStringLiteral("transition_complete_timestamp"), static_cast<qint64>(res.transitionCompleteTimestampMs));
                    det.insert(QStringLiteral("result"), completed ? QStringLiteral("completed") : QStringLiteral("failed"));
                    det.insert(QStringLiteral("failure_reason"), rejectReasonStr);
                    det.insert(QStringLiteral("reason"), rejectReasonStr);
                    det.insert(QStringLiteral("tracks_played"), res.trackIndexFrom + 1);
                    log::LogService::instance().log(
                        QStringLiteral("FolderSession"),
                        QStringLiteral("AutoAdvance"),
                        completed ? QStringLiteral("Info") : QStringLiteral("Failed"),
                        completed
                            ? QString("Folder session completed in Deck %1").arg(c)
                            : QString("Folder session AutoAdvance failed in Deck %1 (%2)").arg(c, rejectReasonStr),
                        QStringLiteral("Deck"),
                        QString(c),
                        QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)),
                        QString(),
                        QString(),
                        completed ? 0 : rejectErrorCode(res.rejectReason),
                        0.0
                    );
                }
                emit deckStateChanged(QString(c));
            }

            if (d->checkAndResetTrackChanged()) {
                emit deckStateChanged(QString(c));
            }
        }
    });
    trackTimer->start(50);

    // Periodic check for dynamic DSP telemetry (Compressor / Limiter GR, VU levels) & audio health
    auto* dspTimer = new QTimer(this);
    connect(dspTimer, &QTimer::timeout, this, [this]() {
        emit dspTelemetryChanged();
        emit diagnosticsChanged();
        if (++m_healthCheckTick >= 30) { // ~1 second interval
            m_healthCheckTick = 0;
            checkAudioHealthAndGraph();
        }
    });
    dspTimer->start(33); // ~30 fps telemetry updates

    // 6. Initialize Logging Service & baseline session metrics
    log::LogService::instance().start();
    m_sessionStartTime = std::chrono::steady_clock::now();
    m_lastXrunLogTime = m_sessionStartTime;
    m_advanceSeq = 0;
    m_pendingXrunDelta = 0;
    if (m_audioOutput) {
        const auto diag = m_audioOutput->diagnostics();
        m_lastReportedXruns = diag.xrunCount;
        m_lastGraphSampleRate = diag.sampleRate;
        m_lastGraphBufferFrames = (diag.negotiatedQuantum > 0) ? diag.negotiatedQuantum : diag.bufferFrames;
        m_lastDirectModeStatus = static_cast<int>(diag.directModeStatus);
    }
    m_lastDroppedLogCount = log::LogService::instance().droppedLogCount();
    m_lastDeckUnderruns = {0, 0, 0, 0};

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

void EngineController::recordSessionStart(const QString& graphicsBackend, const QString& gpuName) {
    if (m_sessionStartRecorded) return;
    m_sessionStartRecorded = true;

    QJsonObject det;
#ifdef DESK32_APP_VERSION
    det.insert(QStringLiteral("app_version"), QStringLiteral(DESK32_APP_VERSION));
#else
    det.insert(QStringLiteral("app_version"), QStringLiteral("0.1.0"));
#endif
#ifdef DESK32_GIT_COMMIT
    det.insert(QStringLiteral("git_commit"), QStringLiteral(DESK32_GIT_COMMIT));
#else
    det.insert(QStringLiteral("git_commit"), QStringLiteral("unknown"));
#endif
#ifdef DESK32_GIT_DIRTY
    det.insert(QStringLiteral("git_dirty"), DESK32_GIT_DIRTY != 0);
#endif
    det.insert(QStringLiteral("qt_version"), QString::fromLatin1(qVersion()));
    det.insert(QStringLiteral("os"), QSysInfo::prettyProductName());
    det.insert(QStringLiteral("platform"), QGuiApplication::platformName());
    det.insert(QStringLiteral("desktop_env"), QString::fromUtf8(qgetenv("XDG_CURRENT_DESKTOP")));
    det.insert(QStringLiteral("graphics_backend"), graphicsBackend);
    det.insert(QStringLiteral("gpu_name"), gpuName);

    if (m_audioOutput) {
        const auto diag = m_audioOutput->diagnostics();
        const uint32_t effectiveQuantum = (diag.negotiatedQuantum > 0) ? diag.negotiatedQuantum : diag.bufferFrames;
        det.insert(QStringLiteral("pipewire_sample_rate_hz"), static_cast<int>(diag.sampleRate));
        det.insert(QStringLiteral("pipewire_quantum"), static_cast<int>(effectiveQuantum));
        det.insert(QStringLiteral("pipewire_requested_quantum"), static_cast<int>(diag.bufferFrames));
        det.insert(QStringLiteral("pipewire_direct_mode"), static_cast<int>(diag.directModeStatus));
        det.insert(QStringLiteral("pipewire_device"), QString::fromStdString(diag.deviceName));
    }
    det.insert(QStringLiteral("initial_output_path"),
               m_outputRoutingMode.load(std::memory_order_relaxed) == 1
                   ? QStringLiteral("RenderMonitor")
                   : QStringLiteral("Listening"));
    det.insert(QStringLiteral("sample_rate_policy"),
               m_switchingMode.load(std::memory_order_relaxed) == 1
                   ? QStringLiteral("Comparative")
                   : QStringLiteral("Normal"));

    log::LogService::instance().log(
        QStringLiteral("Session"),
        QStringLiteral("SessionStart"),
        QStringLiteral("Info"),
        QString("Session started (commit=%1, backend=%2, platform=%3)")
            .arg(det["git_commit"].toString(), graphicsBackend, det["platform"].toString()),
        QStringLiteral("System"),
        QStringLiteral("Desk32"),
        QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact))
    );
}

void EngineController::recordSessionEnd(const QString& reason) {
    if (m_sessionEndRecorded) return;
    m_sessionEndRecorded = true;

    const auto now = std::chrono::steady_clock::now();
    const double uptimeMs = (m_sessionStartTime.time_since_epoch().count() > 0)
        ? std::chrono::duration<double, std::milli>(now - m_sessionStartTime).count()
        : 0.0;
    const uint64_t totalXruns = m_audioOutput ? m_audioOutput->diagnostics().xrunCount : 0;

    QJsonObject det;
    det.insert(QStringLiteral("reason"), reason);
    det.insert(QStringLiteral("uptime_ms"), uptimeMs);
    det.insert(QStringLiteral("auto_advance_count"), static_cast<qint64>(m_advanceSeq));
    det.insert(QStringLiteral("total_xruns"), static_cast<qint64>(totalXruns));

    log::LogService::instance().log(
        QStringLiteral("Session"),
        QStringLiteral("SessionEnd"),
        QStringLiteral("Info"),
        QString("Session ended (%1, uptime=%2 ms, auto_advance=%3, xruns=%4)")
            .arg(reason)
            .arg(uptimeMs, 0, 'f', 0)
            .arg(m_advanceSeq)
            .arg(totalXruns),
        QStringLiteral("System"),
        QStringLiteral("Desk32"),
        QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)),
        QString(),
        QString(),
        0,
        uptimeMs
    );
}

void EngineController::checkAudioHealthAndGraph() {
    if (m_audioOutput) {
        const auto diag = m_audioOutput->diagnostics();
        const uint32_t effectiveQuantum = (diag.negotiatedQuantum > 0) ? diag.negotiatedQuantum : diag.bufferFrames;

        // 1. Detect graph parameter changes (§6)
        const int curDirect = static_cast<int>(diag.directModeStatus);
        if (diag.sampleRate != m_lastGraphSampleRate ||
            effectiveQuantum != m_lastGraphBufferFrames ||
            curDirect != m_lastDirectModeStatus) {
            QJsonObject det;
            det.insert(QStringLiteral("sample_rate_hz_before"), static_cast<int>(m_lastGraphSampleRate));
            det.insert(QStringLiteral("sample_rate_hz_after"), static_cast<int>(diag.sampleRate));
            det.insert(QStringLiteral("quantum_before"), static_cast<int>(m_lastGraphBufferFrames));
            det.insert(QStringLiteral("quantum_after"), static_cast<int>(effectiveQuantum));
            det.insert(QStringLiteral("direct_mode_before"), m_lastDirectModeStatus);
            det.insert(QStringLiteral("direct_mode_after"), curDirect);
            det.insert(QStringLiteral("device"), QString::fromStdString(diag.deviceName));

            log::LogService::instance().log(
                QStringLiteral("AudioEngine"),
                QStringLiteral("GraphChanged"),
                QStringLiteral("Info"),
                QString("Audio graph changed: %1Hz/%2f -> %3Hz/%4f")
                    .arg(m_lastGraphSampleRate).arg(m_lastGraphBufferFrames)
                    .arg(diag.sampleRate).arg(effectiveQuantum),
                QStringLiteral("Device"),
                QString::fromStdString(diag.deviceName),
                QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact))
            );

            m_lastGraphSampleRate = diag.sampleRate;
            m_lastGraphBufferFrames = effectiveQuantum;
            m_lastDirectModeStatus = curDirect;
        }

        // 2. Aggregated PipeWire XRUN reporting (§6)
        if (diag.xrunCount > m_lastReportedXruns) {
            m_pendingXrunDelta += (diag.xrunCount - m_lastReportedXruns);
            m_lastReportedXruns = diag.xrunCount;
        }
        const auto now = std::chrono::steady_clock::now();
        if (m_pendingXrunDelta > 0 &&
            std::chrono::duration<double, std::milli>(now - m_lastXrunLogTime).count() >= 1000.0) {
            const uint64_t delta = m_pendingXrunDelta;
            m_pendingXrunDelta = 0;
            m_lastXrunLogTime = now;

            QJsonObject det;
            det.insert(QStringLiteral("source"), QStringLiteral("pipewire_report"));
            det.insert(QStringLiteral("xrun_delta"), static_cast<qint64>(delta));
            det.insert(QStringLiteral("xrun_total"), static_cast<qint64>(diag.xrunCount));
            det.insert(QStringLiteral("sample_rate_hz"), static_cast<int>(diag.sampleRate));
            det.insert(QStringLiteral("quantum"), static_cast<int>(effectiveQuantum));
            det.insert(QStringLiteral("device"), QString::fromStdString(diag.deviceName));
            if (m_deckManager) {
                det.insert(QStringLiteral("deck"), QString(m_deckManager->activeDeckId()));
            }

            log::LogService::instance().log(
                QStringLiteral("AudioEngine"),
                QStringLiteral("XRUN"),
                QStringLiteral("Warning"),
                QString("Detected %1 PipeWire XRUN(s) (total: %2)").arg(delta).arg(diag.xrunCount),
                QStringLiteral("Device"),
                QString::fromStdString(diag.deviceName),
                QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)),
                QString(),
                QString(),
                log::ErrorCode::AudioXrunDetected,
                0.0
            );
        }
    }

    // 3. DecoderWorker buffer underrun separation (§6)
    if (m_deckManager) {
        const std::array<char, 4> deckIds{'A', 'B', 'C', 'D'};
        for (size_t i = 0; i < deckIds.size(); ++i) {
            const auto* d = m_deckManager->deck(deckIds[i]);
            if (!d) continue;
            const uint64_t curUnderruns = d->worker().underrunCount();
            if (curUnderruns > m_lastDeckUnderruns[i]) {
                const uint64_t delta = curUnderruns - m_lastDeckUnderruns[i];
                m_lastDeckUnderruns[i] = curUnderruns;
                QJsonObject det;
                det.insert(QStringLiteral("source"), QStringLiteral("decoder_underrun"));
                det.insert(QStringLiteral("deck"), QString(deckIds[i]));
                det.insert(QStringLiteral("underrun_delta"), static_cast<qint64>(delta));
                det.insert(QStringLiteral("underrun_total"), static_cast<qint64>(curUnderruns));
                det.insert(QStringLiteral("path"), QString::fromStdString(d->trackInfo().path));
                log::LogService::instance().log(
                    QStringLiteral("AudioEngine"),
                    QStringLiteral("Underrun"),
                    QStringLiteral("Warning"),
                    QString("Deck %1 decoder ring-buffer underrun (+%2, total: %3)")
                        .arg(deckIds[i]).arg(delta).arg(curUnderruns),
                    QStringLiteral("Deck"),
                    QString(deckIds[i]),
                    QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)),
                    QString(),
                    QString(),
                    log::ErrorCode::DecoderUnderrunDetected,
                    0.0
                );
            }
        }
    }

    // 4. Self-monitoring LogService queue overflow (§0 Principle 5)
    const size_t curDropped = log::LogService::instance().droppedLogCount();
    if (curDropped > m_lastDroppedLogCount) {
        const size_t delta = curDropped - m_lastDroppedLogCount;
        m_lastDroppedLogCount = curDropped;
        QJsonObject det;
        det.insert(QStringLiteral("dropped_delta"), static_cast<qint64>(delta));
        det.insert(QStringLiteral("dropped_total"), static_cast<qint64>(curDropped));
        log::LogService::instance().log(
            QStringLiteral("System"),
            QStringLiteral("LogDropped"),
            QStringLiteral("Warning"),
            QString("LogService bounded queue dropped %1 event(s) (total: %2)").arg(delta).arg(curDropped),
            QStringLiteral("System"),
            QStringLiteral("LogService"),
            QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)),
            QString(),
            QString(),
            log::ErrorCode::LogQueueOverflow,
            0.0
        );
    }
}

void EngineController::copyToClipboard(const QString& text) {
    if (auto* cb = QGuiApplication::clipboard()) {
        cb->setText(text);
    }
}

void EngineController::commitDspParameterChange(
    const QString& dspNode,
    const QVariantMap& beforeMap,
    const QVariantMap& afterMap,
    const QString& trigger)
{
    QJsonObject det;
    det.insert(QStringLiteral("dsp_node"), dspNode);
    det.insert(QStringLiteral("trigger"), trigger);
    det.insert(QStringLiteral("before"), QJsonObject::fromVariantMap(beforeMap));
    det.insert(QStringLiteral("after"), QJsonObject::fromVariantMap(afterMap));

    log::LogService::instance().log(
        QStringLiteral("DSP"),
        QStringLiteral("ParameterChange"),
        QStringLiteral("Success"),
        QString("Updated %1 parameters (trigger: %2)").arg(dspNode, trigger),
        QStringLiteral("DSPNode"),
        dspNode,
        QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact))
    );
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

bool EngineController::isMasterMute() const {
    return m_masterMute.load(std::memory_order_relaxed);
}

double EngineController::masterPeakL() const {
    return m_pipeline ? m_pipeline->masterPeakL() : 0.0;
}

double EngineController::masterPeakR() const {
    return m_pipeline ? m_pipeline->masterPeakR() : 0.0;
}

double EngineController::masterRmsL() const {
    return m_pipeline ? m_pipeline->masterRmsL() : 0.0;
}

double EngineController::masterRmsR() const {
    return m_pipeline ? m_pipeline->masterRmsR() : 0.0;
}

bool EngineController::masterClipped() const {
    return m_pipeline ? m_pipeline->masterClipped() : false;
}

double EngineController::masterHeadroomDb() const {
    double peak = std::max(masterPeakL(), masterPeakR());
    if (peak <= 0.000001) return 96.0;
    return -20.0 * std::log10(peak);
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
    return !m_listeningCorrectionBypassed.load(std::memory_order_relaxed);
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

bool EngineController::isRenderIdle() const {
    return m_renderEngine.isIdle();
}

bool EngineController::isRendering() const {
    return m_renderEngine.isRendering();
}

int EngineController::renderWorkerState() const {
    return static_cast<int>(m_renderEngine.workerState());
}

QString EngineController::renderWorkerStateText() const {
    switch (m_renderEngine.workerState()) {
        case engine::render::RenderWorkerState::Idle:      return QStringLiteral("Idle");
        case engine::render::RenderWorkerState::Starting:  return QStringLiteral("Starting");
        case engine::render::RenderWorkerState::Rendering: return QStringLiteral("Rendering");
        case engine::render::RenderWorkerState::Stopping:  return QStringLiteral("Stopping");
    }
    return QStringLiteral("Idle");
}

int EngineController::renderQueuedCount() const {
    return m_renderEngine.queuedTasksCount();
}

int EngineController::renderStatus() const {
    auto info = m_renderEngine.currentTaskInfo();
    if (info.taskId.isEmpty() && m_renderEngine.isIdle()) {
        return -1; // Idle / No active or completed task
    }
    return static_cast<int>(info.status);
}

QString EngineController::renderStatusText() const {
    auto info = m_renderEngine.currentTaskInfo();
    if (info.taskId.isEmpty() && m_renderEngine.isIdle()) {
        return QStringLiteral("Idle");
    }
    switch (info.status) {
        case engine::render::RenderStatus::Queued:     return QStringLiteral("Queued");
        case engine::render::RenderStatus::Processing: return QStringLiteral("Processing");
        case engine::render::RenderStatus::Completed:  return QStringLiteral("Completed");
        case engine::render::RenderStatus::Failed:     return QStringLiteral("Failed");
        case engine::render::RenderStatus::Cancelled:  return QStringLiteral("Cancelled");
    }
    return QStringLiteral("Idle");
}

QString EngineController::renderTaskId() const {
    return m_renderEngine.currentTaskInfo().taskId;
}

QString EngineController::renderType() const {
    auto info = m_renderEngine.currentTaskInfo();
    if (info.taskId.isEmpty()) return QStringLiteral("None");
    return (info.type == engine::render::RenderType::Album) ? QStringLiteral("Album") : QStringLiteral("Track");
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

QString EngineController::renderSerial() const {
    return m_renderEngine.currentTaskInfo().serial;
}

QString EngineController::renderErrorMessage() const {
    return m_renderEngine.currentTaskInfo().errorMessage;
}

QVariantList EngineController::renderTrackResults() const {
    return m_renderEngine.currentTaskInfo().trackResults;
}

QStringList EngineController::renderOutputFiles() const {
    return m_renderEngine.currentTaskInfo().renderedFiles;
}

bool EngineController::renderRollbackPerformed() const {
    return m_renderEngine.currentTaskInfo().rollbackPerformed;
}

bool EngineController::renderRollbackSucceeded() const {
    return m_renderEngine.currentTaskInfo().rollbackSucceeded;
}

double EngineController::renderMonitorVolume() const {
    return m_pipeline ? m_pipeline->renderMonitorVolume() : 1.0;
}

double EngineController::listeningVolume() const {
    return m_pipeline ? m_pipeline->listeningVolume() : 1.0;
}

void EngineController::setSwitchingMode(int mode) {
    if (m_switchingMode.exchange(mode) != mode) {
        emit switchingModeChanged(mode);
    }
}

void EngineController::setOutputRoutingMode(int path) {
    const int prev = m_outputRoutingMode.exchange(path);
    if (prev != path) {
        emit outputRoutingModeChanged(path);
        QJsonObject det;
        det.insert(QStringLiteral("from"), prev == 1 ? QStringLiteral("RenderMonitor") : QStringLiteral("Listening"));
        det.insert(QStringLiteral("to"), path == 1 ? QStringLiteral("RenderMonitor") : QStringLiteral("Listening"));
        det.insert(QStringLiteral("trigger"), QStringLiteral("user"));
        log::LogService::instance().log(
            "Routing",
            "SetOutputPath",
            "Success",
            path == 0 ? "Switched to Listening Path" : "Switched to Render Monitor Path",
            "OutputPath",
            path == 0 ? "Listening" : "RenderMonitor",
            QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact))
        );
    }
}

void EngineController::setGlobalDspBypass(bool bypass) {
    const bool prevBypass = m_globalDspBypass.load(std::memory_order_relaxed);
    if (prevBypass == bypass && !m_globalBypassDesynced.load(std::memory_order_relaxed)) {
        return;
    }

    if (!m_pipeline) {
        m_globalDspBypass.store(bypass, std::memory_order_relaxed);
        m_globalBypassDesynced.store(false, std::memory_order_relaxed);
        emit globalDspBypassChanged(bypass);
        return;
    }

    engine::dsp::DspCommand cmdMaster;
    cmdMaster.type = engine::dsp::DspCommandType::SetGlobalBypass;
    cmdMaster.subIndex = 0; // Master
    cmdMaster.boolFlag1 = bypass ? 1 : 0;

    engine::dsp::DspCommand cmdListen;
    cmdListen.type = engine::dsp::DspCommandType::SetGlobalBypass;
    cmdListen.subIndex = 1; // Listening
    cmdListen.boolFlag1 = bypass ? 1 : 0;

    // P1-1: Atomically enqueue both Master and Listening commands under a single producer lock.
    // pushTwo() guarantees that either BOTH commands are enqueued or NEITHER is enqueued.
    // Partial bypass state between Master and Listening chains is structurally impossible.
    if (!m_pipeline->paramQueue().pushTwo(cmdMaster, cmdListen)) {
        QJsonObject det;
        det.insert(QStringLiteral("scope"), QStringLiteral("global"));
        det.insert(QStringLiteral("target_chains"), QJsonArray{QStringLiteral("Master"), QStringLiteral("Listening")});
        det.insert(QStringLiteral("bypassed"), bypass);
        det.insert(QStringLiteral("stage"), QStringLiteral("dsp_queue_push"));
        log::LogService::instance().log(
            "DSP",
            "GlobalBypass",
            "Failed",
            QString("Failed to queue Global DSP Bypass: queue full (code: 3001)"),
            "DSPQueue",
            "Pipeline",
            QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)),
            QString(),
            QString(),
            log::ErrorCode::DspQueueFull,
            0.0
        );
        emit dspQueueOverflowWarning(static_cast<int>(cmdMaster.type));
        emit globalDspBypassChanged(prevBypass);
        return;
    }

    m_globalDspBypass.store(bypass, std::memory_order_relaxed);
    m_globalBypassDesynced.store(false, std::memory_order_relaxed);

    // Section 10.2.3 & 5.9: Global DSP Bypass is temporary and does NOT destroy individual DSP node ON/OFF states!
    emit globalDspBypassChanged(bypass);
    emit peqParametersChanged();
    emit geqParametersChanged();
    emit filterParametersChanged();
    emit compressorParametersChanged();
    emit limiterParametersChanged();
    emit reverbParametersChanged();
    QJsonObject bypassDet;
    bypassDet.insert(QStringLiteral("scope"), QStringLiteral("global"));
    bypassDet.insert(QStringLiteral("target_chains"), QJsonArray{QStringLiteral("Master"), QStringLiteral("Listening")});
    bypassDet.insert(QStringLiteral("bypassed"), bypass);
    bypassDet.insert(QStringLiteral("trigger"), QStringLiteral("user"));
    log::LogService::instance().log(
        "DSP",
        "GlobalBypass",
        "Success",
        bypass ? "Global DSP Bypass Engaged" : "Global DSP Bypass Disengaged",
        "DSPChain",
        "Master",
        QString::fromUtf8(QJsonDocument(bypassDet).toJson(QJsonDocument::Compact))
    );
}

bool EngineController::resyncGlobalDspBypass() {
    if (!m_pipeline) return false;
    const bool target = m_globalDspBypass.load(std::memory_order_relaxed);

    engine::dsp::DspCommand cmdMaster;
    cmdMaster.type = engine::dsp::DspCommandType::SetGlobalBypass;
    cmdMaster.subIndex = 0;
    cmdMaster.boolFlag1 = target ? 1 : 0;

    engine::dsp::DspCommand cmdListen;
    cmdListen.type = engine::dsp::DspCommandType::SetGlobalBypass;
    cmdListen.subIndex = 1;
    cmdListen.boolFlag1 = target ? 1 : 0;

    if (!m_pipeline->paramQueue().hasSpace(2)) {
        log::LogService::instance().log(
            "DSP",
            "GlobalBypassResync",
            "Failed",
            "Failed to resynchronize Global DSP Bypass: parameter queue full (code: 3005)",
            "DSPQueue",
            "Pipeline"
        );
        return false;
    }

    if (m_pipeline->paramQueue().pushTwo(cmdMaster, cmdListen)) {
        m_globalBypassDesynced.store(false, std::memory_order_relaxed);
        log::LogService::instance().log(
            "DSP",
            "GlobalBypassResync",
            "Success",
            "Global DSP Bypass resynchronized successfully",
            "DSPChain",
            "Master"
        );
        emit globalDspBypassChanged(target);
        return true;
    }

    return false;
}

void EngineController::setMasterMute(bool mute) {
    bool prev = m_masterMute.exchange(mute);
    if (prev != mute) {
        emit masterMuteChanged(mute);
        log::LogService::instance().log(
            "MasterControl",
            "Mute",
            "Success",
            mute ? "Master Output Muted" : "Master Output Unmuted",
            "Master",
            mute ? "MuteOn" : "MuteOff"
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
        const auto info = d->trackInfo();
        const double sr = info.sampleRate;
        const double posMs = (sr > 0.0) ? (static_cast<double>(d->playhead()) * 1000.0 / sr) : 0.0;
        const double durMs = (sr > 0.0) ? (static_cast<double>(info.totalFrames) * 1000.0 / sr) : 0.0;
        QJsonObject det;
        det.insert(QStringLiteral("deck"), deckId);
        det.insert(QStringLiteral("trigger"), QStringLiteral("user"));
        det.insert(QStringLiteral("path"), QString::fromStdString(info.path));
        det.insert(QStringLiteral("position_ms"), posMs);
        det.insert(QStringLiteral("duration_ms_track"), durMs);
        log::LogService::instance().log(
            "Transport", "Play", "Success",
            "Deck " + deckId + " started playback",
            "Deck", deckId,
            QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact))
        );
    }
}

void EngineController::pause(const QString& deckId) {
    if (!m_deckManager || deckId.isEmpty()) return;
    char id = deckId.toUpper().at(0).toLatin1();
    auto* d = m_deckManager->deck(id);
    if (d) {
        d->pause();
        emit deckStateChanged(deckId);
        const auto info = d->trackInfo();
        const double sr = info.sampleRate;
        const double posMs = (sr > 0.0) ? (static_cast<double>(d->playhead()) * 1000.0 / sr) : 0.0;
        const double durMs = (sr > 0.0) ? (static_cast<double>(info.totalFrames) * 1000.0 / sr) : 0.0;
        QJsonObject det;
        det.insert(QStringLiteral("deck"), deckId);
        det.insert(QStringLiteral("trigger"), QStringLiteral("user"));
        det.insert(QStringLiteral("path"), QString::fromStdString(info.path));
        det.insert(QStringLiteral("position_ms"), posMs);
        det.insert(QStringLiteral("duration_ms_track"), durMs);
        log::LogService::instance().log(
            "Transport", "Pause", "Success",
            "Deck " + deckId + " paused",
            "Deck", deckId,
            QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact))
        );
    }
}

void EngineController::stop(const QString& deckId) {
    if (!m_deckManager || deckId.isEmpty()) return;
    char id = deckId.toUpper().at(0).toLatin1();
    auto* d = m_deckManager->deck(id);
    if (d) {
        const auto info = d->trackInfo();
        const double sr = info.sampleRate;
        const double posMs = (sr > 0.0) ? (static_cast<double>(d->playhead()) * 1000.0 / sr) : 0.0;
        const double durMs = (sr > 0.0) ? (static_cast<double>(info.totalFrames) * 1000.0 / sr) : 0.0;
        d->stop();
        emit deckStateChanged(deckId);
        QJsonObject det;
        det.insert(QStringLiteral("deck"), deckId);
        det.insert(QStringLiteral("trigger"), QStringLiteral("user"));
        det.insert(QStringLiteral("path"), QString::fromStdString(info.path));
        det.insert(QStringLiteral("position_ms"), posMs);
        det.insert(QStringLiteral("duration_ms_track"), durMs);
        log::LogService::instance().log(
            "Transport", "Stop", "Success",
            "Deck " + deckId + " stopped",
            "Deck", deckId,
            QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact))
        );
    }
}

void EngineController::recordSeekEvent(
    char deckChar,
    int64_t beforeFrame,
    const engine::decoder::SeekResult& res,
    const QString& trigger,
    double durationMs,
    const std::string& trackPath)
{
    const QString deckStr(deckChar);
    const double sr = res.sampleRateHz;
    auto framesToMs = [sr](int64_t frames) -> double {
        return (sr > 0.0) ? (static_cast<double>(frames) * 1000.0 / sr) : 0.0;
    };

    const double posBeforeMs = framesToMs(beforeFrame);
    const double requestedMs = framesToMs(res.requestedFrame);
    const double actualMs = framesToMs(res.actualFrame);

    QJsonObject details;
    details.insert(QStringLiteral("deck"), deckStr);
    details.insert(QStringLiteral("trigger"), trigger);
    details.insert(QStringLiteral("path"), QString::fromStdString(trackPath));
    details.insert(QStringLiteral("sample_rate_hz"), sr);
    details.insert(QStringLiteral("position_ms_before"), posBeforeMs);
    details.insert(QStringLiteral("requested_position_ms"), requestedMs);
    details.insert(QStringLiteral("position_ms_after"), actualMs);
    details.insert(QStringLiteral("actual_position_ms"), actualMs);
    details.insert(QStringLiteral("requested_frame"), static_cast<qint64>(res.requestedFrame));
    details.insert(QStringLiteral("actual_frame"), static_cast<qint64>(res.actualFrame));
    details.insert(QStringLiteral("within_buffer"), res.withinBuffer);

    if (!res.success) {
        details.insert(QStringLiteral("stage"), QString::fromStdString(res.stage));
        if (!res.errorMessage.empty()) {
            details.insert(QStringLiteral("error_detail"), QString::fromStdString(res.errorMessage));
        }
    }

    const QString detailsJson = QString::fromUtf8(QJsonDocument(details).toJson(QJsonDocument::Compact));
    const QString status = res.success ? QStringLiteral("Success") : QStringLiteral("Failed");
    const QString msg = res.success
        ? QString("Deck %1 seek to %2 ms (trigger: %3, within_buffer: %4)")
              .arg(deckStr)
              .arg(actualMs, 0, 'f', 1)
              .arg(trigger)
              .arg(res.withinBuffer ? "true" : "false")
        : QString("Deck %1 seek failed at stage '%2' (trigger: %3)")
              .arg(deckStr)
              .arg(QString::fromStdString(res.stage))
              .arg(trigger);

    log::LogService::instance().log(
        QStringLiteral("Transport"),
        QStringLiteral("Seek"),
        status,
        msg,
        QStringLiteral("Deck"),
        deckStr,
        detailsJson,
        QString(),
        QString(),
        res.success ? 0 : res.errorCode,
        durationMs
    );
}

void EngineController::seek(const QString& deckId, qint64 targetFrame) {
    seekWithTrigger(deckId, targetFrame, QStringLiteral("user"));
}

void EngineController::seekWithTrigger(const QString& deckId, qint64 targetFrame, const QString& trigger) {
    if (!m_deckManager || deckId.isEmpty()) return;
    char id = deckId.toUpper().at(0).toLatin1();
    auto* d = m_deckManager->deck(id);
    if (d) {
        const int64_t beforeFrame = d->playhead();
        const std::string trackPath = d->trackInfo().path;
        const auto start = std::chrono::steady_clock::now();
        const auto res = d->seekWithResult(targetFrame);
        const double durationMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

        recordSeekEvent(id, beforeFrame, res, trigger, durationMs, trackPath);
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
        if (res.hadSeek) {
            const auto* tgtDeck = m_deckManager->deck(res.targetDeck);
            const std::string tgtPath = tgtDeck ? tgtDeck->trackInfo().path : std::string();
            recordSeekEvent(res.targetDeck, res.seekBeforeFrame, res.seekResult,
                            QStringLiteral("compare_switch"), res.seekDurationMs, tgtPath);
        }
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
        if (res.hadSeek) {
            const auto* tgtDeck = m_deckManager->deck(res.targetDeck);
            const std::string tgtPath = tgtDeck ? tgtDeck->trackInfo().path : std::string();
            recordSeekEvent(res.targetDeck, res.seekBeforeFrame, res.seekResult,
                            QStringLiteral("compare_switch"), res.seekDurationMs, tgtPath);
        }
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

    const auto start = std::chrono::steady_clock::now();
    bool ok = d->loadTrack(filePath.toStdString(), m_deckManager->graphRate());
    const double loadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

    if (ok) {
        emit deckStateChanged(deckId);
        const auto info = d->trackInfo();
        const double sr = info.sampleRate;
        const double durMs = (sr > 0.0) ? (static_cast<double>(info.totalFrames) * 1000.0 / sr) : 0.0;
        QJsonObject det;
        det.insert(QStringLiteral("deck"), deckId);
        det.insert(QStringLiteral("trigger"), QStringLiteral("user"));
        det.insert(QStringLiteral("path"), filePath);
        det.insert(QStringLiteral("codec"), QFileInfo(filePath).suffix().toLower());
        det.insert(QStringLiteral("sample_rate_hz"), sr);
        det.insert(QStringLiteral("channels"), static_cast<int>(info.channels));
        det.insert(QStringLiteral("duration_ms_track"), durMs);
        log::LogService::instance().log(
            "Deck",
            "LoadTrack",
            "Success",
            QString("Loaded %1 into Deck %2").arg(QFileInfo(filePath).fileName(), deckId),
            "File",
            filePath,
            QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)),
            QString(),
            QString(),
            0,
            loadMs
        );
    } else {
        const bool locked = d->isLocked();
        const int32_t errCode = locked ? log::ErrorCode::TrackLockedOrCorrupt : log::ErrorCode::TrackOpenFailed;
        QJsonObject det;
        det.insert(QStringLiteral("deck"), deckId);
        det.insert(QStringLiteral("trigger"), QStringLiteral("user"));
        det.insert(QStringLiteral("path"), filePath);
        det.insert(QStringLiteral("stage"), locked ? QStringLiteral("deck_lock_check") : QStringLiteral("open"));
        det.insert(QStringLiteral("deck_locked"), locked);
        det.insert(QStringLiteral("file_exists"), QFileInfo::exists(filePath));
        det.insert(QStringLiteral("retried"), false);
        log::LogService::instance().log(
            "Deck",
            "LoadTrack",
            "Failed",
            QString("Failed to load %1 into Deck %2 (%3)")
                .arg(QFileInfo(filePath).fileName(), deckId, locked ? QStringLiteral("deck locked") : QStringLiteral("open failed")),
            "File",
            filePath,
            QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)),
            QString(),
            QString(),
            errCode,
            loadMs
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
        const auto& fs = d->folderSession();
        const QString firstPath = fs.currentTrack() ? QString::fromStdString(fs.currentTrack()->path) : QString();
        QJsonObject det;
        det.insert(QStringLiteral("deck"), deckId);
        det.insert(QStringLiteral("trigger"), QStringLiteral("user"));
        det.insert(QStringLiteral("folder_path"), folderPath);
        det.insert(QStringLiteral("track_count"), static_cast<int>(fs.tracks().size()));
        det.insert(QStringLiteral("first_track_path"), firstPath);
        log::LogService::instance().log(
            "FolderSession",
            "StartSession",
            "Success",
            QString("Started folder session in Deck %1 with %2 tracks").arg(deckId).arg(fs.tracks().size()),
            "Folder",
            folderPath,
            QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact))
        );
    } else {
        QJsonObject det;
        det.insert(QStringLiteral("deck"), deckId);
        det.insert(QStringLiteral("trigger"), QStringLiteral("user"));
        det.insert(QStringLiteral("folder_path"), folderPath);
        det.insert(QStringLiteral("stage"), QStringLiteral("open"));
        det.insert(QStringLiteral("candidate_count"), static_cast<int>(files.size()));
        log::LogService::instance().log(
            "FolderSession",
            "StartSession",
            "Failed",
            QString("Failed to start folder session in Deck %1 for %2").arg(deckId, folderPath),
            "Folder",
            folderPath,
            QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)),
            QString(),
            QString(),
            log::ErrorCode::TrackOpenFailed,
            0.0
        );
    }
    return ok;
}

bool EngineController::advanceFolderNext(const QString& deckId) {
    if (!m_deckManager || deckId.isEmpty()) return false;
    char id = deckId.toUpper().at(0).toLatin1();
    auto* d = m_deckManager->deck(id);
    if (!d) return false;

    const auto& fsBefore = d->folderSession();
    const int idxFrom = static_cast<int>(fsBefore.currentIndex());
    const std::string pathFrom = fsBefore.currentTrack() ? fsBefore.currentTrack()->path : std::string();

    bool ok = d->advanceFolderNext();
    const auto& fsAfter = d->folderSession();
    const int idxTo = static_cast<int>(fsAfter.currentIndex());
    const std::string pathTo = fsAfter.currentTrack() ? fsAfter.currentTrack()->path : std::string();

    QJsonObject det;
    det.insert(QStringLiteral("deck"), deckId);
    det.insert(QStringLiteral("trigger"), QStringLiteral("user"));
    det.insert(QStringLiteral("track_index_from"), idxFrom);
    det.insert(QStringLiteral("track_index_to"), idxTo);
    det.insert(QStringLiteral("path_from"), QString::fromStdString(pathFrom));
    det.insert(QStringLiteral("path_to"), QString::fromStdString(pathTo));
    log::LogService::instance().log(
        "FolderSession",
        "AdvanceNext",
        ok ? "Success" : "Info",
        ok ? QString("Manual folder next in Deck %1 [%2 -> %3]").arg(deckId).arg(idxFrom).arg(idxTo)
           : QString("Folder next reached end in Deck %1").arg(deckId),
        "Deck",
        deckId,
        QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact))
    );

    emit deckStateChanged(deckId);
    return ok;
}

bool EngineController::retreatFolderPrevious(const QString& deckId) {
    if (!m_deckManager || deckId.isEmpty()) return false;
    char id = deckId.toUpper().at(0).toLatin1();
    auto* d = m_deckManager->deck(id);
    if (!d) return false;

    const auto& fsBefore = d->folderSession();
    const int idxFrom = static_cast<int>(fsBefore.currentIndex());
    const std::string pathFrom = fsBefore.currentTrack() ? fsBefore.currentTrack()->path : std::string();

    bool ok = d->retreatFolderPrevious();
    const auto& fsAfter = d->folderSession();
    const int idxTo = static_cast<int>(fsAfter.currentIndex());
    const std::string pathTo = fsAfter.currentTrack() ? fsAfter.currentTrack()->path : std::string();

    QJsonObject det;
    det.insert(QStringLiteral("deck"), deckId);
    det.insert(QStringLiteral("trigger"), QStringLiteral("user"));
    det.insert(QStringLiteral("track_index_from"), idxFrom);
    det.insert(QStringLiteral("track_index_to"), idxTo);
    det.insert(QStringLiteral("path_from"), QString::fromStdString(pathFrom));
    det.insert(QStringLiteral("path_to"), QString::fromStdString(pathTo));
    log::LogService::instance().log(
        "FolderSession",
        "RetreatPrevious",
        ok ? "Success" : "Info",
        ok ? QString("Manual folder previous in Deck %1 [%2 -> %3]").arg(deckId).arg(idxFrom).arg(idxTo)
           : QString("Folder previous at start in Deck %1").arg(deckId),
        "Deck",
        deckId,
        QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact))
    );

    emit deckStateChanged(deckId);
    return ok;
}

void EngineController::setGeqBandGain(int bandIndex, double gainDb) {
    if (m_pipeline) {
        // P0-21: Continuous slider parameter update exclusively via lock-free DspParameterMailbox
        m_pipeline->paramMailbox().setGeqBandGain(static_cast<size_t>(bandIndex), static_cast<float>(gainDb));
    }
    // Update Control Thread local UI Snapshot
    m_uiGeq.setBandGain(static_cast<size_t>(bandIndex), gainDb);
    emit geqParametersChanged();
}

double EngineController::getGeqBandGain(int bandIndex) {
    return (bandIndex >= 0 && bandIndex < 32) ? m_uiGeq.bandGain(static_cast<size_t>(bandIndex)) : 0.0;
}

void EngineController::resetAllGeqBands() {
    if (m_pipeline) {
        // P0-21: Atomic batch reset exclusively via DspParameterMailbox
        m_pipeline->paramMailbox().resetAllGeqBands();
    }
    m_uiGeq.resetAllBands();
    emit geqParametersChanged();
    log::LogService::instance().log("DSP", "ResetGEQ", "Success", "Reset all 32 GEQ bands to 0 dB", "DSPNode", "GEQ");
}

QVariantList EngineController::getGeqCurve(int numPoints) {
    QVariantList list;
    if (numPoints < 2) return list;

    std::vector<double> freqs;
    freqs.reserve(static_cast<size_t>(numPoints) + GraphicEQ::NUM_BANDS);

    for (int i = 0; i < numPoints; ++i) {
        // Logarithmically spaced frequencies between 16.0 Hz and 20000.0 Hz
        double f = 16.0 * std::pow(20000.0 / 16.0, static_cast<double>(i) / (numPoints - 1));
        freqs.push_back(f);
    }
    // Explicitly add exact center frequencies of all bands so each peak is perfectly represented
    for (size_t b = 0; b < GraphicEQ::NUM_BANDS; ++b) {
        freqs.push_back(m_uiGeq.bandFrequency(b));
    }
    std::sort(freqs.begin(), freqs.end());
    freqs.erase(std::unique(freqs.begin(), freqs.end(), [](double a, double b) {
        return std::abs(a - b) < 1e-4;
    }), freqs.end());

    std::vector<double> outDb;
    m_uiGeq.calculateResponseCurve(freqs, outDb);

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
    if (bandIndex < 0 || bandIndex >= static_cast<int>(ParametricEQ::BANDS_PER_CHANNEL)) return map;

    const auto& band = isRightChannel ? m_uiPeq.rightBand(static_cast<size_t>(bandIndex))
                                      : m_uiPeq.leftBand(static_cast<size_t>(bandIndex));
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
    if (bandIndex < 0 || bandIndex >= static_cast<int>(ParametricEQ::BANDS_PER_CHANNEL)) return;

    if (m_pipeline) {
        // P0-21: Continuous parameter update exclusively via lock-free DspParameterMailbox
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
        m_uiPeq.setRightBand(static_cast<size_t>(bandIndex), cfg);
    } else {
        m_uiPeq.setLeftBand(static_cast<size_t>(bandIndex), cfg);
    }
    emit peqParametersChanged();
}

void EngineController::setPeqBandEnabled(bool isRightChannel, int bandIndex, bool enabled) {
    if (bandIndex < 0 || bandIndex >= static_cast<int>(ParametricEQ::BANDS_PER_CHANNEL)) return;

    if (m_pipeline) {
        // P0-21: Update band enable status exclusively via DspParameterMailbox
        m_pipeline->paramMailbox().setPeqBandEnabled(static_cast<size_t>(bandIndex), isRightChannel ? 1 : 0, enabled);
    }

    auto cfg = isRightChannel ? m_uiPeq.rightBand(static_cast<size_t>(bandIndex))
                              : m_uiPeq.leftBand(static_cast<size_t>(bandIndex));
    cfg.enabled = enabled;
    if (isRightChannel) {
        m_uiPeq.setRightBand(static_cast<size_t>(bandIndex), cfg);
    } else {
        m_uiPeq.setLeftBand(static_cast<size_t>(bandIndex), cfg);
    }
    emit peqParametersChanged();
}

QVariantList EngineController::getPeqCurve(bool isRightChannel, int numPoints) {
    QVariantList list;
    if (numPoints < 2) return list;

    std::vector<double> freqs(static_cast<size_t>(numPoints));
    for (int i = 0; i < numPoints; ++i) {
        freqs[static_cast<size_t>(i)] = 20.0 * std::pow(20000.0 / 20.0, static_cast<double>(i) / (numPoints - 1));
    }

    std::vector<double> leftDb, rightDb;
    m_uiPeq.calculateCompositeResponse(freqs, leftDb, rightDb);

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
    bool ok = m_uiPeq.applyPreset(presetName.toStdString());
    if (ok) {
        if (m_pipeline) {
            // P0-21: Publish entire 24-band PEQ configuration atomically via Mailbox
            std::array<engine::dsp::ContinuousDspParameters::PeqBandConfig, 24> leftBands{};
            std::array<engine::dsp::ContinuousDspParameters::PeqBandConfig, 24> rightBands{};
            for (size_t b = 0; b < 24; ++b) {
                const auto& l = m_uiPeq.leftBand(b);
                leftBands[b].frequency = static_cast<float>(l.frequency);
                leftBands[b].gainDb = static_cast<float>(l.gainDb);
                leftBands[b].Q = static_cast<float>(l.Q);
                leftBands[b].type = static_cast<uint8_t>(l.type);
                leftBands[b].enabled = l.enabled ? 1 : 0;
                leftBands[b].modified = 1;

                const auto& r = m_uiPeq.rightBand(b);
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

void EngineController::setFilterParams(double hpfFreq, bool hpfOn, int hpfSlope, double lpfFreq, bool lpfOn, int lpfSlope) {
    if (m_pipeline) {
        // P0-21: Continuous parameter update exclusively via lock-free DspParameterMailbox
        m_pipeline->paramMailbox().setFilterParams(static_cast<float>(hpfFreq), static_cast<float>(lpfFreq),
                                                   static_cast<uint8_t>(hpfSlope), static_cast<uint8_t>(lpfSlope),
                                                   hpfOn, lpfOn);
    }
    m_uiFilter.setHpfFrequency(hpfFreq);
    m_uiFilter.setHpfEnabled(hpfOn);
    m_uiFilter.setHpfSlope(hpfSlope == 1 ? HighLowPassFilter::FilterSlope::Slope24dB : HighLowPassFilter::FilterSlope::Slope12dB);

    m_uiFilter.setLpfFrequency(lpfFreq);
    m_uiFilter.setLpfEnabled(lpfOn);
    m_uiFilter.setLpfSlope(lpfSlope == 1 ? HighLowPassFilter::FilterSlope::Slope24dB : HighLowPassFilter::FilterSlope::Slope12dB);

    emit filterParametersChanged();
}

QVariantMap EngineController::getFilterParams() {
    QVariantMap map;
    map["hpfFrequency"] = m_uiFilter.hpfFrequency();
    map["hpfEnabled"] = m_uiFilter.isHpfEnabled();
    map["hpfSlope"] = m_uiFilter.hpfSlope() == HighLowPassFilter::FilterSlope::Slope24dB ? 1 : 0;
    map["lpfFrequency"] = m_uiFilter.lpfFrequency();
    map["lpfEnabled"] = m_uiFilter.isLpfEnabled();
    map["lpfSlope"] = m_uiFilter.lpfSlope() == HighLowPassFilter::FilterSlope::Slope24dB ? 1 : 0;
    map["bypassed"] = m_filterBypassed.load(std::memory_order_relaxed);
    return map;
}

QVariantList EngineController::getFilterCurve(int numPoints) {
    QVariantList list;
    if (numPoints < 2) return list;

    std::vector<double> freqs(static_cast<size_t>(numPoints));
    for (int i = 0; i < numPoints; ++i) {
        freqs[static_cast<size_t>(i)] = 20.0 * std::pow(20000.0 / 20.0, static_cast<double>(i) / (numPoints - 1));
    }

    std::vector<double> outDb;
    m_uiFilter.calculateResponseCurve(freqs, outDb);

    for (int i = 0; i < numPoints; ++i) {
        QVariantMap pt;
        pt["f"] = freqs[static_cast<size_t>(i)];
        pt["db"] = outDb[static_cast<size_t>(i)];
        list.append(pt);
    }
    return list;
}

void EngineController::setCompressorParams(double thresholdDb, double ratio, double attackMs, double releaseMs, double makeupDb, double kneeDb) {
    bool enabled = !m_compBypassed.load(std::memory_order_relaxed);
    double sRate = m_pipeline ? m_pipeline->graphSampleRate() : 48000.0;
    m_uiComp.configure(sRate, thresholdDb, ratio, attackMs, releaseMs, makeupDb, kneeDb, enabled);
    emit compressorParametersChanged();

    if (m_pipeline) {
        // P0-14, P0-21: Configure executed on Control Thread and published lock-free via EpochReclaimer
        m_pipeline->updateCompressor(thresholdDb, ratio, attackMs, releaseMs, makeupDb, kneeDb, enabled);
    }
}

QVariantMap EngineController::getCompressorParams() {
    QVariantMap map;
    map["thresholdDb"] = m_uiComp.thresholdDb();
    map["ratio"] = m_uiComp.ratio();
    map["attackMs"] = m_uiComp.attackMs();
    map["releaseMs"] = m_uiComp.releaseMs();
    map["makeupGainDb"] = m_uiComp.makeupGainDb();
    map["kneeDb"] = m_uiComp.kneeDb();
    map["enabled"] = m_uiComp.isEnabled();
    map["bypassed"] = m_compBypassed.load(std::memory_order_relaxed);
    map["gainReductionDb"] = m_pipeline ? m_pipeline->compressorGainReductionDb() : 0.0;
    return map;
}

void EngineController::setLimiterParams(double ceilingDb, double releaseMs, double lookaheadMs) {
    bool enabled = !m_limiterBypassed.load(std::memory_order_relaxed);
    double sRate = m_pipeline ? m_pipeline->graphSampleRate() : 48000.0;
    m_uiLimiter.configure(sRate, ceilingDb, releaseMs, lookaheadMs, enabled);
    emit limiterParametersChanged();

    if (m_pipeline) {
        // P0-14, P0-21: Configure and instance preparation executed on Control Thread
        m_pipeline->updatePeakLimiter(ceilingDb, releaseMs, lookaheadMs, enabled);
    }
}

QVariantMap EngineController::getLimiterParams() {
    QVariantMap map;
    map["ceilingDb"] = m_uiLimiter.ceilingDb();
    map["releaseMs"] = m_uiLimiter.releaseMs();
    map["lookaheadMs"] = m_uiLimiter.lookaheadMs();
    map["latencyFrames"] = m_pipeline ? m_pipeline->limiterLatencyFrames() : static_cast<int>(m_uiLimiter.latencyFrames());
    map["enabled"] = m_uiLimiter.isEnabled();
    map["bypassed"] = m_limiterBypassed.load(std::memory_order_relaxed);
    map["gainReductionDb"] = m_pipeline ? m_pipeline->limiterGainReductionDb() : 0.0;
    return map;
}

void EngineController::setReverbParams(double roomSize, double damping, double wetLevel, double predelayMs, bool tailPreserving) {
    bool enabled = !m_reverbBypassed.load(std::memory_order_relaxed);
    double sRate = m_pipeline ? m_pipeline->graphSampleRate() : 48000.0;
    m_uiReverb.configure(sRate, roomSize, damping, wetLevel, predelayMs, tailPreserving, enabled);
    emit reverbParametersChanged();

    if (m_pipeline) {
        // P0-14, P0-21: Configure and instance preparation executed on Control Thread
        m_pipeline->updateReverb(roomSize, damping, wetLevel, predelayMs, tailPreserving, enabled);
    }
}

QVariantMap EngineController::getReverbParams() {
    QVariantMap map;
    map["roomSize"] = m_uiReverb.roomSize();
    map["damping"] = m_uiReverb.damping();
    map["wetLevel"] = m_pipeline ? m_pipeline->currentTelemetry().reverbWetLevel : m_uiReverb.wetLevel();
    map["predelayMs"] = m_uiReverb.predelayMs();
    map["tailPreserving"] = m_uiReverb.isTailPreserving();
    map["enabled"] = m_uiReverb.isEnabled();
    map["bypassed"] = m_reverbBypassed.load(std::memory_order_relaxed);
    return map;
}

QStringList EngineController::availableListeningPresets() {
    return QStringList{ "Monitor Flat", "Acoustic Clarity", "Harman Target", "Bass Punch", "Vocal Air" };
}

void EngineController::setListeningPreset(const QString& presetName) {
    if (m_pipeline) {
        // P0-27: Precompute preset coefficients on Control Thread and publish atomically via Mailbox
        m_pipeline->paramMailbox().setListeningPreset(presetName.toStdString());
    }
    bool ok = m_uiListeningCorrection.applyPreset(presetName.toStdString());
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

bool EngineController::findDspNode(const QString& nodeName, uint8_t& outChain, uint8_t& outIndex) const noexcept {
    if (nodeName == "PEQ") { outChain = 0; outIndex = 0; return true; }
    if (nodeName == "GEQ") { outChain = 0; outIndex = 1; return true; }
    if (nodeName == "Filter") { outChain = 0; outIndex = 2; return true; }
    if (nodeName == "Compressor") { outChain = 0; outIndex = 3; return true; }
    if (nodeName == "ExternalInsert" || nodeName == "Insert") { outChain = 0; outIndex = 4; return true; }
    if (nodeName == "Reverb") { outChain = 0; outIndex = 5; return true; }
    if (nodeName == "Limiter") { outChain = 0; outIndex = 6; return true; }
    if (nodeName == "ListeningCorrection") { outChain = 1; outIndex = 0; return true; }
    return false;
}

void EngineController::emitDspNodeSignal(const QString& nodeName) {
    if (nodeName == "PEQ") emit peqParametersChanged();
    else if (nodeName == "GEQ") emit geqParametersChanged();
    else if (nodeName == "Filter") emit filterParametersChanged();
    else if (nodeName == "Compressor") emit compressorParametersChanged();
    else if (nodeName == "ExternalInsert" || nodeName == "Insert") emit externalInsertStatusChanged();
    else if (nodeName == "Reverb") emit reverbParametersChanged();
    else if (nodeName == "Limiter") emit limiterParametersChanged();
    else if (nodeName == "ListeningCorrection") emit outputRoutingModeChanged(m_outputRoutingMode.load(std::memory_order_relaxed));
}

void EngineController::setDspNodeBypass(const QString& nodeName, bool bypass) {
    uint8_t targetChain = 0;
    uint8_t nodeIndex = 0;
    if (!findDspNode(nodeName, targetChain, nodeIndex)) {
        return;
    }

    const bool prevBypassed = isDspNodeBypassed(nodeName);
    if (prevBypassed == bypass) {
        return;
    }

    if (m_pipeline) {
        engine::dsp::DspCommand cmd;
        cmd.type = engine::dsp::DspCommandType::SetNodeBypass;
        cmd.index = nodeIndex;
        cmd.subIndex = targetChain;
        cmd.boolFlag1 = bypass ? 1 : 0;
        if (!m_pipeline->paramQueue().push(cmd)) {
            log::LogService::instance().log(
                "DSP",
                "NodeBypass",
                "Failed",
                QString("Failed to set bypass for %1: parameter queue full (code: 3001)").arg(nodeName),
                "DSPNode",
                nodeName
            );
            emit dspQueueOverflowWarning(static_cast<int>(cmd.type));
            // Ensure UI stays synchronized with actual DSP state on failure (P1-2)
            emitDspNodeSignal(nodeName);
            return;
        }
    }

    // Command successfully queued into Audio Thread: update state (P1-2)
    // Parameter values are strictly PRESERVED and not erased or reset on bypass
    if (nodeName == "PEQ") {
        m_peqBypassed.store(bypass, std::memory_order_relaxed);
    } else if (nodeName == "GEQ") {
        m_geqBypassed.store(bypass, std::memory_order_relaxed);
    } else if (nodeName == "Filter") {
        m_filterBypassed.store(bypass, std::memory_order_relaxed);
    } else if (nodeName == "Compressor") {
        m_compBypassed.store(bypass, std::memory_order_relaxed);
    } else if (nodeName == "ExternalInsert" || nodeName == "Insert") {
        m_insertBypassed.store(bypass, std::memory_order_relaxed);
        if (m_insertNode) {
            m_insertNode->setBypassed(bypass);
        }
    } else if (nodeName == "Reverb") {
        m_reverbBypassed.store(bypass, std::memory_order_relaxed);
    } else if (nodeName == "Limiter") {
        m_limiterBypassed.store(bypass, std::memory_order_relaxed);
    } else if (nodeName == "ListeningCorrection") {
        m_listeningCorrectionBypassed.store(bypass, std::memory_order_relaxed);
    }

    emitDspNodeSignal(nodeName);
    log::LogService::instance().log(
        "DSP",
        "NodeBypass",
        "Success",
        QString("%1 Bypass %2").arg(nodeName, bypass ? "Engaged" : "Disengaged"),
        "DSPNode",
        nodeName
    );
}

bool EngineController::resyncDspNodeBypass(const QString& nodeName) {
    if (!m_pipeline) return false;
    uint8_t targetChain = 0;
    uint8_t nodeIndex = 0;
    if (!findDspNode(nodeName, targetChain, nodeIndex)) {
        return false;
    }

    const bool state = isDspNodeBypassed(nodeName);
    engine::dsp::DspCommand cmd;
    cmd.type = engine::dsp::DspCommandType::SetNodeBypass;
    cmd.index = nodeIndex;
    cmd.subIndex = targetChain;
    cmd.boolFlag1 = state ? 1 : 0;

    if (m_pipeline->paramQueue().push(cmd)) {
        log::LogService::instance().log(
            "DSP",
            "NodeBypassResync",
            "Success",
            QString("%1 bypass resynchronized to %2").arg(nodeName, state ? "true" : "false"),
            "DSPNode",
            nodeName
        );
        emitDspNodeSignal(nodeName);
        return true;
    }

    log::LogService::instance().log(
        "DSP",
        "NodeBypassResync",
        "Failed",
        QString("Failed to resynchronize bypass for %1: queue full (code: 3006)").arg(nodeName),
        "DSPNode",
        nodeName
    );
    return false;
}

bool EngineController::isDspNodeBypassed(const QString& nodeName) {
    if (nodeName == "PEQ") return m_peqBypassed.load(std::memory_order_relaxed);
    if (nodeName == "GEQ") return m_geqBypassed.load(std::memory_order_relaxed);
    if (nodeName == "Filter") return m_filterBypassed.load(std::memory_order_relaxed);
    if (nodeName == "Compressor") return m_compBypassed.load(std::memory_order_relaxed);
    if (nodeName == "ExternalInsert" || nodeName == "Insert") return m_insertBypassed.load(std::memory_order_relaxed);
    if (nodeName == "Limiter") return m_limiterBypassed.load(std::memory_order_relaxed);
    if (nodeName == "Reverb") return m_reverbBypassed.load(std::memory_order_relaxed);
    if (nodeName == "ListeningCorrection") return m_listeningCorrectionBypassed.load(std::memory_order_relaxed);
    return false;
}

bool EngineController::isDspNodeAppliedBypassed(const QString& nodeName) const noexcept {
    if (!m_pipeline) return false;
    uint8_t targetChain = 0;
    uint8_t nodeIndex = 0;
    if (!findDspNode(nodeName, targetChain, nodeIndex)) {
        return false;
    }
    if (targetChain == 1) {
        return m_pipeline->isListeningNodeBypassed(nodeIndex);
    }
    return m_pipeline->isMasterNodeBypassed(nodeIndex);
}

void EngineController::renderOfflineBlock(float* outInterleaved, size_t frames) {
    if (m_audioOutput) {
        m_audioOutput->renderOfflineBlock(outInterleaved, frames);
    }
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
        QJsonObject det;
        det.insert(QStringLiteral("host_name"), clientName);
        det.insert(QStringLiteral("plugin_id"), QStringLiteral("external_jack_bridge"));
        det.insert(QStringLiteral("sample_rate_hz"), sRate);
        det.insert(QStringLiteral("buffer_size"), static_cast<int>(bSize));
        det.insert(QStringLiteral("reported_latency_frames"), reportedLatencyFrames);
        det.insert(QStringLiteral("reported_latency_ms"), (sRate > 0.0) ? (reportedLatencyFrames * 1000.0 / sRate) : 0.0);
        det.insert(QStringLiteral("latency_source"), QStringLiteral("host_reported"));
        log::LogService::instance().log(
            "ExternalInsert",
            "Configure",
            "Success",
            QString("Connected external insert host '%1' [Send: %2/%3, Return: %4/%5, Latency: %6 frames]")
                .arg(clientName, sendL, sendR, returnL, returnR).arg(reportedLatencyFrames),
            "InsertBridge",
            clientName,
            QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact))
        );
    } else {
        QJsonObject det;
        det.insert(QStringLiteral("host_name"), clientName);
        det.insert(QStringLiteral("stage"), QStringLiteral("bridge_configure"));
        det.insert(QStringLiteral("error_detail"), QString::fromStdString(m_insertNode->bridge().lastError()));
        log::LogService::instance().log(
            "ExternalInsert",
            "Configure",
            "Failed",
            QString("Failed to configure external insert host '%1': %2")
                .arg(clientName, QString::fromStdString(m_insertNode->bridge().lastError())),
            "InsertBridge",
            clientName,
            QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)),
            QString(),
            QString(),
            log::ErrorCode::ExternalHostFailed,
            0.0
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
    const auto& b = m_insertNode->bridge();
    const double sr = b.config().sampleRate;
    const double reportedMs = (sr > 0.0) ? (static_cast<double>(b.reportedLatencyFrames()) * 1000.0 / sr) : 0.0;
    QJsonObject det;
    det.insert(QStringLiteral("host_name"), QString::fromStdString(b.config().clientName));
    det.insert(QStringLiteral("reported_latency_frames"), static_cast<int>(b.reportedLatencyFrames()));
    det.insert(QStringLiteral("reported_latency_ms"), reportedMs);
    det.insert(QStringLiteral("measured_latency_frames"), static_cast<int>(b.measuredLatencyFrames()));
    det.insert(QStringLiteral("measured_latency_ms"), b.measuredLatencyMs());
    det.insert(QStringLiteral("latency_source"), QString::fromUtf8(engine::insert::ExternalInsertBridge::latencySourceToString(b.latencySource())));
    log::LogService::instance().log(
        "ExternalInsert",
        "RecordImpulseReturn",
        "Success",
        QString("Measured physical hardware round-trip latency: %1 frames (%2 ms).")
            .arg(b.measuredLatencyFrames())
            .arg(b.measuredLatencyMs(), 0, 'f', 2),
        "InsertBridge",
        QString::fromStdString(b.config().clientName),
        QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact))
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
    m_insertNode->setDelayCompensationFrames(static_cast<size_t>(std::max(0, frames)));
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
        QJsonObject det;
        det.insert(QStringLiteral("preset_id"), presetName);
        det.insert(QStringLiteral("category"), category);
        det.insert(QStringLiteral("stage"), QStringLiteral("preset_load"));
        log::LogService::instance().log(
            "PresetManagement",
            "ApplyPreset",
            "Failed",
            QString("Preset '%1' not found in category '%2'.").arg(presetName, category),
            category,
            presetName,
            QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)),
            QString(),
            QString(),
            log::ErrorCode::PresetNotFound,
            0.0
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
        if (obj.contains("compressor")) {
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
        if (obj.contains("limiter")) {
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
        if (obj.contains("filter")) {
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
        QJsonObject snapDet;
        snapDet.insert(QStringLiteral("preset_id"), presetName);
        snapDet.insert(QStringLiteral("category"), category);
        snapDet.insert(QStringLiteral("snapshot"), obj);
        log::LogService::instance().log(
            "PresetManagement",
            "ApplyPreset",
            "Success",
            QString("Applied Master DSP preset '%1'.").arg(presetName),
            category,
            presetName,
            QString::fromUtf8(QJsonDocument(snapDet).toJson(QJsonDocument::Compact))
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
        QJsonArray bands;
        for (size_t i = 0; i < 12; ++i) {
            const auto& b = m_uiListeningCorrection.leftBand(i);
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
    m_renderEngine.templateChain().copyParametersFrom(
        m_uiPeq,
        m_uiGeq,
        m_uiFilter,
        m_uiComp,
        m_uiReverb,
        m_uiLimiter
    );
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

void EngineController::cancelAllRenders() {
    m_renderEngine.cancelAllRenders();
    log::LogService::instance().log(
        "Render", "CancelAllRenders", "Info",
        "Cancellation requested for active render task and all queued tasks.",
        "RenderEngine", "Queue"
    );
    emit renderStateChanged();
}

void EngineController::resetRenderStatus() {
    if (m_renderEngine.isIdle()) {
        m_renderEngine.resetTaskInfo();
        emit renderStateChanged();
    }
}

void EngineController::setRenderMonitorVolume(double volume) {
    if (!m_pipeline) return;
    double clamped = std::clamp(volume, 0.0, 2.0);
    double prev = m_pipeline->renderMonitorVolume();
    if (std::abs(prev - clamped) > 1e-6) {
        m_pipeline->setRenderMonitorVolume(clamped);
        emit renderMonitorVolumeChanged(clamped);
    }
}

void EngineController::setListeningVolume(double volume) {
    if (!m_pipeline) return;
    double clamped = std::clamp(volume, 0.0, 2.0);
    double prev = m_pipeline->listeningVolume();
    if (std::abs(prev - clamped) > 1e-6) {
        m_pipeline->setListeningVolume(clamped);
        emit listeningVolumeChanged(clamped);
    }
}

bool EngineController::auditionRenderedFile(const QString& targetDeckId, const QString& filePath, bool switchToRenderMonitor) {
    if (!QFile::exists(filePath)) {
        qWarning() << "auditionRenderedFile: File does not exist:" << filePath;
        QJsonObject det;
        det.insert(QStringLiteral("path"), filePath);
        det.insert(QStringLiteral("stage"), QStringLiteral("file_exists_check"));
        log::LogService::instance().log(
            "Render", "Audition", "Failed",
            QString("Failed to audition rendered file '%1': file does not exist.").arg(filePath),
            "RenderAudition", filePath,
            QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)),
            QString(), QString(), log::ErrorCode::RenderAuditionFailed, 0.0
        );
        return false;
    }

    // 1. Load track into target deck
    bool loaded = loadTrack(targetDeckId, filePath);
    if (!loaded) {
        QJsonObject det;
        det.insert(QStringLiteral("path"), filePath);
        det.insert(QStringLiteral("deck"), targetDeckId);
        det.insert(QStringLiteral("stage"), QStringLiteral("load_track"));
        log::LogService::instance().log(
            "Render", "Audition", "Failed",
            QString("Failed to audition rendered file '%1' on Deck %2 (deck may be locked or format unsupported).")
                .arg(filePath, targetDeckId),
            "RenderAudition", filePath,
            QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)),
            QString(), QString(), log::ErrorCode::RenderAuditionFailed, 0.0
        );
        return false;
    }

    // 2. Select deck and optionally switch to Render Monitor Path (Section 5.11.5)
    switchDeck(targetDeckId, 0);
    if (switchToRenderMonitor) {
        setOutputRoutingMode(1); // 1 = RenderMonitorPath
    }

    const bool isPureMonitor = (m_outputRoutingMode.load(std::memory_order_relaxed) == 1);
    log::LogService::instance().log(
        "Render", "Audition", isPureMonitor ? "Success" : "Warning",
        isPureMonitor
            ? QString("Auditioning rendered file '%1' on Deck %2 (Render Monitor Path engaged).")
                  .arg(filePath, targetDeckId)
            : QString("Auditioning rendered file '%1' on Deck %2 via Listening Path (WARNING: Listening Correction is applied).")
                  .arg(filePath, targetDeckId),
        "RenderAudition", filePath
    );

    return true;
}

QVariantMap EngineController::getRenderStatus() const {
    QVariantMap map;
    auto task = m_renderEngine.currentTaskInfo();

    map["isIdle"] = m_renderEngine.isIdle();
    map["isRendering"] = m_renderEngine.isRendering();
    map["workerState"] = static_cast<int>(m_renderEngine.workerState());
    map["workerStateText"] = renderWorkerStateText();
    map["taskId"] = task.taskId;
    map["type"] = (task.type == engine::render::RenderType::Album) ? "Album" : "Track";
    map["status"] = static_cast<int>(task.status);
    map["statusText"] = renderStatusText();
    map["progress"] = task.progress;
    map["currentTrackIndex"] = (task.totalTracks > 0) ? (task.currentTrackIndex + 1) : 0;
    map["totalTracks"] = task.totalTracks;
    map["currentTrackName"] = task.currentTrackName;
    map["serial"] = task.serial;
    map["finalOutputDir"] = task.finalOutputDir;
    map["errorMessage"] = task.errorMessage;
    map["queuedTasksCount"] = m_renderEngine.queuedTasksCount();
    map["trackResults"] = task.trackResults;
    map["renderedFiles"] = task.renderedFiles;
    map["rollbackPerformed"] = task.rollbackPerformed;
    map["rollbackSucceeded"] = task.rollbackSucceeded;

    return map;
}

} // namespace desk32::core
