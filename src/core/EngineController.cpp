#include "EngineController.hpp"
#include "core/log/LogErrorCodes.hpp"
#include "engine/workspace/WorkspaceManager.hpp"
#include "git_version.hpp"
#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTimer>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace desk32::core {

EngineController &EngineController::instance() {
  static EngineController s_instance;
  return s_instance;
}

EngineController::EngineController(QObject *parent)
    : QObject(parent),
      m_presetManager(engine::preset::PresetManager::instance()),
      m_renderEngine(engine::render::RenderEngine::instance()) {
  connect(&m_renderEngine, &engine::render::RenderEngine::renderTaskStarted,
          this,
          [this](const QString &id, const QString &name, int totalTracks) {
            emit renderStateChanged();
            emit renderTaskStarted(id, name, totalTracks);
          });
  connect(&m_renderEngine, &engine::render::RenderEngine::renderProgressUpdated,
          this, [this]() { emit renderStateChanged(); });
  connect(&m_renderEngine, &engine::render::RenderEngine::renderTaskFinished,
          this,
          [this](const QString &id, int status, const QString &outDir,
                 const QString &err) {
            emit renderStateChanged();
            emit renderTaskFinished(id, status, outDir, err);
          });

  auto &wsMgr = engine::workspace::WorkspaceManager::instance();
  connect(&wsMgr, &engine::workspace::WorkspaceManager::workspaceStateChanged,
          this, [this]() {
            m_renderEngine.setWorkspacePath(
                engine::workspace::WorkspaceManager::instance()
                    .openWorkspacePath());
          });
}

EngineController::~EngineController() {
  if (m_audioOutput) {
    m_audioOutput->stop();
  }
}

bool EngineController::initialize(bool offlineMode) {
  // Reset bypass states to defaults matching node construction
  m_peqBypassed.store(true, std::memory_order_relaxed);
  m_geqBypassed.store(true, std::memory_order_relaxed);
  m_filterBypassed.store(true, std::memory_order_relaxed);
  m_compBypassed.store(true, std::memory_order_relaxed);
  m_insertBypassed.store(true, std::memory_order_relaxed);
  m_limiterBypassed.store(true, std::memory_order_relaxed);
  m_reverbBypassed.store(true, std::memory_order_relaxed);
  m_listeningCorrectionBypassed.store(false, std::memory_order_relaxed);
  m_globalDspBypass.store(false, std::memory_order_relaxed);
  m_globalBypassDesynced.store(false, std::memory_order_relaxed);
  m_directModeEnabled.store(false, std::memory_order_relaxed);
  m_fixedGraphSampleRate.store(48000.0, std::memory_order_relaxed);
  m_externalInsertPosition.store(4, std::memory_order_relaxed);
  for (uint8_t k = 0; k < 7; ++k) {
    m_masterNodeIndices[k].store(k, std::memory_order_relaxed);
  }
  m_syncMasterDspToRender.store(true, std::memory_order_relaxed);
  m_renderIncludePeq.store(true, std::memory_order_relaxed);
  m_renderIncludeGeq.store(true, std::memory_order_relaxed);
  m_renderIncludeFilter.store(true, std::memory_order_relaxed);
  m_renderIncludeCompressor.store(true, std::memory_order_relaxed);
  m_renderIncludeReverb.store(true, std::memory_order_relaxed);
  m_renderIncludeLimiter.store(true, std::memory_order_relaxed);

  // 1. Initialize 4-Deck Manager
  m_deckManager = std::make_unique<engine::deck::DeckManager>(48000.0);

  for (char cid : {'A', 'B', 'C', 'D'}) {
    if (auto *d = m_deckManager->deck(cid)) {
      d->setErrorLogCallback([this, cid](const std::string &act,
                                         int32_t code,
                                         const std::string &target,
                                         const std::string &msg) {
        log::LogService::instance().log(
            QStringLiteral("Deck"), QString::fromStdString(act),
            QStringLiteral("Warning"), QString::fromStdString(msg),
            QStringLiteral("AudioFile"), QString::fromStdString(target),
            QStringLiteral("{}"), QString(), QString(), code, 0.0);
        emit deckStateChanged(QString(cid));
        emit diagnosticsChanged();
      });
    }
  }

  auto canonPath = [](const std::string &p) -> QString {
    if (p.empty()) return QString();
    std::error_code ec;
    const auto cp = std::filesystem::weakly_canonical(p, ec);
    return ec ? QString::fromStdString(p) : QString::fromStdString(cp.string());
  };

  // Wire targeted workspace playback-stop guard (only stops decks matching affectedPaths)
  engine::workspace::WorkspaceManager::instance().setStopPlaybackCallback(
      [this, canonPath](const QStringList &affectedPaths) {
        if (!m_deckManager) return;
        QStringList canonAffected;
        canonAffected.reserve(affectedPaths.size());
        for (const QString &ap : affectedPaths) {
          canonAffected.append(canonPath(ap.toStdString()));
        }
        for (char cid : {'A', 'B', 'C', 'D'}) {
          if (auto *d = m_deckManager->deck(cid)) {
            const QString loadedCanon = canonPath(d->trackInfo().path);
            if (canonAffected.isEmpty() ||
                (!loadedCanon.isEmpty() && canonAffected.contains(loadedCanon))) {
              d->stop();
              emit deckStateChanged(QString(cid));
            }
          }
        }
      });

  // Wire workspace Stage-2 external modification metadata reload callback
  engine::workspace::WorkspaceManager::instance().setMetadataReloadCallback(
      [this, canonPath](const QStringList &modifiedAbsPaths) {
        if (!m_deckManager || modifiedAbsPaths.isEmpty()) return;
        QStringList canonMod;
        canonMod.reserve(modifiedAbsPaths.size());
        for (const QString &mp : modifiedAbsPaths) {
          canonMod.append(canonPath(mp.toStdString()));
        }
        for (char cid : {'A', 'B', 'C', 'D'}) {
          if (auto *d = m_deckManager->deck(cid)) {
            const QString loadedCanon = canonPath(d->trackInfo().path);
            if (!loadedCanon.isEmpty() && canonMod.contains(loadedCanon)) {
              d->reloadMetadata();
              emit deckStateChanged(QString(cid));
            }
          }
        }
      });
  m_renderEngine.setWorkspacePath(
      engine::workspace::WorkspaceManager::instance().openWorkspacePath());

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

  // Phase 3: External Insert Node (Default to OFF / Bypassed) + PipeWire Transport
  m_insertNode = std::make_shared<engine::insert::ExternalInsertNode>(
      "External Insert", engine::pipeline::NodeAffiliation::RenderCommon);
  m_insertTransport = std::make_shared<engine::insert::PipeWireInsertTransport>();
  m_insertTransport->setSimulationMode(offlineMode);
  m_insertNode->bridge().setTransport(m_insertTransport);
  m_insertNode->setBypassed(true);

  // 3. Construct Master and Listening Chains with isolated DSP nodes (P0-08,
  // P0-21) Section 5.1: Mastering DSP belongs in Master Chain (upstream of
  // Render Tap) Initial state: optional master DSPs start bypassed matching
  // spec §5.3-5.6 and EngineController model.
  auto masterChain = std::make_unique<engine::pipeline::AudioChain>(
      "Master Chain", engine::pipeline::ChainPurpose::MasteringChain);
  auto peqNode = std::make_shared<engine::pipeline::ParametricEqNode>(
      "Master PEQ", engine::pipeline::NodeAffiliation::RenderCommon);
  peqNode->setBypassed(true);
  masterChain->appendNode(std::move(peqNode));

  auto geqNode =
      std::make_shared<engine::pipeline::GraphicEqNode>("Graphic EQ");
  geqNode->setBypassed(true);
  masterChain->appendNode(std::move(geqNode));

  auto filterNode =
      std::make_shared<engine::pipeline::HighLowPassNode>("HPF / LPF");
  filterNode->setBypassed(true);
  masterChain->appendNode(std::move(filterNode));

  auto compNode =
      std::make_shared<engine::pipeline::CompressorNode>("Compressor");
  compNode->setBypassed(true);
  masterChain->appendNode(std::move(compNode));

  masterChain->appendNode(m_insertNode->clone());

  auto reverbNode = std::make_shared<engine::pipeline::ReverbNode>("Reverb");
  reverbNode->setBypassed(true);
  masterChain->appendNode(std::move(reverbNode));

  auto limiterNode =
      std::make_shared<engine::pipeline::PeakLimiterNode>("Peak Limiter");
  limiterNode->setBypassed(true);
  masterChain->appendNode(std::move(limiterNode));
  masterChain->configure(48000.0);

  // Listening Correction Chain strictly contains Listening Correction ONLY
  auto listeningChain = std::make_unique<engine::pipeline::AudioChain>(
      "Listening Chain",
      engine::pipeline::ChainPurpose::ListeningCorrectionChain);
  auto listenNode = std::make_shared<engine::pipeline::ParametricEqNode>(
      "Listening Correction",
      engine::pipeline::NodeAffiliation::ListeningCorrection);
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
    det.insert(QStringLiteral("stage"),
               QStringLiteral("pipewire_context_init"));
    log::LogService::instance().log(
        "AudioEngine", "Initialize", "Failed",
        "Failed to initialize PipeWire audio output context.", "System",
        "PipeWire",
        QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)),
        QString(), QString(), log::ErrorCode::PipeWireInitFailed, 0.0);
    emit diagnosticsChanged();
    return false;
  }

  // Wire format-aware real-time audio callback (§3.4, §4.3, strictly ZERO RT allocs)
  m_audioControlManager = std::make_unique<engine::backend::AudioControlManager>(
      std::make_unique<engine::backend::PipeWireControlAdapter>(m_audioOutput.get()));

  m_audioOutput->setRawPcmAudioCallback(
      [this](void *outInterleaved, size_t frames,
             engine::backend::OutputPcmFormat fmt) {
        using engine::backend::OutputPcmFormat;
        const size_t frameBytes =
            static_cast<size_t>(engine::backend::outputPcmFormatBytesPerSample(fmt)) * 2u;
        if (!m_deckManager || !m_pipeline) {
          std::memset(outInterleaved, 0, frames * frameBytes);
          return;
        }

        engine::format::SampleFormat sampleFmt = engine::format::SampleFormat::Float32;
        switch (fmt) {
        case OutputPcmFormat::F32LE:
          sampleFmt = engine::format::SampleFormat::Float32;
          break;
        case OutputPcmFormat::S24_32LE:
          sampleFmt = engine::format::SampleFormat::Int24_32;
          break;
        case OutputPcmFormat::S32LE:
          sampleFmt = engine::format::SampleFormat::Int32;
          break;
        case OutputPcmFormat::S16LE:
          sampleFmt = engine::format::SampleFormat::Int16;
          break;
        }

        const int curRoute = m_outputRoutingMode.load(std::memory_order_relaxed);
        m_pipeline->setOutputMode(
            curRoute == 1 ? engine::routing::OutputPathMode::RenderMonitorPath
                          : engine::routing::OutputPathMode::ListeningPath);

        size_t remaining = frames;
        size_t offset = 0;
        while (remaining > 0) {
          const size_t chunk = std::min(remaining, size_t(4096));
          m_deckManager->renderActiveBlock(m_audioInL.data(), m_audioInR.data(), chunk);

          const bool isMuted = m_masterMute.load(std::memory_order_relaxed);
          m_gainSmootherL.setTarget(
              isMuted ? 0.0 : m_masterGainL.load(std::memory_order_relaxed));
          m_gainSmootherR.setTarget(
              isMuted ? 0.0 : m_masterGainR.load(std::memory_order_relaxed));
          const auto *activeD = m_deckManager->activeDeck();
          const double levelMatchGain =
              activeD ? activeD->levelMatchLinearGain() : 1.0;
          const bool directBypass =
              m_directModeEnabled.load(std::memory_order_acquire);
          const bool formatUsesDither =
              (fmt == OutputPcmFormat::S24_32LE || fmt == OutputPcmFormat::S16LE);
          if (m_audioOutput) {
            m_audioOutput->setDitherActiveState(
                formatUsesDither && m_pipeline->liveDitherEnabled() && !directBypass);
          }

          uint8_t *chunkOut =
              static_cast<uint8_t *>(outInterleaved) + (offset * frameBytes);
          m_pipeline->process(m_audioInL.data(), m_audioInR.data(), chunk,
                              chunkOut, sampleFmt, true, &m_gainSmootherL,
                              &m_gainSmootherR, levelMatchGain, directBypass);

          remaining -= chunk;
          offset += chunk;
        }
      });

  bool startOk = m_audioOutput->start();
  if (!startOk) {
    // In headless testing or sandbox environments without an active PipeWire
    // server, enable offline simulation mode to allow full engine operation.
    m_audioOutput->setOfflineSimulationMode(true);
    if (m_insertTransport) {
      m_insertTransport->setSimulationMode(true);
    }
    startOk = m_audioOutput->start();
  }
  if (!startOk) {
    log::LogService::instance().start();
    QJsonObject det;
    det.insert(QStringLiteral("stage"),
               QStringLiteral("pipewire_stream_start"));
    log::LogService::instance().log(
        "AudioEngine", "StartStream", "Failed",
        "Failed to start PipeWire audio output stream.", "System", "PipeWire",
        QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)),
        QString(), QString(), log::ErrorCode::PipeWireStartFailed, 0.0);
    emit diagnosticsChanged();
    return false;
  }

  // Periodic check for seamless track auto-advancement in Folder Sessions
  // (P0-10)
  auto *trackTimer = new QTimer(this);
  connect(trackTimer, &QTimer::timeout, this, [this]() {
    if (!m_deckManager)
      return;
    for (char c : {'A', 'B', 'C', 'D'}) {
      auto *d = m_deckManager->deck(c);
      if (!d)
        continue;

      const auto eofResults = d->processPendingEof();
      for (const auto &res : eofResults) {
        auto rejectErrorCode =
            [](engine::deck::AutoAdvanceRejectReason r) -> int32_t {
          using R = engine::deck::AutoAdvanceRejectReason;
          switch (r) {
          case R::DuplicateEof:
            return log::ErrorCode::AutoAdvanceDuplicateEof;
          case R::StalePlaybackGeneration:
            return log::ErrorCode::AutoAdvanceStaleGeneration;
          case R::StaleDecoderRequest:
            return log::ErrorCode::AutoAdvanceStaleRequest;
          case R::TransitionInProgress:
            return log::ErrorCode::AutoAdvanceTransitionInProgress;
          case R::SessionInactive:
          case R::DeckLocked:
            return log::ErrorCode::AutoAdvanceSessionInactive;
          case R::TransportNotPlaying:
            return log::ErrorCode::AutoAdvanceNotPlaying;
          case R::TrackIdentityMismatch:
            return log::ErrorCode::AutoAdvanceTrackMismatch;
          case R::OpenTrackFailed:
            return log::ErrorCode::TrackOpenFailed;
          default:
            return 0;
          }
        };

        const QString eventSourceStr =
            QString::fromLatin1(engine::deck::toSourceString(res.source));
        const QString rejectReasonStr =
            QString::fromLatin1(engine::deck::toReasonString(res.rejectReason));

        // Log EOF notification diagnostics (both valid and rejected)
        {
          QJsonObject eofDet;
          eofDet.insert(QStringLiteral("deck"), QString(c));
          eofDet.insert(QStringLiteral("session_id"),
                        log::LogService::instance().sessionId());
          eofDet.insert(QStringLiteral("folder_session_id"),
                        static_cast<qint64>(res.folderSessionId));
          eofDet.insert(QStringLiteral("playback_generation"),
                        static_cast<qint64>(res.playbackGenerationBefore));
          eofDet.insert(QStringLiteral("decoder_request_id"),
                        static_cast<qint64>(res.decoderRequestIdBefore));
          eofDet.insert(QStringLiteral("track_identity"),
                        QString::fromStdString(res.pathFrom));
          eofDet.insert(QStringLiteral("event_source"), eventSourceStr);
          eofDet.insert(QStringLiteral("event_timestamp"),
                        static_cast<qint64>(res.eventTimestampMs));
          eofDet.insert(QStringLiteral("received_timestamp"),
                        static_cast<qint64>(res.receivedTimestampMs));
          eofDet.insert(QStringLiteral("valid"), res.validNotification);
          if (d->eofQueueOverflowCount() > 0) {
            eofDet.insert(QStringLiteral("eof_queue_overflow_count"),
                          static_cast<qint64>(d->eofQueueOverflowCount()));
          }
          if (d->eofQueueFullCount() > 0) {
            eofDet.insert(QStringLiteral("eof_queue_full_count"),
                          static_cast<qint64>(d->eofQueueFullCount()));
          }
          if (d->eofQueueCasContentionCount() > 0) {
            eofDet.insert(QStringLiteral("eof_queue_cas_contention_count"),
                          static_cast<qint64>(d->eofQueueCasContentionCount()));
          }
          if (d->eofOverflowCasContentionCount() > 0) {
            eofDet.insert(
                QStringLiteral("eof_overflow_cas_contention_count"),
                static_cast<qint64>(d->eofOverflowCasContentionCount()));
          }
          if (d->eofOverflowDropCount() > 0) {
            eofDet.insert(QStringLiteral("eof_overflow_drop_count"),
                          static_cast<qint64>(d->eofOverflowDropCount()));
          }
          if (!res.validNotification) {
            eofDet.insert(QStringLiteral("invalid_reason"), rejectReasonStr);
          }
          log::LogService::instance().log(
              QStringLiteral("FolderSession"),
              QStringLiteral("EofNotification"),
              res.validNotification ? QStringLiteral("Success")
                                    : QStringLiteral("Ignored"),
              res.validNotification
                  ? QString(
                        "Valid EOF notification on Deck %1 (gen=%2, req=%3)")
                        .arg(c)
                        .arg(res.playbackGenerationBefore)
                        .arg(res.decoderRequestIdBefore)
                  : QString("Ignored EOF notification on Deck %1 (%2)")
                        .arg(c, rejectReasonStr),
              QStringLiteral("Deck"), QString(c),
              QString::fromUtf8(
                  QJsonDocument(eofDet).toJson(QJsonDocument::Compact)),
              QString(), QString(),
              res.validNotification ? 0 : rejectErrorCode(res.rejectReason),
              0.0);
        }

        if (!res.executed) {
          continue;
        }

        if (res.advanced) {
          ++m_advanceSeq;
          QJsonObject det;
          det.insert(QStringLiteral("session_id"),
                     log::LogService::instance().sessionId());
          det.insert(QStringLiteral("folder_session_id"),
                     static_cast<qint64>(res.folderSessionId));
          det.insert(QStringLiteral("deck"), QString(c));
          det.insert(QStringLiteral("advance_seq"),
                     static_cast<qint64>(m_advanceSeq));
          det.insert(QStringLiteral("playback_generation"),
                     static_cast<qint64>(res.playbackGenerationAfter));
          det.insert(QStringLiteral("playback_generation_before"),
                     static_cast<qint64>(res.playbackGenerationBefore));
          det.insert(QStringLiteral("decoder_request_id"),
                     static_cast<qint64>(res.decoderRequestIdAfter));
          det.insert(QStringLiteral("decoder_request_id_before"),
                     static_cast<qint64>(res.decoderRequestIdBefore));
          det.insert(QStringLiteral("track_index_from"), res.trackIndexFrom);
          det.insert(QStringLiteral("track_index_to"), res.trackIndexTo);
          det.insert(QStringLiteral("path_from"),
                     QString::fromStdString(res.pathFrom));
          det.insert(QStringLiteral("path_to"),
                     QString::fromStdString(res.pathTo));
          det.insert(QStringLiteral("trigger"), QStringLiteral("auto_eof"));
          det.insert(QStringLiteral("event_source"), eventSourceStr);
          det.insert(QStringLiteral("event_timestamp"),
                     static_cast<qint64>(res.eventTimestampMs));
          det.insert(QStringLiteral("transition_start_timestamp"),
                     static_cast<qint64>(res.transitionStartTimestampMs));
          det.insert(QStringLiteral("transition_complete_timestamp"),
                     static_cast<qint64>(res.transitionCompleteTimestampMs));
          det.insert(QStringLiteral("result"), QStringLiteral("success"));
          det.insert(QStringLiteral("failure_reason"), QString());

          const auto openSnap = d->worker().readOpenTransitionSnapshot();
          if (openSnap.valid) {
            QJsonObject ot;
            ot.insert(QStringLiteral("open_seq"),
                      static_cast<qint64>(openSnap.openSeq));
            ot.insert(QStringLiteral("request_id"),
                      static_cast<qint64>(openSnap.requestId));
            ot.insert(QStringLiteral("open_wait_ms"),
                      openSnap.openRequestedNs > 0 &&
                              openSnap.openProcessStartNs >=
                                  openSnap.openRequestedNs
                          ? static_cast<double>(openSnap.openProcessStartNs -
                                                openSnap.openRequestedNs) /
                                1e6
                          : 0.0);
            ot.insert(QStringLiteral("worker_wake_delay_ms"),
                      openSnap.cmdNotifiedNs > 0 &&
                              openSnap.workerWokeNs >= openSnap.cmdNotifiedNs
                          ? static_cast<double>(openSnap.workerWokeNs -
                                                openSnap.cmdNotifiedNs) /
                                1e6
                          : 0.0);
            ot.insert(QStringLiteral("decoder_open_ms"),
                      static_cast<double>(std::max<int64_t>(
                          0, openSnap.decoderOpenEndNs -
                                 openSnap.decoderOpenStartNs)) /
                          1e6);
            ot.insert(
                QStringLiteral("eof_to_play_cleared_ms"),
                static_cast<double>(std::max<int64_t>(
                    0, openSnap.playingClearedNs - openSnap.eofClearedNs)) /
                    1e6);
            ot.insert(QStringLiteral("resampler_config_ms"),
                      static_cast<double>(std::max<int64_t>(
                          0, openSnap.resamplerConfigEndNs -
                                 openSnap.resamplerConfigStartNs)) /
                          1e6);
            ot.insert(QStringLiteral("prefill_ms"),
                      static_cast<double>(std::max<int64_t>(
                          0, openSnap.prefillEndNs - openSnap.prefillStartNs)) /
                          1e6);
            ot.insert(QStringLiteral("first_pcm_ms"),
                      openSnap.openRequestedNs > 0 &&
                              openSnap.firstPcmSupplyNs >=
                                  openSnap.openRequestedNs
                          ? static_cast<double>(openSnap.firstPcmSupplyNs -
                                                openSnap.openRequestedNs) /
                                1e6
                          : 0.0);
            ot.insert(QStringLiteral("total_open_ms"),
                      openSnap.openRequestedNs > 0 &&
                              openSnap.openCompleteNs >=
                                  openSnap.openRequestedNs
                          ? static_cast<double>(openSnap.openCompleteNs -
                                                openSnap.openRequestedNs) /
                                1e6
                          : 0.0);
            ot.insert(QStringLiteral("prefill_input_frames"),
                      static_cast<qint64>(openSnap.prefillInputFrames));
            ot.insert(QStringLiteral("prefill_ring_frames"),
                      static_cast<qint64>(openSnap.prefillRingAvailAfter));
            ot.insert(QStringLiteral("eof_before"), openSnap.eofBefore);
            ot.insert(QStringLiteral("playing_before"), openSnap.playingBefore);
            det.insert(QStringLiteral("open_transition"), ot);
          }

          log::LogService::instance().log(
              QStringLiteral("FolderSession"), QStringLiteral("AutoAdvance"),
              QStringLiteral("Success"),
              QString("Auto-advanced Deck %1 [%2 -> %3] (seq=%4)")
                  .arg(c)
                  .arg(res.trackIndexFrom)
                  .arg(res.trackIndexTo)
                  .arg(m_advanceSeq),
              QStringLiteral("Deck"), QString(c),
              QString::fromUtf8(
                  QJsonDocument(det).toJson(QJsonDocument::Compact)));
        } else {
          const bool completed =
              (res.rejectReason ==
               engine::deck::AutoAdvanceRejectReason::EndOfFolder);
          QJsonObject det;
          det.insert(QStringLiteral("session_id"),
                     log::LogService::instance().sessionId());
          det.insert(QStringLiteral("folder_session_id"),
                     static_cast<qint64>(res.folderSessionId));
          det.insert(QStringLiteral("deck"), QString(c));
          det.insert(QStringLiteral("advance_seq"),
                     static_cast<qint64>(m_advanceSeq));
          det.insert(QStringLiteral("playback_generation"),
                     static_cast<qint64>(res.playbackGenerationAfter));
          det.insert(QStringLiteral("decoder_request_id"),
                     static_cast<qint64>(res.decoderRequestIdAfter));
          det.insert(QStringLiteral("track_index_from"), res.trackIndexFrom);
          det.insert(QStringLiteral("track_index_to"), res.trackIndexTo);
          det.insert(QStringLiteral("path_from"),
                     QString::fromStdString(res.pathFrom));
          det.insert(QStringLiteral("path_to"),
                     QString::fromStdString(res.pathTo));
          det.insert(QStringLiteral("trigger"), QStringLiteral("auto_eof"));
          det.insert(QStringLiteral("event_source"), eventSourceStr);
          det.insert(QStringLiteral("event_timestamp"),
                     static_cast<qint64>(res.eventTimestampMs));
          det.insert(QStringLiteral("transition_start_timestamp"),
                     static_cast<qint64>(res.transitionStartTimestampMs));
          det.insert(QStringLiteral("transition_complete_timestamp"),
                     static_cast<qint64>(res.transitionCompleteTimestampMs));
          det.insert(QStringLiteral("result"), completed
                                                   ? QStringLiteral("completed")
                                                   : QStringLiteral("failed"));
          det.insert(QStringLiteral("failure_reason"), rejectReasonStr);
          det.insert(QStringLiteral("reason"), rejectReasonStr);
          det.insert(QStringLiteral("tracks_played"), res.trackIndexFrom + 1);
          log::LogService::instance().log(
              QStringLiteral("FolderSession"), QStringLiteral("AutoAdvance"),
              completed ? QStringLiteral("Info") : QStringLiteral("Failed"),
              completed
                  ? QString("Folder session completed in Deck %1").arg(c)
                  : QString("Folder session AutoAdvance failed in Deck %1 (%2)")
                        .arg(c, rejectReasonStr),
              QStringLiteral("Deck"), QString(c),
              QString::fromUtf8(
                  QJsonDocument(det).toJson(QJsonDocument::Compact)),
              QString(), QString(),
              completed ? 0 : rejectErrorCode(res.rejectReason), 0.0);
        }
        emit deckStateChanged(QString(c));
      }

      if (d->checkAndResetTrackChanged()) {
        emit deckStateChanged(QString(c));
      }
    }
  });
  trackTimer->start(50);

  // Periodic check for dynamic DSP telemetry (Compressor / Limiter GR, VU
  // levels) & audio health
  auto *dspTimer = new QTimer(this);
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
    m_lastGraphBufferFrames = (diag.negotiatedQuantum > 0)
                                  ? diag.negotiatedQuantum
                                  : diag.bufferFrames;
    m_lastDirectModeStatus = static_cast<int>(diag.directModeStatus);
  }
  m_lastDroppedLogCount = log::LogService::instance().droppedLogCount();
  m_lastDeckUnderruns = {0, 0, 0, 0};

  log::LogService::instance().log(
      "AudioEngine", "Initialize", "Success",
      "Desk32 Studio Console engine initialized successfully.", "System",
      "PipeWire");

  return true;
}

void EngineController::recordSessionStart(const QString &graphicsBackend,
                                          const QString &gpuName) {
  if (m_sessionStartRecorded)
    return;
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
  det.insert(QStringLiteral("desktop_env"),
             QString::fromUtf8(qgetenv("XDG_CURRENT_DESKTOP")));
  det.insert(QStringLiteral("graphics_backend"), graphicsBackend);
  det.insert(QStringLiteral("gpu_name"), gpuName);

  if (m_audioOutput) {
    const auto diag = m_audioOutput->diagnostics();
    const uint32_t effectiveQuantum = (diag.negotiatedQuantum > 0)
                                          ? diag.negotiatedQuantum
                                          : diag.bufferFrames;
    det.insert(QStringLiteral("pipewire_sample_rate_hz"),
               static_cast<int>(diag.sampleRate));
    det.insert(QStringLiteral("pipewire_quantum"),
               static_cast<int>(effectiveQuantum));
    det.insert(QStringLiteral("pipewire_requested_quantum"),
               static_cast<int>(diag.bufferFrames));
    det.insert(QStringLiteral("pipewire_direct_mode"),
               static_cast<int>(diag.directModeStatus));
    det.insert(QStringLiteral("pipewire_device"),
               QString::fromStdString(diag.deviceName));
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
      QStringLiteral("Session"), QStringLiteral("SessionStart"),
      QStringLiteral("Info"),
      QString("Session started (commit=%1, backend=%2, platform=%3)")
          .arg(det["git_commit"].toString(), graphicsBackend,
               det["platform"].toString()),
      QStringLiteral("System"), QStringLiteral("Desk32"),
      QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)));
}

void EngineController::recordSessionEnd(const QString &reason) {
  if (m_sessionEndRecorded)
    return;
  m_sessionEndRecorded = true;

  const auto now = std::chrono::steady_clock::now();
  const double uptimeMs =
      (m_sessionStartTime.time_since_epoch().count() > 0)
          ? std::chrono::duration<double, std::milli>(now - m_sessionStartTime)
                .count()
          : 0.0;
  const uint64_t totalXruns =
      m_audioOutput ? m_audioOutput->diagnostics().xrunCount : 0;

  QJsonObject det;
  det.insert(QStringLiteral("reason"), reason);
  det.insert(QStringLiteral("uptime_ms"), uptimeMs);
  det.insert(QStringLiteral("auto_advance_count"),
             static_cast<qint64>(m_advanceSeq));
  det.insert(QStringLiteral("total_xruns"), static_cast<qint64>(totalXruns));

  log::LogService::instance().log(
      QStringLiteral("Session"), QStringLiteral("SessionEnd"),
      QStringLiteral("Info"),
      QString("Session ended (%1, uptime=%2 ms, auto_advance=%3, xruns=%4)")
          .arg(reason)
          .arg(uptimeMs, 0, 'f', 0)
          .arg(m_advanceSeq)
          .arg(totalXruns),
      QStringLiteral("System"), QStringLiteral("Desk32"),
      QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)),
      QString(), QString(), 0, uptimeMs);
}

void EngineController::checkAudioHealthAndGraph() {
  if (m_audioOutput) {
    const auto diag = m_audioOutput->diagnostics();
    const uint32_t effectiveQuantum = (diag.negotiatedQuantum > 0)
                                          ? diag.negotiatedQuantum
                                          : diag.bufferFrames;

    // 1. Detect graph parameter changes (§6)
    const int curDirect = static_cast<int>(diag.directModeStatus);
    if (diag.sampleRate != m_lastGraphSampleRate ||
        effectiveQuantum != m_lastGraphBufferFrames ||
        curDirect != m_lastDirectModeStatus) {
      QJsonObject det;
      det.insert(QStringLiteral("sample_rate_hz_before"),
                 static_cast<int>(m_lastGraphSampleRate));
      det.insert(QStringLiteral("sample_rate_hz_after"),
                 static_cast<int>(diag.sampleRate));
      det.insert(QStringLiteral("quantum_before"),
                 static_cast<int>(m_lastGraphBufferFrames));
      det.insert(QStringLiteral("quantum_after"),
                 static_cast<int>(effectiveQuantum));
      det.insert(QStringLiteral("direct_mode_before"), m_lastDirectModeStatus);
      det.insert(QStringLiteral("direct_mode_after"), curDirect);
      det.insert(QStringLiteral("device"),
                 QString::fromStdString(diag.deviceName));

      log::LogService::instance().log(
          QStringLiteral("AudioEngine"), QStringLiteral("GraphChanged"),
          QStringLiteral("Info"),
          QString("Audio graph changed: %1Hz/%2f -> %3Hz/%4f")
              .arg(m_lastGraphSampleRate)
              .arg(m_lastGraphBufferFrames)
              .arg(diag.sampleRate)
              .arg(effectiveQuantum),
          QStringLiteral("Device"), QString::fromStdString(diag.deviceName),
          QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)));

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
        std::chrono::duration<double, std::milli>(now - m_lastXrunLogTime)
                .count() >= 1000.0) {
      const uint64_t delta = m_pendingXrunDelta;
      m_pendingXrunDelta = 0;
      m_lastXrunLogTime = now;

      QJsonObject det;
      det.insert(QStringLiteral("source"), QStringLiteral("pipewire_report"));
      det.insert(QStringLiteral("xrun_delta"), static_cast<qint64>(delta));
      det.insert(QStringLiteral("xrun_total"),
                 static_cast<qint64>(diag.xrunCount));
      det.insert(QStringLiteral("sample_rate_hz"),
                 static_cast<int>(diag.sampleRate));
      det.insert(QStringLiteral("quantum"), static_cast<int>(effectiveQuantum));
      det.insert(QStringLiteral("device"),
                 QString::fromStdString(diag.deviceName));
      if (m_deckManager) {
        det.insert(QStringLiteral("deck"),
                   QString(m_deckManager->activeDeckId()));
      }

      log::LogService::instance().log(
          QStringLiteral("AudioEngine"), QStringLiteral("XRUN"),
          QStringLiteral("Warning"),
          QString("Detected %1 PipeWire XRUN(s) (total: %2)")
              .arg(delta)
              .arg(diag.xrunCount),
          QStringLiteral("Device"), QString::fromStdString(diag.deviceName),
          QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)),
          QString(), QString(), log::ErrorCode::AudioXrunDetected, 0.0);
    }
  }

  // 3. DecoderWorker buffer underrun separation (§6)
  if (m_deckManager) {
    const std::array<char, 4> deckIds{'A', 'B', 'C', 'D'};
    for (size_t i = 0; i < deckIds.size(); ++i) {
      auto *d = m_deckManager->deck(deckIds[i]);
      if (!d)
        continue;
      const uint64_t curUnderruns = d->worker().underrunCount();
      if (curUnderruns > m_lastDeckUnderruns[i]) {
        const uint64_t delta = curUnderruns - m_lastDeckUnderruns[i];
        m_lastDeckUnderruns[i] = curUnderruns;
        QJsonObject det;
        det.insert(QStringLiteral("source"),
                   QStringLiteral("decoder_underrun"));
        det.insert(QStringLiteral("deck"), QString(deckIds[i]));
        det.insert(QStringLiteral("underrun_delta"),
                   static_cast<qint64>(delta));
        det.insert(QStringLiteral("underrun_total"),
                   static_cast<qint64>(curUnderruns));
        det.insert(QStringLiteral("path"),
                   QString::fromStdString(d->trackInfo().path));
        det.insert(QStringLiteral("session_id"),
                   log::LogService::instance().sessionId());

        QJsonArray snapArr;
        engine::decoder::UnderrunSnapshot uSnap{};
        while (d->worker().popUnderrunSnapshot(uSnap)) {
          QJsonObject sObj;
          sObj.insert(QStringLiteral("steady_ns"),
                      static_cast<qint64>(uSnap.steadyNs));
          sObj.insert(QStringLiteral("underrun_total"),
                      static_cast<qint64>(uSnap.underrunTotal));
          sObj.insert(QStringLiteral("request_id"),
                      static_cast<qint64>(uSnap.requestId));
          sObj.insert(QStringLiteral("active_request_id"),
                      static_cast<qint64>(uSnap.activeRequestId));
          sObj.insert(QStringLiteral("open_seq"),
                      static_cast<qint64>(uSnap.openSeq));
          sObj.insert(QStringLiteral("eof_epoch"),
                      static_cast<qint64>(uSnap.eofEpoch));
          sObj.insert(QStringLiteral("open_phase"),
                      static_cast<int>(uSnap.openPhase));
          sObj.insert(QStringLiteral("is_open_entry"), uSnap.isOpenAtEntry);
          sObj.insert(QStringLiteral("is_playing_entry"),
                      uSnap.isPlayingAtEntry);
          sObj.insert(QStringLiteral("is_eof_check"), uSnap.isEofAtCheck);
          sObj.insert(QStringLiteral("is_open_underrun"),
                      uSnap.isOpenAtUnderrun);
          sObj.insert(QStringLiteral("is_playing_underrun"),
                      uSnap.isPlayingAtUnderrun);
          sObj.insert(QStringLiteral("ring_capacity_frames"),
                      static_cast<qint64>(uSnap.ringCapacityFrames));
          sObj.insert(QStringLiteral("ring_avail_frames"),
                      static_cast<qint64>(uSnap.ringAvailBeforeRead));
          const double gRate =
              (m_lastGraphSampleRate > 0.0) ? m_lastGraphSampleRate : 48000.0;
          sObj.insert(
              QStringLiteral("ring_avail_ms"),
              (static_cast<double>(uSnap.ringAvailBeforeRead) * 1000.0) /
                  gRate);
          sObj.insert(QStringLiteral("requested_frames"),
                      static_cast<qint64>(uSnap.requestedFrames));
          sObj.insert(QStringLiteral("read_frames"),
                      static_cast<qint64>(uSnap.readFrames));
          snapArr.append(sObj);
        }
        det.insert(QStringLiteral("underrun_snapshots"), snapArr);
        det.insert(
            QStringLiteral("snapshot_dropped"),
            static_cast<qint64>(d->worker().underrunSnapshotDropCount()));

        const auto openSnap = d->worker().readOpenTransitionSnapshot();
        if (openSnap.valid) {
          QJsonObject ot;
          ot.insert(QStringLiteral("open_seq"),
                    static_cast<qint64>(openSnap.openSeq));
          ot.insert(QStringLiteral("request_id"),
                    static_cast<qint64>(openSnap.requestId));
          ot.insert(QStringLiteral("open_requested_ns"),
                    static_cast<qint64>(openSnap.openRequestedNs));
          ot.insert(QStringLiteral("eof_cleared_ns"),
                    static_cast<qint64>(openSnap.eofClearedNs));
          ot.insert(QStringLiteral("playing_cleared_ns"),
                    static_cast<qint64>(openSnap.playingClearedNs));
          ot.insert(QStringLiteral("eof_to_play_cleared_ms"),
                    static_cast<double>(std::max<int64_t>(
                        0, openSnap.playingClearedNs - openSnap.eofClearedNs)) /
                        1e6);
          ot.insert(QStringLiteral("open_complete_ns"),
                    static_cast<qint64>(openSnap.openCompleteNs));
          ot.insert(QStringLiteral("play_requested_ns"),
                    static_cast<qint64>(openSnap.playRequestedNs));
          ot.insert(QStringLiteral("prefill_ring_frames"),
                    static_cast<qint64>(openSnap.prefillRingAvailAfter));
          det.insert(QStringLiteral("last_open_transition"), ot);
        }

        const auto perf = d->worker().readDecodePerfStats();
        QJsonObject pf;
        pf.insert(QStringLiteral("max_chunk_total_ms"),
                  static_cast<double>(perf.maxChunkTotalNs) / 1e6);
        pf.insert(QStringLiteral("max_read_frames_ms"),
                  static_cast<double>(perf.maxReadFramesNs) / 1e6);
        pf.insert(QStringLiteral("max_file_read_packet_ms"),
                  static_cast<double>(perf.maxFileReadPacketNs) / 1e6);
        pf.insert(QStringLiteral("max_resample_ms"),
                  static_cast<double>(perf.maxResampleNs) / 1e6);
        pf.insert(QStringLiteral("max_worker_iter_interval_ms"),
                  static_cast<double>(perf.maxWorkerIterIntervalNs) / 1e6);
        pf.insert(QStringLiteral("total_chunks_decoded"),
                  static_cast<qint64>(perf.totalChunksDecoded));
        det.insert(QStringLiteral("decoder_perf"), pf);

        if (m_audioOutput) {
          const auto diag = m_audioOutput->diagnostics();
          QJsonObject cb;
          cb.insert(QStringLiteral("expected_period_ms"),
                    static_cast<double>(diag.expectedPeriodNs) / 1e6);
          cb.insert(QStringLiteral("last_cb_duration_ms"),
                    static_cast<double>(diag.lastCallbackDurationNs) / 1e6);
          cb.insert(QStringLiteral("max_cb_duration_ms"),
                    static_cast<double>(diag.maxCallbackDurationNs) / 1e6);
          cb.insert(QStringLiteral("last_sched_jitter_ms"),
                    static_cast<double>(diag.lastSchedJitterNs) / 1e6);
          cb.insert(QStringLiteral("max_abs_sched_jitter_ms"),
                    static_cast<double>(diag.maxAbsSchedJitterNs) / 1e6);
          det.insert(QStringLiteral("audio_callback"), cb);
        }

        log::LogService::instance().log(
            QStringLiteral("AudioEngine"), QStringLiteral("Underrun"),
            QStringLiteral("Warning"),
            QString("Deck %1 decoder ring-buffer underrun (+%2, total: %3)")
                .arg(deckIds[i])
                .arg(delta)
                .arg(curUnderruns),
            QStringLiteral("Deck"), QString(deckIds[i]),
            QString::fromUtf8(
                QJsonDocument(det).toJson(QJsonDocument::Compact)),
            QString(), QString(), log::ErrorCode::DecoderUnderrunDetected, 0.0);
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
    det.insert(QStringLiteral("dropped_total"),
               static_cast<qint64>(curDropped));
    log::LogService::instance().log(
        QStringLiteral("System"), QStringLiteral("LogDropped"),
        QStringLiteral("Warning"),
        QString("LogService bounded queue dropped %1 event(s) (total: %2)")
            .arg(delta)
            .arg(curDropped),
        QStringLiteral("System"), QStringLiteral("LogService"),
        QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)),
        QString(), QString(), log::ErrorCode::LogQueueOverflow, 0.0);
  }

  // 5. External Insert DisconnectPolicy::StopPlayback enforcement
  if (m_insertNode && m_insertNode->bridge().stopPlaybackRequested()) {
    m_insertNode->bridge().clearStopPlaybackRequested();
    if (m_deckManager) {
      for (char cid : {'A', 'B', 'C', 'D'}) {
        if (auto *d = m_deckManager->deck(cid)) {
          d->stop();
          emit deckStateChanged(QString(cid));
        }
      }
    }
    log::LogService::instance().log(
        QStringLiteral("ExternalInsert"), QStringLiteral("StopPlaybackPolicy"),
        QStringLiteral("Warning"),
        QStringLiteral("External Insert disconnected with StopPlayback policy: stopped active deck playback."),
        QStringLiteral("InsertBridge"),
        QString::fromStdString(m_insertNode->bridge().config().clientName),
        QStringLiteral("{}"), QString(), QString(),
        log::ErrorCode::ExternalHostFailed, 0.0);
    emit externalInsertStatusChanged();
    emit diagnosticsChanged();
  }

  // 6. Poll live External Insert transport mismatch / graph disconnect events (§8.3)
  if (m_insertNode && m_insertTransport) {
    const auto st = m_insertNode->bridge().state();
    if (st == engine::insert::InsertState::Connected ||
        st == engine::insert::InsertState::Active) {
      if (m_insertTransport->consumeGraphDisconnectEvent()) {
        m_insertNode->bridge().handleDisconnect();
        m_insertNode->setBypassed(true);
        m_insertBypassed.store(true, std::memory_order_relaxed);
        log::LogService::instance().log(
            QStringLiteral("ExternalInsert"), QStringLiteral("GraphDisconnect"),
            QStringLiteral("Warning"),
            QStringLiteral("PipeWire external insert node/port removed; auto-bypassed."),
            QStringLiteral("InsertBridge"),
            QString::fromStdString(m_insertNode->bridge().config().clientName),
            QStringLiteral("{}"), QString(), QString(),
            log::ErrorCode::ExternalHostFailed, 0.0);
        emit externalInsertStatusChanged();
        emit diagnosticsChanged();
      } else if (m_insertTransport->hasRateMismatch() ||
                 m_insertTransport->hasBufferMismatch()) {
        m_insertNode->bridge().handleDisconnect();
        m_insertNode->setBypassed(true);
        m_insertBypassed.store(true, std::memory_order_relaxed);
        emit externalInsertStatusChanged();
        emit diagnosticsChanged();
      }
    }
  }
}

void EngineController::copyToClipboard(const QString &text) {
  if (auto *cb = QGuiApplication::clipboard()) {
    cb->setText(text);
  }
}

void EngineController::commitDspParameterChange(const QString &dspNode,
                                                const QVariantMap &beforeMap,
                                                const QVariantMap &afterMap,
                                                const QString &trigger) {
  QJsonObject det;
  det.insert(QStringLiteral("dsp_node"), dspNode);
  det.insert(QStringLiteral("trigger"), trigger);
  det.insert(QStringLiteral("before"), QJsonObject::fromVariantMap(beforeMap));
  det.insert(QStringLiteral("after"), QJsonObject::fromVariantMap(afterMap));

  log::LogService::instance().log(
      QStringLiteral("DSP"), QStringLiteral("ParameterChange"),
      QStringLiteral("Success"),
      QString("Updated %1 parameters (trigger: %2)").arg(dspNode, trigger),
      QStringLiteral("DSPNode"), dspNode,
      QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)));
}

QString EngineController::activeDeckId() const {
  if (!m_deckManager)
    return "A";
  return QString(m_deckManager->activeDeckId());
}

double EngineController::graphSampleRate() const {
  if (!m_deckManager)
    return 48000.0;
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

double EngineController::preDspPeakL() const {
  return m_pipeline ? m_pipeline->preDspPeakL() : 0.0;
}

double EngineController::preDspPeakR() const {
  return m_pipeline ? m_pipeline->preDspPeakR() : 0.0;
}

double EngineController::preDspRmsL() const {
  return m_pipeline ? m_pipeline->preDspRmsL() : 0.0;
}

double EngineController::preDspRmsR() const {
  return m_pipeline ? m_pipeline->preDspRmsR() : 0.0;
}

double EngineController::renderTapPeakL() const {
  return m_pipeline ? m_pipeline->currentTelemetry().renderTapPeakL : 0.0;
}

double EngineController::renderTapPeakR() const {
  return m_pipeline ? m_pipeline->currentTelemetry().renderTapPeakR : 0.0;
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
  if (peak <= 0.000001)
    return 96.0;
  return -20.0 * std::log10(peak);
}

int EngineController::xrunCount() const {
  if (!m_audioOutput)
    return 0;
  return static_cast<int>(m_audioOutput->diagnostics().xrunCount);
}

bool EngineController::isDirectMode() const {
  if (!m_audioOutput)
    return false;
  return m_audioOutput->diagnostics().isDirectMode;
}

double EngineController::lastRenegotiationSilenceMs() const noexcept {
  return m_audioOutput ? m_audioOutput->lastRenegotiationSilenceMs() : 0.0;
}

void EngineController::followActiveDeckSampleRateIfDirectMode() {
  if (!m_deckManager || !m_audioOutput)
    return;
  const bool directOn = m_directModeEnabled.load(std::memory_order_acquire);
  double targetRateD = m_fixedGraphSampleRate.load(std::memory_order_relaxed);
  if (targetRateD <= 0.0) targetRateD = 48000.0;
  if (directOn) {
    auto *active = m_deckManager->activeDeck();
    if (!active || active->trackInfo().sampleRate <= 0.0) {
      return;
    }
    targetRateD = active->trackInfo().sampleRate;
  }

  const uint32_t targetRate = static_cast<uint32_t>(std::llround(targetRateD));
  const uint32_t currentRequested =
      m_audioOutput->diagnostics().requestedSampleRate;
  if (targetRate > 0 &&
      (targetRate != currentRequested ||
       std::abs(m_deckManager->graphRate() - targetRateD) > 0.5)) {
    double silenceMs = 0.0;
    const bool ok = m_audioControlManager
                        ? m_audioControlManager->renegotiateSampleRate(targetRate, silenceMs)
                        : m_audioOutput->renegotiateSampleRate(targetRate, silenceMs);
    m_deckManager->setGraphRate(targetRateD);
    if (m_pipeline) {
      m_pipeline->setGraphSampleRate(targetRateD);
    }
    m_gainSmootherL.configure(targetRateD, 15.0);
    m_gainSmootherR.configure(targetRateD, 15.0);
    emit graphSampleRateChanged(targetRateD);
    emit diagnosticsChanged();
    log::LogService::instance().log(
        QStringLiteral("AudioEngine"),
        QStringLiteral("DirectModeSampleRateFollow"),
        ok ? QStringLiteral("Success") : QStringLiteral("Warning"),
        QString("Direct Mode rate switch to %1 Hz (silence: %2 ms)")
            .arg(targetRate)
            .arg(silenceMs, 0, 'f', 2),
        QStringLiteral("Backend"), QStringLiteral("PipeWire"),
        QString("{\"target_rate\":%1,\"silence_ms\":%2,\"direct_mode\":%3}")
            .arg(targetRate)
            .arg(silenceMs, 0, 'f', 3)
            .arg(directOn ? QStringLiteral("true") : QStringLiteral("false")));
  }
}

bool EngineController::setFixedGraphSampleRate(double sampleRate) {
  if (sampleRate < 8000.0 || sampleRate > 768000.0)
    return false;
  m_fixedGraphSampleRate.store(sampleRate, std::memory_order_relaxed);
  if (m_directModeEnabled.load(std::memory_order_acquire)) {
    // Store fixed rate preference for when Direct Mode is turned off
    emit diagnosticsChanged();
    return true;
  }
  if (!m_deckManager || !m_audioOutput)
    return false;

  const uint32_t targetRate = static_cast<uint32_t>(std::llround(sampleRate));
  double silenceMs = 0.0;
  const bool ok = m_audioControlManager
                      ? m_audioControlManager->renegotiateSampleRate(targetRate, silenceMs)
                      : m_audioOutput->renegotiateSampleRate(targetRate, silenceMs);
  m_deckManager->setGraphRate(sampleRate);
  if (m_pipeline) {
    m_pipeline->setGraphSampleRate(sampleRate);
  }
  m_gainSmootherL.configure(sampleRate, 15.0);
  m_gainSmootherR.configure(sampleRate, 15.0);
  emit graphSampleRateChanged(sampleRate);
  emit diagnosticsChanged();
  log::LogService::instance().log(
      QStringLiteral("AudioEngine"),
      QStringLiteral("FixedSampleRateChange"),
      ok ? QStringLiteral("Success") : QStringLiteral("Warning"),
      QString("Fixed graph sample rate set to %1 Hz (silence: %2 ms)")
          .arg(targetRate)
          .arg(silenceMs, 0, 'f', 2),
      QStringLiteral("Backend"), QStringLiteral("PipeWire"));
  return ok;
}

QVariantMap EngineController::getAudioCapabilities() const {
  QVariantMap root;
  using engine::backend::CapabilityAvailability;
  auto capStr = [](CapabilityAvailability c) -> QString {
    return QString::fromUtf8(engine::backend::capabilityAvailabilityName(c));
  };

  if (!m_audioOutput || !m_audioControlManager) {
    root["backend"] = QStringLiteral("PipeWire");
    root["directMode"] = capStr(CapabilityAvailability::Unsupported);
    root["sampleRateFollowing"] = capStr(CapabilityAvailability::Unsupported);
    root["exclusiveAccess"] = capStr(CapabilityAvailability::Unsupported);
    root["hardwareBitPerfect"] = capStr(CapabilityAvailability::Unsupported);
    root["deviceSwitching"] = capStr(CapabilityAvailability::Unsupported);
    root["latencyControl"] = capStr(CapabilityAvailability::Unsupported);
    root["formatSelection"] = capStr(CapabilityAvailability::Unsupported);
    root["externalInsert"] = capStr(CapabilityAvailability::Supported);
    root["offlineRender"] = capStr(CapabilityAvailability::Supported);
    return root;
  }

  const auto capReport = m_audioControlManager->queryCapabilities(
      m_audioOutput->isOfflineSimulationMode(), m_audioOutput->isInitialized());
  const auto diag = m_audioControlManager->diagnostics();

  root["backend"] = QString::fromStdString(capReport.backendName);
  root["directMode"] = capStr(capReport.directMode);
  root["sampleRateFollowing"] = capStr(capReport.sampleRateFollowing);
  root["exclusiveAccess"] = capStr(capReport.exclusiveAccess);
  root["hardwareBitPerfect"] = capStr(capReport.hardwareBitPerfect);
  root["deviceSwitching"] = capStr(capReport.deviceSwitching);
  root["latencyControl"] = capStr(capReport.latencyControl);
  root["formatSelection"] = capStr(capReport.formatSelection);
  root["externalInsert"] = capStr(capReport.externalInsert);
  root["offlineRender"] = capStr(capReport.offlineRender);
  root["negotiatedSampleRate"] = static_cast<int>(diag.sampleRate);
  root["requestedSampleRate"] = static_cast<int>(diag.requestedSampleRate);
  root["negotiatedQuantum"] = static_cast<int>(
      diag.negotiatedQuantum > 0 ? diag.negotiatedQuantum : diag.bufferFrames);
  root["formatName"] = QString::fromStdString(diag.formatName);
  root["deviceName"] = QString::fromStdString(diag.deviceName);
  root["targetDeviceId"] = QString::fromStdString(diag.targetDeviceId);
  root["liveDitherEnabled"] = isLiveDitherEnabled();
  return root;
}

QVariantList EngineController::enumerateOutputDevices() const {
  QVariantList list;
  if (!m_audioControlManager)
    return list;
  const auto devices = m_audioControlManager->enumerateOutputDevices();
  for (const auto &d : devices) {
    QVariantMap m;
    m["nodeId"] = static_cast<int>(d.nodeId);
    m["name"] = QString::fromStdString(d.name);
    m["description"] = QString::fromStdString(d.description);
    m["mediaClass"] = QString::fromStdString(d.mediaClass);
    m["isDefault"] = d.isDefault;
    m["sampleRate"] = static_cast<int>(d.sampleRate);
    m["channels"] = static_cast<int>(d.channels);
    m["nativeFormat"] = QString::fromUtf8(
        engine::backend::outputPcmFormatName(d.nativeFormat));
    list.append(m);
  }
  return list;
}

bool EngineController::selectOutputDevice(const QString &deviceIdOrName) {
  if (!m_audioControlManager)
    return false;
  std::string errMsg;
  const bool ok = m_audioControlManager->selectOutputDevice(
      deviceIdOrName.toStdString(), &errMsg);
  if (!ok) {
    log::LogService::instance().log(
        QStringLiteral("AudioEngine"), QStringLiteral("SwitchOutputDevice"),
        QStringLiteral("Failed"), QString::fromStdString(errMsg),
        QStringLiteral("Device"), deviceIdOrName, QStringLiteral("{}"),
        QString(), QString(), log::ErrorCode::AudioDeviceSwitchFailed, 0.0);
  } else {
    log::LogService::instance().log(
        QStringLiteral("AudioEngine"), QStringLiteral("SwitchOutputDevice"),
        QStringLiteral("Success"),
        QString("Selected audio output device: %1").arg(deviceIdOrName),
        QStringLiteral("Device"), deviceIdOrName);
  }
  emit diagnosticsChanged();
  return ok;
}

QString EngineController::currentOutputDevice() const {
  if (!m_audioControlManager)
    return QStringLiteral("default");
  return QString::fromStdString(m_audioControlManager->currentTargetDevice());
}

bool EngineController::setOutputBufferFrames(int bufferFrames) {
  if (!m_audioControlManager || bufferFrames <= 0)
    return false;
  const bool ok = m_audioControlManager->applyLatencySetting(
      static_cast<uint32_t>(bufferFrames), graphSampleRate());
  emit diagnosticsChanged();
  return ok;
}

void EngineController::setOutputPcmFormat(const QString &formatName) {
  if (!m_audioControlManager)
    return;
  using engine::backend::OutputPcmFormat;
  const QString upper = formatName.trimmed().toUpper();
  OutputPcmFormat fmt = OutputPcmFormat::F32LE;
  if (upper == "S24_32LE" || upper == "S24_32") {
    fmt = OutputPcmFormat::S24_32LE;
  } else if (upper == "S16LE" || upper == "S16") {
    fmt = OutputPcmFormat::S16LE;
  } else if (upper == "S32LE" || upper == "S32") {
    fmt = OutputPcmFormat::S32LE;
  }
  m_audioControlManager->setPreferredPcmFormat(fmt);
  emit diagnosticsChanged();
}

QString EngineController::outputPcmFormat() const {
  if (!m_audioOutput)
    return QStringLiteral("F32LE");
  return QString::fromUtf8(engine::backend::outputPcmFormatName(
      m_audioOutput->negotiatedFormat()));
}

void EngineController::setLiveDitherEnabled(bool enabled) {
  if (m_pipeline) {
    m_pipeline->setLiveDitherEnabled(enabled);
  }
  if (m_audioOutput) {
    const bool directBypass =
        m_directModeEnabled.load(std::memory_order_acquire);
    m_audioOutput->setDitherActiveState(enabled && !directBypass);
  }
  emit diagnosticsChanged();
}

bool EngineController::isLiveDitherEnabled() const noexcept {
  return m_pipeline ? m_pipeline->liveDitherEnabled() : true;
}

void EngineController::updateSourceBitDepthFromActiveDeck() {
  if (!m_audioOutput || !m_deckManager)
    return;
  const auto *active = m_deckManager->activeDeck();
  uint32_t bits = 24;
  if (active && active->isOpen()) {
    if (const auto meta = active->metadata()) {
      if (meta->bitDepth > 0) {
        bits = meta->bitDepth;
      }
    }
  }
  m_audioOutput->setSourceBitDepth(bits);
}

void EngineController::setDirectModeEnabled(bool enabled) {
  const bool prev =
      m_directModeEnabled.exchange(enabled, std::memory_order_acq_rel);
  if (prev != enabled) {
    emit diagnosticsChanged();
    log::LogService::instance().log(
        QStringLiteral("AudioEngine"), QStringLiteral("DirectModeToggle"),
        QStringLiteral("Success"),
        enabled
            ? QStringLiteral(
                  "Direct Mode enabled (bypass DSP & follow track sample rate)")
            : QStringLiteral("Direct Mode disabled"),
        QStringLiteral("AudioEngine"), QStringLiteral("DirectMode"));
    followActiveDeckSampleRateIfDirectMode();
  }
}

int EngineController::directModeStatus() const {
  if (!m_audioOutput)
    return 0; // Unknown
  return static_cast<int>(m_audioOutput->directModeStatus());
}

int EngineController::bitPerfectStatus() const {
  // 0 = Unknown / Indeterminate, 1 = NotVerified (processing/resampling/dither/truncation
  // active), 2 = Verified
  const auto *dm = m_deckManager.get();
  const auto *active = dm ? dm->activeDeck() : nullptr;
  const bool hasOpenTrack = active && active->isOpen();

  const bool directBypass = m_directModeEnabled.load(std::memory_order_relaxed);
  const auto diag = m_audioOutput ? m_audioOutput->diagnostics()
                                  : engine::backend::StreamDiagnostics{};
  if (!diag.outputFormatLossless) {
    return 1; // Output format quantization loss (e.g. S16LE on >16-bit source)
  }

  const bool formatUsesDither =
      (diag.negotiatedFormat == engine::backend::OutputPcmFormat::S24_32LE ||
       diag.negotiatedFormat == engine::backend::OutputPcmFormat::S16LE);
  if (!directBypass) {
    if (formatUsesDither && m_pipeline && m_pipeline->liveDitherEnabled()) {
      return 1; // TPDF dither active on integer PCM quantization
    }
    if (m_masterMute.load(std::memory_order_relaxed) ||
        std::abs(m_masterGainL.load(std::memory_order_relaxed) - 1.0) > 1e-6 ||
        std::abs(m_masterGainR.load(std::memory_order_relaxed) - 1.0) > 1e-6 ||
        (active && std::abs(active->levelMatchGainDb()) > 1e-4)) {
      return 1;
    }

    const int routeMode = m_outputRoutingMode.load(std::memory_order_relaxed);
    if ((routeMode == 0 && (listeningMuted() || std::abs(listeningVolume() - 1.0) > 1e-6)) ||
        (routeMode == 1 && (renderMonitorMuted() || std::abs(renderMonitorVolume() - 1.0) > 1e-6))) {
      return 1;
    }

    if (!m_globalDspBypass.load(std::memory_order_relaxed)) {
      const bool filterActive =
          !m_filterBypassed.load(std::memory_order_relaxed) &&
          (m_uiFilter.isHpfEnabled() || m_uiFilter.isLpfEnabled());
      const bool insertActive =
          !m_insertBypassed.load(std::memory_order_relaxed) ||
          (m_insertNode && !m_insertNode->isBypassed());
      if (!m_peqBypassed.load(std::memory_order_relaxed) ||
          !m_geqBypassed.load(std::memory_order_relaxed) || filterActive ||
          !m_compBypassed.load(std::memory_order_relaxed) || insertActive ||
          !m_reverbBypassed.load(std::memory_order_relaxed) ||
          !m_limiterBypassed.load(std::memory_order_relaxed)) {
        return 1;
      }

      if (routeMode == 0 &&
          !m_listeningCorrectionBypassed.load(std::memory_order_relaxed)) {
        for (size_t b = 0; b < ParametricEQ::BANDS_PER_CHANNEL; ++b) {
          if (m_uiListeningCorrection.leftBand(b).enabled ||
              m_uiListeningCorrection.rightBand(b).enabled) {
            return 1;
          }
        }
      }
    }
  }

  if (hasOpenTrack && active->trackInfo().sampleRate > 0.0 &&
      std::abs(active->trackInfo().sampleRate - graphSampleRate()) > 0.5) {
    return 1;
  }

  const int dmStatus = directModeStatus();
  if (dmStatus == 1) {
    return 1;
  }

  const auto telem = m_pipeline ? m_pipeline->currentTelemetry()
                                : engine::pipeline::DspTelemetry{};
  const bool level1Lossless = (telem.processedFrameCount > 0) &&
                              telem.level1SamplesIdentical &&
                              telem.level1OutputPcmLossless;
  if (dmStatus == 2 && (hasOpenTrack || level1Lossless)) {
    return 2;
  }
  if (!directBypass && level1Lossless) {
    return 2; // Level 1 software bit-perfect verified even when Direct Mode is OFF (§3.4)
  }

  return 0;
}

int EngineController::bitPerfectModeState() const {
  // 0 = Unsupported, 1 = Indeterminate, 2 = NotVerified, 3 = Verified, 4 = Direct
  if (!m_audioOutput ||
      m_audioOutput->state() == engine::backend::StreamState::Unconnected ||
      m_audioOutput->state() == engine::backend::StreamState::Error) {
    return 0; // Unsupported
  }
  const int legacy = bitPerfectStatus();
  if (legacy == 1) {
    return 2; // NotVerified
  }
  if (m_pipeline && m_pipeline->currentTelemetry().processedFrameCount > 0 &&
      (!m_pipeline->level1SamplesIdentical() ||
       !m_pipeline->currentTelemetry().level1OutputPcmLossless)) {
    return 2; // NotVerified
  }
  if (legacy == 2) {
    return m_directModeEnabled.load(std::memory_order_relaxed) ? 4 : 3;
  }
  if (m_directModeEnabled.load(std::memory_order_relaxed)) {
    // Direct mode requested, backend status is Unknown (offline/indeterminate)
    return 1; // Indeterminate
  }
  return 1; // Indeterminate
}

QString EngineController::bitPerfectModeStateText() const {
  switch (bitPerfectModeState()) {
  case 0: return QStringLiteral("Unsupported");
  case 1: return QStringLiteral("Indeterminate");
  case 2: return QStringLiteral("NotVerified");
  case 3: return QStringLiteral("Verified");
  case 4: return QStringLiteral("Direct");
  }
  return QStringLiteral("Indeterminate");
}

QVariantMap EngineController::getBitPerfectVerificationReport() const {
  QVariantMap report;
  const int modeState = bitPerfectModeState();
  report["modeState"] = modeState;
  report["modeStateText"] = bitPerfectModeStateText();
  report["legacyStatus"] = bitPerfectStatus();

  const auto telem = m_pipeline ? m_pipeline->currentTelemetry() : engine::pipeline::DspTelemetry{};
  const bool level1Pass =
      telem.processedFrameCount > 0 && telem.level1SamplesIdentical &&
      telem.level1OutputPcmLossless;
  QVariantMap level1;
  level1["internalSamplesIdentical"] = telem.level1SamplesIdentical;
  level1["outputPcmLossless"] = telem.level1OutputPcmLossless;
  level1["ditherActive"] = telem.level1DitherActive;
  level1["directBypassActive"] = telem.level1DirectBypassActive;
  level1["postPcmMaxAbsDiff"] = telem.level1PostPcmMaxAbsDiff;
  level1["maxAbsDiff"] = telem.level1MaxAbsDiff;
  level1["inputHash"] = static_cast<qulonglong>(telem.level1InputHash);
  level1["outputHash"] = static_cast<qulonglong>(telem.level1OutputHash);
  level1["verifiedBlocks"] = static_cast<qulonglong>(m_pipeline ? m_pipeline->level1VerifiedBlocks() : 0);
  level1["status"] = level1Pass
                         ? QStringLiteral("Verified")
                         : (telem.processedFrameCount == 0 ? QStringLiteral("Indeterminate")
                                                           : QStringLiteral("NotVerified"));
  report["level1"] = level1;

  const auto *dm = m_deckManager.get();
  const auto *active = dm ? dm->activeDeck() : nullptr;
  const bool hasTrack = active && active->isOpen();
  const double trackRate = hasTrack ? active->trackInfo().sampleRate : 0.0;
  const double gRate = graphSampleRate();
  const bool noResample = hasTrack && trackRate > 0.0 && std::abs(trackRate - gRate) <= 0.5;
  const auto diag = m_audioOutput ? m_audioOutput->diagnostics() : engine::backend::StreamDiagnostics{};
  const bool backendOk = m_audioOutput && diag.directModeStatus == engine::backend::DirectModeStatus::Active;

  QVariantMap level2;
  level2["backendParamsVerified"] = backendOk;
  level2["graphRateMatchesTrack"] = noResample;
  level2["outputFormatLossless"] = diag.outputFormatLossless;
  level2["outputBitDepth"] = static_cast<int>(diag.outputBitDepth);
  level2["ditherActive"] = diag.ditherActive;
  level2["negotiatedFormat"] = QString::fromUtf8(
      engine::backend::outputPcmFormatName(diag.negotiatedFormat));
  level2["requestedRate"] = static_cast<int>(diag.requestedSampleRate);
  level2["actualRate"] = static_cast<int>(diag.sampleRate);
  level2["formatName"] = QString::fromStdString(diag.formatName);
  level2["noResampling"] = noResample;
  level2["status"] = (backendOk && noResample && diag.outputFormatLossless &&
                      !diag.ditherActive && bitPerfectStatus() == 2)
                         ? QStringLiteral("Verified")
                         : ((diag.directModeStatus == engine::backend::DirectModeStatus::Disabled ||
                             !diag.outputFormatLossless)
                                ? QStringLiteral("NotVerified")
                                : QStringLiteral("Indeterminate"));
  report["level2"] = level2;

  QVariantMap level3;
  level3["status"] = QStringLiteral("NotVerified");
  level3["externalHardwareCaptureRequired"] = true;
  level3["message"] = QStringLiteral("Level 3 verification requires external hardware loopback capture comparison.");
  report["level3"] = level3;

  return report;
}

int EngineController::backendStatus() const {
  if (!m_audioOutput)
    return 0; // Unconnected
  return static_cast<int>(m_audioOutput->state());
}

QString EngineController::backendStatusText() const {
  if (!m_audioOutput)
    return "Unconnected";
  switch (m_audioOutput->state()) {
  case engine::backend::StreamState::Unconnected:
    return "Unconnected";
  case engine::backend::StreamState::Connecting:
    return "Connecting";
  case engine::backend::StreamState::Paused:
    return "Paused";
  case engine::backend::StreamState::Streaming:
    return "Streaming";
  case engine::backend::StreamState::Error:
    return "Error";
  }
  return "Unknown";
}

double EngineController::compressorGainReductionDb() const {
  if (m_globalDspBypass.load(std::memory_order_relaxed))
    return 0.0;
  return m_pipeline ? m_pipeline->compressorGainReductionDb() : 0.0;
}

double EngineController::limiterGainReductionDb() const {
  if (m_globalDspBypass.load(std::memory_order_relaxed))
    return 0.0;
  return m_pipeline ? m_pipeline->limiterGainReductionDb() : 0.0;
}

int EngineController::limiterLatencyFrames() const {
  if (m_globalDspBypass.load(std::memory_order_relaxed))
    return 0;
  return m_pipeline ? m_pipeline->limiterLatencyFrames() : 0;
}

QString EngineController::listeningPresetName() const {
  return m_listeningPresetName;
}

bool EngineController::listeningCorrectionActive() const {
  if (m_outputRoutingMode.load(std::memory_order_relaxed) != 0)
    return false;
  if (m_globalDspBypass.load(std::memory_order_relaxed))
    return false;
  return !m_listeningCorrectionBypassed.load(std::memory_order_relaxed);
}

bool EngineController::isGeqBypassed() const {
  return m_geqBypassed.load(std::memory_order_relaxed);
}

void EngineController::setGeqBypassed(bool bypass) {
  setDspNodeBypass(QStringLiteral("GEQ"), bypass);
}

bool EngineController::isPeqBypassed() const {
  return m_peqBypassed.load(std::memory_order_relaxed);
}

void EngineController::setPeqBypassed(bool bypass) {
  setDspNodeBypass(QStringLiteral("PEQ"), bypass);
}

int EngineController::externalInsertState() const {
  if (!m_insertNode)
    return 0;
  return static_cast<int>(m_insertNode->bridge().state());
}

QString EngineController::externalInsertStateText() const {
  if (!m_insertNode)
    return "Not Configured";
  return QString::fromUtf8(engine::insert::ExternalInsertBridge::stateToString(
      m_insertNode->bridge().state()));
}

bool EngineController::isExternalInsertBypassed() const {
  if (!m_insertNode)
    return true;
  return m_insertNode->isBypassed();
}

int EngineController::externalInsertReportedLatency() const {
  if (!m_insertNode)
    return 0;
  return static_cast<int>(m_insertNode->bridge().reportedLatencyFrames());
}

int EngineController::externalInsertMeasuredLatency() const {
  if (!m_insertNode)
    return -1;
  return static_cast<int>(m_insertNode->bridge().measuredLatencyFrames());
}

double EngineController::externalInsertMeasuredLatencyMs() const {
  if (!m_insertNode)
    return -1.0;
  return m_insertNode->bridge().measuredLatencyMs();
}

int EngineController::insertDelayCompensation() const {
  if (!m_insertNode)
    return 0;
  return static_cast<int>(m_insertNode->delayBuffer().delayFrames());
}

int EngineController::externalInsertXRunCount() const {
  if (!m_insertNode)
    return 0;
  return static_cast<int>(m_insertNode->bridge().xrunCount());
}

QString EngineController::externalInsertLastError() const {
  if (!m_insertNode)
    return QString();
  return QString::fromStdString(m_insertNode->bridge().lastError());
}

QString EngineController::externalInsertClientName() const {
  if (!m_insertNode)
    return QStringLiteral("Carla");
  QString client =
      QString::fromStdString(m_insertNode->bridge().config().clientName);
  return client.isEmpty() ? QStringLiteral("Carla") : client;
}

QString EngineController::externalInsertSendPortL() const {
  if (!m_insertNode)
    return QStringLiteral("carla:audio_in_1");
  QString p = QString::fromStdString(m_insertNode->bridge().config().sendPortL);
  return p.isEmpty() ? QStringLiteral("carla:audio_in_1") : p;
}

QString EngineController::externalInsertSendPortR() const {
  if (!m_insertNode)
    return QStringLiteral("carla:audio_in_2");
  QString p = QString::fromStdString(m_insertNode->bridge().config().sendPortR);
  return p.isEmpty() ? QStringLiteral("carla:audio_in_2") : p;
}

QString EngineController::externalInsertReturnPortL() const {
  if (!m_insertNode)
    return QStringLiteral("carla:audio_out_1");
  QString p =
      QString::fromStdString(m_insertNode->bridge().config().returnPortL);
  return p.isEmpty() ? QStringLiteral("carla:audio_out_1") : p;
}

QString EngineController::externalInsertReturnPortR() const {
  if (!m_insertNode)
    return QStringLiteral("carla:audio_out_2");
  QString p =
      QString::fromStdString(m_insertNode->bridge().config().returnPortR);
  return p.isEmpty() ? QStringLiteral("carla:audio_out_2") : p;
}

bool EngineController::isRenderIdle() const { return m_renderEngine.isIdle(); }

bool EngineController::isRendering() const {
  return m_renderEngine.isRendering();
}

int EngineController::renderWorkerState() const {
  return static_cast<int>(m_renderEngine.workerState());
}

QString EngineController::renderWorkerStateText() const {
  switch (m_renderEngine.workerState()) {
  case engine::render::RenderWorkerState::Idle:
    return QStringLiteral("Idle");
  case engine::render::RenderWorkerState::Starting:
    return QStringLiteral("Starting");
  case engine::render::RenderWorkerState::Rendering:
    return QStringLiteral("Rendering");
  case engine::render::RenderWorkerState::Stopping:
    return QStringLiteral("Stopping");
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
  case engine::render::RenderStatus::Queued:
    return QStringLiteral("Queued");
  case engine::render::RenderStatus::Processing:
    return QStringLiteral("Processing");
  case engine::render::RenderStatus::Completed:
    return QStringLiteral("Completed");
  case engine::render::RenderStatus::Failed:
    return QStringLiteral("Failed");
  case engine::render::RenderStatus::Cancelled:
    return QStringLiteral("Cancelled");
  }
  return QStringLiteral("Idle");
}

QString EngineController::renderTaskId() const {
  return m_renderEngine.currentTaskInfo().taskId;
}

QString EngineController::renderType() const {
  auto info = m_renderEngine.currentTaskInfo();
  if (info.taskId.isEmpty())
    return QStringLiteral("None");
  return (info.type == engine::render::RenderType::Album)
             ? QStringLiteral("Album")
             : QStringLiteral("Track");
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

bool EngineController::renderMonitorMuted() const {
  return m_pipeline ? m_pipeline->renderMonitorMuted() : false;
}

bool EngineController::listeningMuted() const {
  return m_pipeline ? m_pipeline->listeningMuted() : false;
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
    emit diagnosticsChanged();
    QJsonObject det;
    det.insert(QStringLiteral("from"), prev == 1
                                           ? QStringLiteral("RenderMonitor")
                                           : QStringLiteral("Listening"));
    det.insert(QStringLiteral("to"), path == 1 ? QStringLiteral("RenderMonitor")
                                               : QStringLiteral("Listening"));
    det.insert(QStringLiteral("trigger"), QStringLiteral("user"));
    log::LogService::instance().log(
        "Routing", "SetOutputPath", "Success",
        path == 0 ? "Switched to Listening Path"
                  : "Switched to Render Monitor Path",
        "OutputPath", path == 0 ? "Listening" : "RenderMonitor",
        QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)));
  }
}

void EngineController::setGlobalDspBypass(bool bypass) {
  const bool prevBypass = m_globalDspBypass.load(std::memory_order_relaxed);
  if (prevBypass == bypass &&
      !m_globalBypassDesynced.load(std::memory_order_relaxed)) {
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

  // P1-1: Atomically enqueue both Master and Listening commands under a single
  // producer lock. pushTwo() guarantees that either BOTH commands are enqueued
  // or NEITHER is enqueued. Partial bypass state between Master and Listening
  // chains is structurally impossible.
  if (!m_pipeline->paramQueue().pushTwo(cmdMaster, cmdListen)) {
    QJsonObject det;
    det.insert(QStringLiteral("scope"), QStringLiteral("global"));
    det.insert(
        QStringLiteral("target_chains"),
        QJsonArray{QStringLiteral("Master"), QStringLiteral("Listening")});
    det.insert(QStringLiteral("bypassed"), bypass);
    det.insert(QStringLiteral("stage"), QStringLiteral("dsp_queue_push"));
    log::LogService::instance().log(
        "DSP", "GlobalBypass", "Failed",
        QString("Failed to queue Global DSP Bypass: queue full (code: 3001)"),
        "DSPQueue", "Pipeline",
        QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)),
        QString(), QString(), log::ErrorCode::DspQueueFull, 0.0);
    emit dspQueueOverflowWarning(static_cast<int>(cmdMaster.type));
    emit globalDspBypassChanged(prevBypass);
    return;
  }

  m_globalDspBypass.store(bypass, std::memory_order_relaxed);
  m_globalBypassDesynced.store(false, std::memory_order_relaxed);

  // Section 10.2.3 & 5.9: Global DSP Bypass is temporary and does NOT destroy
  // individual DSP node ON/OFF states!
  emit globalDspBypassChanged(bypass);
  emit diagnosticsChanged();
  emit peqParametersChanged();
  emit geqParametersChanged();
  emit filterParametersChanged();
  emit compressorParametersChanged();
  emit limiterParametersChanged();
  emit reverbParametersChanged();
  QJsonObject bypassDet;
  bypassDet.insert(QStringLiteral("scope"), QStringLiteral("global"));
  bypassDet.insert(
      QStringLiteral("target_chains"),
      QJsonArray{QStringLiteral("Master"), QStringLiteral("Listening")});
  bypassDet.insert(QStringLiteral("bypassed"), bypass);
  bypassDet.insert(QStringLiteral("trigger"), QStringLiteral("user"));
  log::LogService::instance().log(
      "DSP", "GlobalBypass", "Success",
      bypass ? "Global DSP Bypass Engaged" : "Global DSP Bypass Disengaged",
      "DSPChain", "Master",
      QString::fromUtf8(
          QJsonDocument(bypassDet).toJson(QJsonDocument::Compact)));
}

bool EngineController::resyncGlobalDspBypass() {
  if (!m_pipeline)
    return false;
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
    log::LogService::instance().log("DSP", "GlobalBypassResync", "Failed",
                                    "Failed to resynchronize Global DSP "
                                    "Bypass: parameter queue full (code: 3005)",
                                    "DSPQueue", "Pipeline");
    return false;
  }

  if (m_pipeline->paramQueue().pushTwo(cmdMaster, cmdListen)) {
    m_globalBypassDesynced.store(false, std::memory_order_relaxed);
    log::LogService::instance().log(
        "DSP", "GlobalBypassResync", "Success",
        "Global DSP Bypass resynchronized successfully", "DSPChain", "Master");
    emit globalDspBypassChanged(target);
    return true;
  }

  return false;
}

void EngineController::setMasterMute(bool mute) {
  bool prev = m_masterMute.exchange(mute);
  if (prev != mute) {
    emit masterMuteChanged(mute);
    emit diagnosticsChanged();
    log::LogService::instance().log("MasterControl", "Mute", "Success",
                                    mute ? "Master Output Muted"
                                         : "Master Output Unmuted",
                                    "Master", mute ? "MuteOn" : "MuteOff");
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
    emit diagnosticsChanged();
  }
}

void EngineController::setMasterGainL(double gain) {
  double clamped = std::clamp(gain, 0.0, 2.0);
  double prev = m_masterGainL.exchange(clamped, std::memory_order_relaxed);
  if (prev != clamped) {
    emit masterGainLChanged(clamped);
    double avg =
        (clamped + m_masterGainR.load(std::memory_order_relaxed)) * 0.5;
    m_masterGain.store(avg, std::memory_order_relaxed);
    emit masterGainChanged(avg);
    emit diagnosticsChanged();
  }
}

void EngineController::setMasterGainR(double gain) {
  double clamped = std::clamp(gain, 0.0, 2.0);
  double prev = m_masterGainR.exchange(clamped, std::memory_order_relaxed);
  if (prev != clamped) {
    emit masterGainRChanged(clamped);
    double avg =
        (m_masterGainL.load(std::memory_order_relaxed) + clamped) * 0.5;
    m_masterGain.store(avg, std::memory_order_relaxed);
    emit masterGainChanged(avg);
    emit diagnosticsChanged();
  }
}

void EngineController::play(const QString &deckId) {
  if (!m_deckManager || deckId.isEmpty())
    return;
  char id = deckId.toUpper().at(0).toLatin1();
  auto *d = m_deckManager->deck(id);
  if (d) {
    if (m_deckManager->activeDeckId() != id) {
      switchDeck(deckId, m_switchingMode);
    }
    followActiveDeckSampleRateIfDirectMode();
    d->play();
    emit deckStateChanged(deckId);
    const auto info = d->trackInfo();
    const double sr = info.sampleRate;
    const double posMs =
        (sr > 0.0) ? (static_cast<double>(d->playhead()) * 1000.0 / sr) : 0.0;
    const double durMs =
        (sr > 0.0) ? (static_cast<double>(info.totalFrames) * 1000.0 / sr)
                   : 0.0;
    QJsonObject det;
    det.insert(QStringLiteral("deck"), deckId);
    det.insert(QStringLiteral("trigger"), QStringLiteral("user"));
    det.insert(QStringLiteral("path"), QString::fromStdString(info.path));
    det.insert(QStringLiteral("position_ms"), posMs);
    det.insert(QStringLiteral("duration_ms_track"), durMs);
    log::LogService::instance().log(
        "Transport", "Play", "Success", "Deck " + deckId + " started playback",
        "Deck", deckId,
        QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)));
  }
}

void EngineController::pause(const QString &deckId) {
  if (!m_deckManager || deckId.isEmpty())
    return;
  char id = deckId.toUpper().at(0).toLatin1();
  auto *d = m_deckManager->deck(id);
  if (d) {
    d->pause();
    emit deckStateChanged(deckId);
    const auto info = d->trackInfo();
    const double sr = info.sampleRate;
    const double posMs =
        (sr > 0.0) ? (static_cast<double>(d->playhead()) * 1000.0 / sr) : 0.0;
    const double durMs =
        (sr > 0.0) ? (static_cast<double>(info.totalFrames) * 1000.0 / sr)
                   : 0.0;
    QJsonObject det;
    det.insert(QStringLiteral("deck"), deckId);
    det.insert(QStringLiteral("trigger"), QStringLiteral("user"));
    det.insert(QStringLiteral("path"), QString::fromStdString(info.path));
    det.insert(QStringLiteral("position_ms"), posMs);
    det.insert(QStringLiteral("duration_ms_track"), durMs);
    log::LogService::instance().log(
        "Transport", "Pause", "Success", "Deck " + deckId + " paused", "Deck",
        deckId,
        QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)));
  }
}

void EngineController::stop(const QString &deckId) {
  if (!m_deckManager || deckId.isEmpty())
    return;
  char id = deckId.toUpper().at(0).toLatin1();
  auto *d = m_deckManager->deck(id);
  if (d) {
    const auto info = d->trackInfo();
    const double sr = info.sampleRate;
    const double posMs =
        (sr > 0.0) ? (static_cast<double>(d->playhead()) * 1000.0 / sr) : 0.0;
    const double durMs =
        (sr > 0.0) ? (static_cast<double>(info.totalFrames) * 1000.0 / sr)
                   : 0.0;
    d->stop();
    emit deckStateChanged(deckId);
    QJsonObject det;
    det.insert(QStringLiteral("deck"), deckId);
    det.insert(QStringLiteral("trigger"), QStringLiteral("user"));
    det.insert(QStringLiteral("path"), QString::fromStdString(info.path));
    det.insert(QStringLiteral("position_ms"), posMs);
    det.insert(QStringLiteral("duration_ms_track"), durMs);
    log::LogService::instance().log(
        "Transport", "Stop", "Success", "Deck " + deckId + " stopped", "Deck",
        deckId,
        QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)));
  }
}

void EngineController::recordSeekEvent(char deckChar, int64_t beforeFrame,
                                       const engine::decoder::SeekResult &res,
                                       const QString &trigger,
                                       double durationMs,
                                       const std::string &trackPath) {
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
  details.insert(QStringLiteral("requested_frame"),
                 static_cast<qint64>(res.requestedFrame));
  details.insert(QStringLiteral("actual_frame"),
                 static_cast<qint64>(res.actualFrame));
  details.insert(QStringLiteral("within_buffer"), res.withinBuffer);

  if (!res.success) {
    details.insert(QStringLiteral("stage"), QString::fromStdString(res.stage));
    if (!res.errorMessage.empty()) {
      details.insert(QStringLiteral("error_detail"),
                     QString::fromStdString(res.errorMessage));
    }
  }

  const QString detailsJson =
      QString::fromUtf8(QJsonDocument(details).toJson(QJsonDocument::Compact));
  const QString status =
      res.success ? QStringLiteral("Success") : QStringLiteral("Failed");
  const QString msg =
      res.success
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
      QStringLiteral("Transport"), QStringLiteral("Seek"), status, msg,
      QStringLiteral("Deck"), deckStr, detailsJson, QString(), QString(),
      res.success ? 0 : res.errorCode, durationMs);
}

void EngineController::seek(const QString &deckId, qint64 targetFrame) {
  seekWithTrigger(deckId, targetFrame, QStringLiteral("user"));
}

void EngineController::seekWithTrigger(const QString &deckId,
                                       qint64 targetFrame,
                                       const QString &trigger) {
  if (!m_deckManager || deckId.isEmpty())
    return;
  char id = deckId.toUpper().at(0).toLatin1();
  auto *d = m_deckManager->deck(id);
  if (d) {
    const int64_t beforeFrame = d->playhead();
    const std::string trackPath = d->trackInfo().path;
    const auto start = std::chrono::steady_clock::now();
    const auto res = d->seekWithResult(targetFrame);
    const double durationMs = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - start)
                                  .count();

    recordSeekEvent(id, beforeFrame, res, trigger, durationMs, trackPath);
    emit deckStateChanged(deckId);
  }
}

void EngineController::switchDeck(const QString &targetDeckId, int mode) {
  if (!m_deckManager || targetDeckId.isEmpty())
    return;
  char id = targetDeckId.toUpper().at(0).toLatin1();
  auto smode = (mode == 1) ? engine::deck::SwitchingMode::Comparative
                           : engine::deck::SwitchingMode::Normal;
  auto res = m_deckManager->switchDeck(id, smode);
  if (res.success) {
    followActiveDeckSampleRateIfDirectMode();
    updateSourceBitDepthFromActiveDeck();
    emit activeDeckChanged(targetDeckId);
    emit deckStateChanged("A");
    emit deckStateChanged("B");
    emit deckStateChanged("C");
    emit deckStateChanged("D");
    emit diagnosticsChanged();
    log::LogService::instance().log(
        "Deck", "SwitchDeck", "Success",
        QString("Switched active deck to %1 (%2 mode, mute time: %3 ms)")
            .arg(targetDeckId)
            .arg(mode == 1 ? "Comparative" : "Normal")
            .arg(res.muteTimeMs, 0, 'f', 2),
        "Deck", targetDeckId);
    if (res.hadSeek) {
      const auto *tgtDeck = m_deckManager->deck(res.targetDeck);
      const std::string tgtPath =
          tgtDeck ? tgtDeck->trackInfo().path : std::string();
      recordSeekEvent(res.targetDeck, res.seekBeforeFrame, res.seekResult,
                      QStringLiteral("compare_switch"), res.seekDurationMs,
                      tgtPath);
    }
  }
}

void EngineController::quickSwitchAB() {
  if (!m_deckManager)
    return;
  auto smode = (m_switchingMode == 1) ? engine::deck::SwitchingMode::Comparative
                                      : engine::deck::SwitchingMode::Normal;
  auto res = m_deckManager->quickSwitchAB(smode);
  if (res.success) {
    followActiveDeckSampleRateIfDirectMode();
    updateSourceBitDepthFromActiveDeck();
    QString act(res.targetDeck);
    emit activeDeckChanged(act);
    emit deckStateChanged("A");
    emit deckStateChanged("B");
    emit diagnosticsChanged();
    if (res.hadSeek) {
      const auto *tgtDeck = m_deckManager->deck(res.targetDeck);
      const std::string tgtPath =
          tgtDeck ? tgtDeck->trackInfo().path : std::string();
      recordSeekEvent(res.targetDeck, res.seekBeforeFrame, res.seekResult,
                      QStringLiteral("compare_switch"), res.seekDurationMs,
                      tgtPath);
    }
  }
}

void EngineController::setDeckLocked(const QString &deckId, bool locked) {
  if (!m_deckManager || deckId.isEmpty())
    return;
  char id = deckId.toUpper().at(0).toLatin1();
  auto *d = m_deckManager->deck(id);
  if (d) {
    d->setLocked(locked);
    emit deckStateChanged(deckId);
    log::LogService::instance().log("Deck", "DeckLock", "Success",
                                    QString("Deck %1 lock %2")
                                        .arg(deckId)
                                        .arg(locked ? "Engaged" : "Disengaged"),
                                    "Deck", deckId);
  }
}

bool EngineController::loadWorkspaceTrack(const QString &deckId,
                                          const QString &relativePath) {
  auto &wsMgr = engine::workspace::WorkspaceManager::instance();
  if (!wsMgr.hasOpenWorkspace() || relativePath.trimmed().isEmpty()) {
    return false;
  }
  const QString candidate =
      QDir(wsMgr.openWorkspacePath()).filePath(relativePath);
  if (!wsMgr.isPathInsideOpenWorkspace(candidate, false)) {
    return false;
  }
  return loadTrack(deckId, candidate);
}

bool EngineController::copyDeckTrackToWorkspace(const QString &deckId) {
  if (!m_deckManager || deckId.isEmpty())
    return false;
  auto &wsMgr = engine::workspace::WorkspaceManager::instance();
  if (!wsMgr.hasOpenWorkspace())
    return false;

  char id = deckId.toUpper().at(0).toLatin1();
  auto *d = m_deckManager->deck(id);
  if (!d || d->trackInfo().path.empty())
    return false;

  const QString srcPath = QString::fromStdString(d->trackInfo().path);
  if (wsMgr.isPathInsideOpenWorkspace(srcPath, false)) {
    log::LogService::instance().log(
        "Workspace", "CopyDeckTrack", "Info",
        QString("Skipped self-copy: Deck %1 track is already inside the open workspace (%2)")
            .arg(deckId, srcPath),
        "Deck", deckId);
    return false;
  }
  const auto res = wsMgr.importFile(srcPath);
  return res.status == engine::workspace::ImportResultStatus::Imported ||
         res.status == engine::workspace::ImportResultStatus::SkippedIdentical;
}

void EngineController::setDeckOffsetFrames(const QString &deckId,
                                           qint64 offsetFrames) {
  if (!m_deckManager || deckId.isEmpty())
    return;
  char id = deckId.toUpper().at(0).toLatin1();
  auto *d = m_deckManager->deck(id);
  if (d) {
    d->setOffsetFrames(static_cast<int64_t>(offsetFrames));
    emit deckStateChanged(deckId);
    emit diagnosticsChanged();
  }
}

qint64 EngineController::getDeckOffsetFrames(const QString &deckId) const {
  if (!m_deckManager || deckId.isEmpty())
    return 0;
  char id = deckId.toUpper().at(0).toLatin1();
  const auto *d = m_deckManager->deck(id);
  return d ? static_cast<qint64>(d->offsetFrames()) : 0;
}

void EngineController::setDeckLevelMatchDb(const QString &deckId,
                                           double gainDb) {
  if (!m_deckManager || deckId.isEmpty())
    return;
  char id = deckId.toUpper().at(0).toLatin1();
  auto *d = m_deckManager->deck(id);
  if (d) {
    d->setLevelMatchGainDb(gainDb);
    emit deckStateChanged(deckId);
    emit diagnosticsChanged();
  }
}

double EngineController::getDeckLevelMatchDb(const QString &deckId) const {
  if (!m_deckManager || deckId.isEmpty())
    return 0.0;
  char id = deckId.toUpper().at(0).toLatin1();
  const auto *d = m_deckManager->deck(id);
  return d ? d->levelMatchGainDb() : 0.0;
}

double EngineController::getTrackMeasuredRmsDb(const QString &deckId) const {
  if (!m_deckManager || deckId.isEmpty())
    return -96.0;
  char id = deckId.toUpper().at(0).toLatin1();
  const auto *d = m_deckManager->deck(id);
  if (!d || !d->isOpen() || d->trackInfo().path.empty())
    return -96.0;
  double rmsDb = -96.0;
  engine::deck::ReferenceDeck::measureTrackRmsDb(d->trackInfo().path, 0.0, 10.0, rmsDb);
  return rmsDb;
}

double EngineController::measureAndApplyDeckLevelMatch(
    const QString &referenceDeckId, const QString &targetDeckId) {
  if (!m_deckManager || referenceDeckId.isEmpty() || targetDeckId.isEmpty())
    return 0.0;
  auto *refDeck = m_deckManager->deck(referenceDeckId.toUpper().at(0).toLatin1());
  auto *tgtDeck = m_deckManager->deck(targetDeckId.toUpper().at(0).toLatin1());
  if (!refDeck || !tgtDeck || !refDeck->isOpen() || !tgtDeck->isOpen())
    return 0.0;

  double refRmsDb = -96.0;
  double tgtRmsDb = -96.0;
  if (!engine::deck::ReferenceDeck::measureTrackRmsDb(refDeck->trackInfo().path, 0.0, 10.0, refRmsDb) ||
      !engine::deck::ReferenceDeck::measureTrackRmsDb(tgtDeck->trackInfo().path, 0.0, 10.0, tgtRmsDb))
    return 0.0;

  const double deltaDb = std::clamp(refRmsDb - tgtRmsDb, -24.0, 24.0);
  setDeckLevelMatchDb(targetDeckId, deltaDb);
  log::LogService::instance().log(
      QStringLiteral("Deck"), QStringLiteral("AutoLevelMatch"),
      QStringLiteral("Success"),
      QString("Measured level match %1 -> %2: %3 dB (Ref: %4 dBFS, Target: %5 dBFS)")
          .arg(referenceDeckId, targetDeckId)
          .arg(deltaDb, 0, 'f', 2)
          .arg(refRmsDb, 0, 'f', 2)
          .arg(tgtRmsDb, 0, 'f', 2),
      QStringLiteral("Deck"), targetDeckId);
  return deltaDb;
}

bool EngineController::loadTrack(const QString &deckId,
                                 const QString &filePath) {
  if (!m_deckManager || deckId.isEmpty())
    return false;
  char id = deckId.toUpper().at(0).toLatin1();
  auto *d = m_deckManager->deck(id);
  if (!d)
    return false;

  if (engine::workspace::WorkspaceManager::instance()
          .isPathBlockedByLockedWorkspace(filePath)) {
    log::LogService::instance().log(
        "Deck", "LoadTrack", "Failed",
        QString("Blocked access to locked workspace file: %1").arg(filePath),
        "File", filePath,
        QString("{\"deck\":\"%1\",\"stage\":\"workspace_lock_check\"}")
            .arg(deckId),
        QString(), QString(), log::ErrorCode::WorkspaceLockedAccessDenied, 0.0);
    return false;
  }

  const auto start = std::chrono::steady_clock::now();
  bool ok = d->loadTrack(filePath.toStdString(), m_deckManager->graphRate());
  const double loadMs = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - start)
                            .count();

  if (ok) {
    if (m_deckManager->activeDeckId() == id) {
      followActiveDeckSampleRateIfDirectMode();
      updateSourceBitDepthFromActiveDeck();
    }
    emit deckStateChanged(deckId);
    emit diagnosticsChanged();
    const auto info = d->trackInfo();
    const double sr = info.sampleRate;
    const double durMs =
        (sr > 0.0) ? (static_cast<double>(info.totalFrames) * 1000.0 / sr)
                   : 0.0;
    QJsonObject det;
    det.insert(QStringLiteral("deck"), deckId);
    det.insert(QStringLiteral("trigger"), QStringLiteral("user"));
    det.insert(QStringLiteral("path"), filePath);
    det.insert(QStringLiteral("codec"), QFileInfo(filePath).suffix().toLower());
    det.insert(QStringLiteral("sample_rate_hz"), sr);
    det.insert(QStringLiteral("channels"), static_cast<int>(info.channels));
    det.insert(QStringLiteral("duration_ms_track"), durMs);
    log::LogService::instance().log(
        "Deck", "LoadTrack", "Success",
        QString("Loaded %1 into Deck %2")
            .arg(QFileInfo(filePath).fileName(), deckId),
        "File", filePath,
        QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)),
        QString(), QString(), 0, loadMs);
  } else {
    const bool locked = d->isLocked();
    const int32_t errCode = locked ? log::ErrorCode::TrackLockedOrCorrupt
                                   : log::ErrorCode::TrackOpenFailed;
    QJsonObject det;
    det.insert(QStringLiteral("deck"), deckId);
    det.insert(QStringLiteral("trigger"), QStringLiteral("user"));
    det.insert(QStringLiteral("path"), filePath);
    det.insert(QStringLiteral("stage"), locked
                                            ? QStringLiteral("deck_lock_check")
                                            : QStringLiteral("open"));
    det.insert(QStringLiteral("deck_locked"), locked);
    det.insert(QStringLiteral("file_exists"), QFileInfo::exists(filePath));
    det.insert(QStringLiteral("retried"), false);
    log::LogService::instance().log(
        "Deck", "LoadTrack", "Failed",
        QString("Failed to load %1 into Deck %2 (%3)")
            .arg(QFileInfo(filePath).fileName(), deckId,
                 locked ? QStringLiteral("deck locked")
                        : QStringLiteral("open failed")),
        "File", filePath,
        QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)),
        QString(), QString(), errCode, loadMs);
  }
  return ok;
}

bool EngineController::startFolderSession(const QString &deckId,
                                          const QString &folderPath,
                                          const QStringList &fileNames) {
  if (!m_deckManager || deckId.isEmpty())
    return false;
  char id = deckId.toUpper().at(0).toLatin1();
  auto *d = m_deckManager->deck(id);
  if (!d)
    return false;

  QFileInfo folderInfo(folderPath);
  if (folderInfo.isSymLink() ||
      folderInfo.fileName().startsWith(QStringLiteral(".desk32"))) {
    return false;
  }
  if (engine::workspace::WorkspaceManager::instance()
          .isPathBlockedByLockedWorkspace(folderPath)) {
    log::LogService::instance().log(
        "FolderSession", "StartSession", "Failed",
        QString("Blocked folder session in locked workspace: %1")
            .arg(folderPath),
        "Folder", folderPath,
        QString("{\"deck\":\"%1\",\"stage\":\"workspace_lock_check\"}")
            .arg(deckId),
        QString(), QString(), log::ErrorCode::WorkspaceLockedAccessDenied, 0.0);
    return false;
  }

  std::vector<std::string> files;
  QDir dir(folderPath);
  if (fileNames.isEmpty()) {
    QStringList nameFilters;
    nameFilters << "*.flac" << "*.wav" << "*.mp3" << "*.ogg" << "*.opus"
                << "*.aac" << "*.m4a" << "*.aif" << "*.aiff"
                << "*.FLAC" << "*.WAV" << "*.MP3" << "*.OGG" << "*.OPUS"
                << "*.AAC" << "*.M4A" << "*.AIF" << "*.AIFF";
    QFileInfoList entries =
        dir.entryInfoList(nameFilters, QDir::Files | QDir::NoSymLinks);
    for (const auto &fi : entries) {
      if (fi.isSymLink() ||
          fi.fileName().startsWith(QStringLiteral(".desk32"))) {
        continue;
      }
      files.push_back(fi.fileName().toStdString());
    }
  } else {
    for (const auto &fn : fileNames) {
      if (fn.startsWith(QStringLiteral(".desk32")) ||
          QFileInfo(dir.filePath(fn)).isSymLink()) {
        continue;
      }
      files.push_back(fn.toStdString());
    }
  }

  bool ok = d->startFolderSession(folderPath.toStdString(), files,
                                  m_deckManager->graphRate());
  if (ok) {
    if (m_deckManager->activeDeckId() == id) {
      followActiveDeckSampleRateIfDirectMode();
      updateSourceBitDepthFromActiveDeck();
    }
    emit deckStateChanged(deckId);
    emit diagnosticsChanged();
    const auto &fs = d->folderSession();
    const QString firstPath =
        fs.currentTrack() ? QString::fromStdString(fs.currentTrack()->path)
                          : QString();
    QJsonObject det;
    det.insert(QStringLiteral("deck"), deckId);
    det.insert(QStringLiteral("trigger"), QStringLiteral("user"));
    det.insert(QStringLiteral("folder_path"), folderPath);
    det.insert(QStringLiteral("track_count"),
               static_cast<int>(fs.tracks().size()));
    det.insert(QStringLiteral("first_track_path"), firstPath);
    log::LogService::instance().log(
        "FolderSession", "StartSession", "Success",
        QString("Started folder session in Deck %1 with %2 tracks")
            .arg(deckId)
            .arg(fs.tracks().size()),
        "Folder", folderPath,
        QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)));
  } else {
    QJsonObject det;
    det.insert(QStringLiteral("deck"), deckId);
    det.insert(QStringLiteral("trigger"), QStringLiteral("user"));
    det.insert(QStringLiteral("folder_path"), folderPath);
    det.insert(QStringLiteral("stage"), QStringLiteral("open"));
    det.insert(QStringLiteral("candidate_count"),
               static_cast<int>(files.size()));
    log::LogService::instance().log(
        "FolderSession", "StartSession", "Failed",
        QString("Failed to start folder session in Deck %1 for %2")
            .arg(deckId, folderPath),
        "Folder", folderPath,
        QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)),
        QString(), QString(), log::ErrorCode::TrackOpenFailed, 0.0);
  }
  return ok;
}

bool EngineController::startFolderSessionFromDeck(const QString &deckId) {
  if (!m_deckManager || deckId.isEmpty())
    return false;
  char id = deckId.toUpper().at(0).toLatin1();
  auto *d = m_deckManager->deck(id);
  if (!d)
    return false;

  const QString folderPath =
      QFileInfo(QString::fromStdString(d->trackInfo().path)).absolutePath();
  if (folderPath.isEmpty() ||
      engine::workspace::WorkspaceManager::instance()
          .isPathBlockedByLockedWorkspace(folderPath)) {
    return false;
  }

  bool ok = d->startFolderSessionFromCurrentTrack(m_deckManager->graphRate());
  if (ok) {
    if (m_deckManager->activeDeckId() == id) {
      followActiveDeckSampleRateIfDirectMode();
      updateSourceBitDepthFromActiveDeck();
    }
    emit deckStateChanged(deckId);
    emit diagnosticsChanged();
    log::LogService::instance().log(
        "FolderSession", "StartFromDeck", "Success",
        QString("Started folder session from loaded track in Deck %1 (%2 tracks)")
            .arg(deckId)
            .arg(d->folderSession().tracks().size()),
        "Folder", folderPath);
  }
  return ok;
}

bool EngineController::advanceFolderNext(const QString &deckId) {
  if (!m_deckManager || deckId.isEmpty())
    return false;
  char id = deckId.toUpper().at(0).toLatin1();
  auto *d = m_deckManager->deck(id);
  if (!d)
    return false;

  const auto &fsBefore = d->folderSession();
  const int idxFrom = static_cast<int>(fsBefore.currentIndex());
  const std::string pathFrom =
      fsBefore.currentTrack() ? fsBefore.currentTrack()->path : std::string();

  bool ok = d->advanceFolderNext();
  const auto &fsAfter = d->folderSession();
  const int idxTo = static_cast<int>(fsAfter.currentIndex());
  const std::string pathTo =
      fsAfter.currentTrack() ? fsAfter.currentTrack()->path : std::string();

  QJsonObject det;
  det.insert(QStringLiteral("deck"), deckId);
  det.insert(QStringLiteral("trigger"), QStringLiteral("user"));
  det.insert(QStringLiteral("track_index_from"), idxFrom);
  det.insert(QStringLiteral("track_index_to"), idxTo);
  det.insert(QStringLiteral("path_from"), QString::fromStdString(pathFrom));
  det.insert(QStringLiteral("path_to"), QString::fromStdString(pathTo));
  log::LogService::instance().log(
      "FolderSession", "AdvanceNext", ok ? "Success" : "Info",
      ok ? QString("Manual folder next in Deck %1 [%2 -> %3]")
               .arg(deckId)
               .arg(idxFrom)
               .arg(idxTo)
         : QString("Folder next reached end in Deck %1").arg(deckId),
      "Deck", deckId,
      QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)));

  emit deckStateChanged(deckId);
  return ok;
}

bool EngineController::retreatFolderPrevious(const QString &deckId) {
  if (!m_deckManager || deckId.isEmpty())
    return false;
  char id = deckId.toUpper().at(0).toLatin1();
  auto *d = m_deckManager->deck(id);
  if (!d)
    return false;

  const auto &fsBefore = d->folderSession();
  const int idxFrom = static_cast<int>(fsBefore.currentIndex());
  const std::string pathFrom =
      fsBefore.currentTrack() ? fsBefore.currentTrack()->path : std::string();

  bool ok = d->retreatFolderPrevious();
  const auto &fsAfter = d->folderSession();
  const int idxTo = static_cast<int>(fsAfter.currentIndex());
  const std::string pathTo =
      fsAfter.currentTrack() ? fsAfter.currentTrack()->path : std::string();

  QJsonObject det;
  det.insert(QStringLiteral("deck"), deckId);
  det.insert(QStringLiteral("trigger"), QStringLiteral("user"));
  det.insert(QStringLiteral("track_index_from"), idxFrom);
  det.insert(QStringLiteral("track_index_to"), idxTo);
  det.insert(QStringLiteral("path_from"), QString::fromStdString(pathFrom));
  det.insert(QStringLiteral("path_to"), QString::fromStdString(pathTo));
  log::LogService::instance().log(
      "FolderSession", "RetreatPrevious", ok ? "Success" : "Info",
      ok ? QString("Manual folder previous in Deck %1 [%2 -> %3]")
               .arg(deckId)
               .arg(idxFrom)
               .arg(idxTo)
         : QString("Folder previous at start in Deck %1").arg(deckId),
      "Deck", deckId,
      QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)));

  emit deckStateChanged(deckId);
  return ok;
}

void EngineController::setGeqBandGain(int bandIndex, double gainDb) {
  if (m_pipeline) {
    // P0-21: Continuous slider parameter update exclusively via lock-free
    // DspParameterMailbox
    m_pipeline->paramMailbox().setGeqBandGain(static_cast<size_t>(bandIndex),
                                              static_cast<float>(gainDb));
  }
  // Update Control Thread local UI Snapshot
  m_uiGeq.setBandGain(static_cast<size_t>(bandIndex), gainDb);
  emit geqParametersChanged();
}

void EngineController::setGeqBandEnabled(int bandIndex, bool enabled) {
  if (bandIndex < 0 || bandIndex >= 32)
    return;
  if (m_pipeline) {
    m_pipeline->paramMailbox().setGeqBandEnabled(static_cast<size_t>(bandIndex),
                                                 enabled);
  }
  m_uiGeq.setBandEnabled(static_cast<size_t>(bandIndex), enabled);
  emit geqParametersChanged();
}

bool EngineController::isGeqBandEnabled(int bandIndex) const {
  return (bandIndex >= 0 && bandIndex < 32)
             ? m_uiGeq.isBandEnabled(static_cast<size_t>(bandIndex))
             : false;
}

double EngineController::getGeqBandGain(int bandIndex) {
  return (bandIndex >= 0 && bandIndex < 32)
             ? m_uiGeq.bandGain(static_cast<size_t>(bandIndex))
             : 0.0;
}

void EngineController::resetAllGeqBands() {
  if (m_pipeline) {
    // P0-21: Atomic batch reset exclusively via DspParameterMailbox
    m_pipeline->paramMailbox().resetAllGeqBands();
  }
  m_uiGeq.resetAllBands();
  emit geqParametersChanged();
  log::LogService::instance().log("DSP", "ResetGEQ", "Success",
                                  "Reset all 32 GEQ bands to 0 dB", "DSPNode",
                                  "GEQ");
}

QVariantList EngineController::getGeqCurve(int numPoints) {
  QVariantList list;
  if (numPoints < 2)
    return list;

  std::vector<double> freqs;
  freqs.reserve(static_cast<size_t>(numPoints) + GraphicEQ::NUM_BANDS);

  for (int i = 0; i < numPoints; ++i) {
    // Logarithmically spaced frequencies between 16.0 Hz and 20000.0 Hz
    double f = 16.0 * std::pow(20000.0 / 16.0,
                               static_cast<double>(i) / (numPoints - 1));
    freqs.push_back(f);
  }
  // Explicitly add exact center frequencies of all bands so each peak is
  // perfectly represented
  for (size_t b = 0; b < GraphicEQ::NUM_BANDS; ++b) {
    freqs.push_back(m_uiGeq.bandFrequency(b));
  }
  std::sort(freqs.begin(), freqs.end());
  freqs.erase(
      std::unique(freqs.begin(), freqs.end(),
                  [](double a, double b) { return std::abs(a - b) < 1e-4; }),
      freqs.end());

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
  if (bandIndex < 0 ||
      bandIndex >= static_cast<int>(ParametricEQ::BANDS_PER_CHANNEL))
    return map;

  const auto &band = isRightChannel
                         ? m_uiPeq.rightBand(static_cast<size_t>(bandIndex))
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

void EngineController::setPeqBand(bool isRightChannel, int bandIndex,
                                  double frequency, double gainDb, double q,
                                  int filterType, bool enabled) {
  if (bandIndex < 0 ||
      bandIndex >= static_cast<int>(ParametricEQ::BANDS_PER_CHANNEL))
    return;

  if (m_pipeline) {
    // P0-21: Continuous parameter update exclusively via lock-free
    // DspParameterMailbox
    m_pipeline->paramMailbox().setPeqBand(
        static_cast<size_t>(bandIndex), isRightChannel ? 1 : 0,
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

void EngineController::setPeqBandEnabled(bool isRightChannel, int bandIndex,
                                         bool enabled) {
  if (bandIndex < 0 ||
      bandIndex >= static_cast<int>(ParametricEQ::BANDS_PER_CHANNEL))
    return;

  if (m_pipeline) {
    // P0-21: Update band enable status exclusively via DspParameterMailbox
    m_pipeline->paramMailbox().setPeqBandEnabled(
        static_cast<size_t>(bandIndex), isRightChannel ? 1 : 0, enabled);
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
  if (numPoints < 2)
    return list;

  std::vector<double> freqs(static_cast<size_t>(numPoints));
  for (int i = 0; i < numPoints; ++i) {
    freqs[static_cast<size_t>(i)] =
        20.0 *
        std::pow(20000.0 / 20.0, static_cast<double>(i) / (numPoints - 1));
  }

  std::vector<double> leftDb, rightDb;
  m_uiPeq.calculateCompositeResponse(freqs, leftDb, rightDb);

  const auto &target = isRightChannel ? rightDb : leftDb;
  for (int i = 0; i < numPoints; ++i) {
    QVariantMap pt;
    pt["f"] = freqs[static_cast<size_t>(i)];
    pt["db"] = target[static_cast<size_t>(i)];
    list.append(pt);
  }
  return list;
}

void EngineController::applyPeqPreset(const QString &presetName) {
  bool ok = m_uiPeq.applyPreset(presetName.toStdString());
  if (ok) {
    if (m_pipeline) {
      // P0-21: Publish entire 16-band-per-channel PEQ configuration atomically
      // via Mailbox
      std::array<engine::dsp::ContinuousDspParameters::PeqBandConfig, 24>
          leftBands{};
      std::array<engine::dsp::ContinuousDspParameters::PeqBandConfig, 24>
          rightBands{};
      for (size_t b = 0; b < ParametricEQ::BANDS_PER_CHANNEL; ++b) {
        const auto &l = m_uiPeq.leftBand(b);
        leftBands[b].frequency = static_cast<float>(l.frequency);
        leftBands[b].gainDb = static_cast<float>(l.gainDb);
        leftBands[b].Q = static_cast<float>(l.Q);
        leftBands[b].type = static_cast<uint8_t>(l.type);
        leftBands[b].enabled = l.enabled ? 1 : 0;
        leftBands[b].modified = 1;

        const auto &r = m_uiPeq.rightBand(b);
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
    log::LogService::instance().log("DSP", "ApplyPEQPreset", "Success",
                                    "Applied PEQ Preset: " + presetName,
                                    "DSPNode", "PEQ");
  }
}

void EngineController::setFilterParams(double hpfFreq, bool hpfOn, int hpfSlope,
                                       double lpfFreq, bool lpfOn, int lpfSlope,
                                       double hpfQ, double lpfQ) {
  const double clampedHpfQ = std::clamp(hpfQ, 0.1, 10.0);
  const double clampedLpfQ = std::clamp(lpfQ, 0.1, 10.0);
  if (m_pipeline) {
    // P0-21: Continuous parameter update exclusively via lock-free
    // DspParameterMailbox
    m_pipeline->paramMailbox().setFilterParams(
        static_cast<float>(hpfFreq), static_cast<float>(lpfFreq),
        static_cast<uint8_t>(hpfSlope), static_cast<uint8_t>(lpfSlope), hpfOn,
        lpfOn, static_cast<float>(clampedHpfQ),
        static_cast<float>(clampedLpfQ));
  }
  m_uiFilter.setHpfFrequency(hpfFreq);
  m_uiFilter.setHpfEnabled(hpfOn);
  m_uiFilter.setHpfSlope(hpfSlope == 1
                             ? HighLowPassFilter::FilterSlope::Slope24dB
                             : HighLowPassFilter::FilterSlope::Slope12dB);
  m_uiFilter.setHpfQ(clampedHpfQ);

  m_uiFilter.setLpfFrequency(lpfFreq);
  m_uiFilter.setLpfEnabled(lpfOn);
  m_uiFilter.setLpfSlope(lpfSlope == 1
                             ? HighLowPassFilter::FilterSlope::Slope24dB
                             : HighLowPassFilter::FilterSlope::Slope12dB);
  m_uiFilter.setLpfQ(clampedLpfQ);

  emit filterParametersChanged();
  emit diagnosticsChanged();
}

QVariantMap EngineController::getFilterParams() {
  QVariantMap map;
  map["hpfFrequency"] = m_uiFilter.hpfFrequency();
  map["hpfEnabled"] = m_uiFilter.isHpfEnabled();
  map["hpfSlope"] =
      m_uiFilter.hpfSlope() == HighLowPassFilter::FilterSlope::Slope24dB ? 1
                                                                         : 0;
  map["hpfQ"] = m_uiFilter.hpfQ();
  map["lpfFrequency"] = m_uiFilter.lpfFrequency();
  map["lpfEnabled"] = m_uiFilter.isLpfEnabled();
  map["lpfSlope"] =
      m_uiFilter.lpfSlope() == HighLowPassFilter::FilterSlope::Slope24dB ? 1
                                                                         : 0;
  map["lpfQ"] = m_uiFilter.lpfQ();
  map["bypassed"] = m_filterBypassed.load(std::memory_order_relaxed);
  return map;
}

QVariantList EngineController::getFilterCurve(int numPoints) {
  QVariantList list;
  if (numPoints < 2)
    return list;

  std::vector<double> freqs(static_cast<size_t>(numPoints));
  for (int i = 0; i < numPoints; ++i) {
    freqs[static_cast<size_t>(i)] =
        20.0 *
        std::pow(20000.0 / 20.0, static_cast<double>(i) / (numPoints - 1));
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

void EngineController::setCompressorParams(double thresholdDb, double ratio,
                                           double attackMs, double releaseMs,
                                           double makeupDb, double kneeDb) {
  bool enabled = !m_compBypassed.load(std::memory_order_relaxed);
  double sRate = m_pipeline ? m_pipeline->graphSampleRate() : 48000.0;
  m_uiComp.configure(sRate, thresholdDb, ratio, attackMs, releaseMs, makeupDb,
                     kneeDb, enabled);
  emit compressorParametersChanged();

  if (m_pipeline) {
    // P0-14, P0-21: Configure executed on Control Thread and published
    // lock-free via EpochReclaimer. Live pipeline uses AudioNode::isBypassed()
    // as the bypass gate so parameter changes while bypassed remain ready when
    // unbypassed.
    m_pipeline->updateCompressor(thresholdDb, ratio, attackMs, releaseMs,
                                 makeupDb, kneeDb, true);
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
  map["gainReductionDb"] =
      m_pipeline ? m_pipeline->compressorGainReductionDb() : 0.0;
  return map;
}

void EngineController::setLimiterParams(double ceilingDb, double releaseMs,
                                        double lookaheadMs) {
  bool enabled = !m_limiterBypassed.load(std::memory_order_relaxed);
  double sRate = m_pipeline ? m_pipeline->graphSampleRate() : 48000.0;
  m_uiLimiter.configure(sRate, ceilingDb, releaseMs, lookaheadMs, enabled);
  emit limiterParametersChanged();

  if (m_pipeline) {
    // P0-14, P0-21: Configure and instance preparation executed on Control
    // Thread
    m_pipeline->updatePeakLimiter(ceilingDb, releaseMs, lookaheadMs, true);
  }
}

QVariantMap EngineController::getLimiterParams() {
  QVariantMap map;
  map["ceilingDb"] = m_uiLimiter.ceilingDb();
  map["releaseMs"] = m_uiLimiter.releaseMs();
  map["lookaheadMs"] = m_uiLimiter.lookaheadMs();
  map["latencyFrames"] = m_pipeline
                             ? m_pipeline->limiterLatencyFrames()
                             : static_cast<int>(m_uiLimiter.latencyFrames());
  map["enabled"] = m_uiLimiter.isEnabled();
  map["bypassed"] = m_limiterBypassed.load(std::memory_order_relaxed);
  map["gainReductionDb"] =
      m_pipeline ? m_pipeline->limiterGainReductionDb() : 0.0;
  return map;
}

void EngineController::setReverbParams(double roomSize, double damping,
                                       double wetLevel, double predelayMs,
                                       bool tailPreserving) {
  bool enabled = !m_reverbBypassed.load(std::memory_order_relaxed);
  double sRate = m_pipeline ? m_pipeline->graphSampleRate() : 48000.0;
  m_uiReverb.configure(sRate, roomSize, damping, wetLevel, predelayMs,
                       tailPreserving, enabled);
  emit reverbParametersChanged();

  if (m_pipeline) {
    // P0-14, P0-21: Configure and instance preparation executed on Control
    // Thread (§5.6 tail retention & processing time budget limits)
    m_pipeline->updateReverb(
        roomSize, damping, wetLevel, predelayMs, tailPreserving, true,
        m_reverbMaxTailRetentionSeconds.load(std::memory_order_relaxed),
        m_reverbMaxProcessingTimeMs.load(std::memory_order_relaxed));
  }
}

void EngineController::setReverbLimits(double maxTailRetentionSeconds,
                                       double maxProcessingTimeMs) {
  const double clampedRetention = std::clamp(maxTailRetentionSeconds, 0.1, 30.0);
  const double clampedBudgetMs = std::max(0.0, maxProcessingTimeMs);
  m_reverbMaxTailRetentionSeconds.store(clampedRetention,
                                        std::memory_order_relaxed);
  m_reverbMaxProcessingTimeMs.store(clampedBudgetMs,
                                    std::memory_order_relaxed);
  if (m_pipeline) {
    m_pipeline->updateReverb(
        m_uiReverb.roomSize(), m_uiReverb.damping(), m_uiReverb.wetLevel(),
        m_uiReverb.predelayMs(), m_uiReverb.isTailPreserving(), true,
        clampedRetention, clampedBudgetMs);
  }
  emit reverbParametersChanged();
}

QVariantMap EngineController::getReverbParams() {
  QVariantMap map;
  map["roomSize"] = m_uiReverb.roomSize();
  map["damping"] = m_uiReverb.damping();
  map["wetLevel"] = m_pipeline ? m_pipeline->currentTelemetry().reverbWetLevel
                               : m_uiReverb.wetLevel();
  map["predelayMs"] = m_uiReverb.predelayMs();
  map["tailPreserving"] = m_uiReverb.isTailPreserving();
  map["enabled"] = m_uiReverb.isEnabled();
  map["bypassed"] = m_reverbBypassed.load(std::memory_order_relaxed);
  map["maxTailRetentionSeconds"] =
      m_reverbMaxTailRetentionSeconds.load(std::memory_order_relaxed);
  map["maxProcessingTimeMs"] =
      m_reverbMaxProcessingTimeMs.load(std::memory_order_relaxed);
  uint64_t budgetExceeded = 0;
  uint64_t tailTimeouts = 0;
  bool tailPreserveBypass = false;
  if (m_pipeline) {
    if (auto mc = m_pipeline->cloneMasterChainForVerification()) {
      for (size_t i = 0; i < mc->nodeCount(); ++i) {
        if (auto *rev =
                dynamic_cast<engine::pipeline::ReverbNode *>(mc->node(i))) {
          budgetExceeded = rev->processingBudgetExceededCount();
          tailTimeouts = rev->tailRetentionTimeoutRetireCount();
          tailPreserveBypass =
              rev->bypassBehavior() ==
              engine::pipeline::BypassBehavior::TailPreserving;
          break;
        }
      }
    }
  }
  map["tailPreservingBypassActive"] = tailPreserveBypass;
  map["processingBudgetExceededCount"] = static_cast<qulonglong>(budgetExceeded);
  map["tailRetentionTimeoutRetireCount"] = static_cast<qulonglong>(tailTimeouts);
  return map;
}

QVariantMap EngineController::getLastPresetImportReport() const {
  std::lock_guard<std::mutex> lock(m_presetReportMutex);
  return m_lastPresetImportReport;
}

QVariantList EngineController::getAudioNodeContracts() const {
  QVariantList out;
  auto appendChain = [&](const std::unique_ptr<engine::pipeline::AudioChain> &ch,
                         const QString &chainLabel) {
    if (!ch)
      return;
    for (size_t i = 0; i < ch->nodeCount(); ++i) {
      const auto *n = ch->node(i);
      if (!n)
        continue;
      const auto c = n->contract();
      QVariantMap m;
      m["chain"] = chainLabel;
      m["name"] = QString::fromStdString(c.name);
      switch (c.affiliation) {
      case engine::pipeline::NodeAffiliation::RenderCommon:
        m["affiliation"] = QStringLiteral("RenderCommon");
        break;
      case engine::pipeline::NodeAffiliation::ListeningCorrection:
        m["affiliation"] = QStringLiteral("ListeningCorrection");
        break;
      case engine::pipeline::NodeAffiliation::RenderMonitorOnly:
        m["affiliation"] = QStringLiteral("RenderMonitorOnly");
        break;
      }
      m["inputChannels"] = static_cast<int>(c.inputChannels);
      m["outputChannels"] = static_cast<int>(c.outputChannels);
      m["sampleFormat"] = QStringLiteral("Float64Planar");
      m["maxBlockSize"] = static_cast<int>(c.maxBlockSize);
      m["sampleRate"] = c.sampleRate;
      m["latencyFrames"] = QVariant::fromValue(static_cast<qint64>(c.latencyFrames));
      switch (c.state) {
      case engine::pipeline::NodeState::Uninitialized:
        m["state"] = QStringLiteral("Uninitialized");
        break;
      case engine::pipeline::NodeState::Active:
        m["state"] = QStringLiteral("Active");
        break;
      case engine::pipeline::NodeState::Bypassed:
        m["state"] = QStringLiteral("Bypassed");
        break;
      case engine::pipeline::NodeState::TailRingout:
        m["state"] = QStringLiteral("TailRingout");
        break;
      case engine::pipeline::NodeState::Error:
        m["state"] = QStringLiteral("Error");
        break;
      }
      m["bypassBehavior"] =
          (c.bypassBehavior == engine::pipeline::BypassBehavior::TailPreserving)
              ? QStringLiteral("TailPreserving")
              : QStringLiteral("Immediate");
      out.append(m);
    }
  };

  if (m_pipeline) {
    appendChain(m_pipeline->cloneMasterChainForVerification(),
                QStringLiteral("Master"));
    appendChain(m_pipeline->cloneListeningChainForVerification(),
                QStringLiteral("Listening"));
  }
  return out;
}

QStringList EngineController::availableListeningPresets() {
  return m_presetManager.listPresets(
      engine::preset::PresetCategories::ListeningCorrection);
}

void EngineController::setListeningPreset(const QString &presetName) {
  if (m_pipeline) {
    // P0-27: Precompute preset coefficients on Control Thread and publish
    // atomically via Mailbox
    m_pipeline->paramMailbox().setListeningPreset(presetName.toStdString());
  }
  bool ok = m_uiListeningCorrection.applyPreset(presetName.toStdString());
  if (ok) {
    m_listeningPresetName = presetName;
    emit listeningPresetChanged(presetName);
    emit peqParametersChanged();
    emit diagnosticsChanged();
    log::LogService::instance().log(
        "ListeningCorrection", "ApplyPreset", "Success",
        "Applied Listening Correction preset: " + presetName, "Preset",
        presetName);
  }
}

QVariantMap EngineController::getListeningBand(bool isRightChannel,
                                               int bandIndex) {
  QVariantMap map;
  if (bandIndex < 0 ||
      bandIndex >= static_cast<int>(ParametricEQ::BANDS_PER_CHANNEL))
    return map;

  const auto &band =
      isRightChannel
          ? m_uiListeningCorrection.rightBand(static_cast<size_t>(bandIndex))
          : m_uiListeningCorrection.leftBand(static_cast<size_t>(bandIndex));
  map["index"] = bandIndex;
  map["channel"] = isRightChannel ? "Right" : "Left";
  map["enabled"] = band.enabled;
  map["type"] = static_cast<int>(band.type);
  map["frequency"] = band.frequency;
  map["gainDb"] = band.gainDb;
  map["q"] = band.Q;
  return map;
}

void EngineController::setListeningBand(bool isRightChannel, int bandIndex,
                                        double frequency, double gainDb,
                                        double q, int filterType,
                                        bool enabled) {
  if (bandIndex < 0 ||
      bandIndex >= static_cast<int>(ParametricEQ::BANDS_PER_CHANNEL))
    return;

  ParametricEQ::BandConfig cfg;
  cfg.frequency = frequency;
  cfg.gainDb = gainDb;
  cfg.Q = q;
  cfg.type = static_cast<BiquadFilterType>(std::clamp(filterType, 0, 5));
  cfg.enabled = enabled;

  if (m_pipeline) {
    m_pipeline->paramMailbox().setListeningBand(isRightChannel,
                                                static_cast<size_t>(bandIndex),
                                                cfg);
  }
  if (isRightChannel) {
    m_uiListeningCorrection.setRightBand(static_cast<size_t>(bandIndex), cfg);
  } else {
    m_uiListeningCorrection.setLeftBand(static_cast<size_t>(bandIndex), cfg);
  }
  m_listeningPresetName = QStringLiteral("Custom");
  emit listeningPresetChanged(m_listeningPresetName);
  emit diagnosticsChanged();
}

QVariantList EngineController::getListeningCurve(bool isRightChannel,
                                                 int numPoints) {
  QVariantList list;
  if (numPoints < 2)
    return list;

  std::vector<double> freqs(static_cast<size_t>(numPoints));
  for (int i = 0; i < numPoints; ++i) {
    freqs[static_cast<size_t>(i)] =
        20.0 *
        std::pow(20000.0 / 20.0, static_cast<double>(i) / (numPoints - 1));
  }

  std::vector<double> leftDb, rightDb;
  m_uiListeningCorrection.calculateCompositeResponse(freqs, leftDb, rightDb);
  const auto &target = isRightChannel ? rightDb : leftDb;
  for (int i = 0; i < numPoints; ++i) {
    QVariantMap pt;
    pt["f"] = freqs[static_cast<size_t>(i)];
    pt["db"] = target[static_cast<size_t>(i)];
    list.append(pt);
  }
  return list;
}

bool EngineController::findDspNode(const QString &nodeName, uint8_t &outChain,
                                   uint8_t &outIndex) const noexcept {
  auto idxOrInvalid = [&](size_t canonicalSlot) -> bool {
    const uint8_t idx = m_masterNodeIndices[canonicalSlot].load(std::memory_order_relaxed);
    if (idx == 0xFF) return false;
    outChain = 0;
    outIndex = idx;
    return true;
  };
  if (nodeName == "PEQ") return idxOrInvalid(0);
  if (nodeName == "GEQ") return idxOrInvalid(1);
  if (nodeName == "Filter") return idxOrInvalid(2);
  if (nodeName == "Compressor") return idxOrInvalid(3);
  if (nodeName == "ExternalInsert" || nodeName == "Insert") return idxOrInvalid(4);
  if (nodeName == "Reverb") return idxOrInvalid(5);
  if (nodeName == "Limiter") return idxOrInvalid(6);
  if (nodeName == "ListeningCorrection") {
    outChain = 1;
    outIndex = 0;
    return true;
  }
  return false;
}

void EngineController::emitDspNodeSignal(const QString &nodeName) {
  if (nodeName == "PEQ")
    emit peqParametersChanged();
  else if (nodeName == "GEQ")
    emit geqParametersChanged();
  else if (nodeName == "Filter")
    emit filterParametersChanged();
  else if (nodeName == "Compressor")
    emit compressorParametersChanged();
  else if (nodeName == "ExternalInsert" || nodeName == "Insert")
    emit externalInsertStatusChanged();
  else if (nodeName == "Reverb")
    emit reverbParametersChanged();
  else if (nodeName == "Limiter")
    emit limiterParametersChanged();
  else if (nodeName == "ListeningCorrection")
    emit outputRoutingModeChanged(
        m_outputRoutingMode.load(std::memory_order_relaxed));
}

void EngineController::setDspNodeBypass(const QString &nodeName, bool bypass) {
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
          "DSP", "NodeBypass", "Failed",
          QString(
              "Failed to set bypass for %1: parameter queue full (code: 3001)")
              .arg(nodeName),
          "DSPNode", nodeName);
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
    m_uiPeq.setEnabled(!bypass);
  } else if (nodeName == "GEQ") {
    m_geqBypassed.store(bypass, std::memory_order_relaxed);
    m_uiGeq.setEnabled(!bypass);
  } else if (nodeName == "Filter") {
    m_filterBypassed.store(bypass, std::memory_order_relaxed);
  } else if (nodeName == "Compressor") {
    m_compBypassed.store(bypass, std::memory_order_relaxed);
    m_uiComp.setEnabled(!bypass);
  } else if (nodeName == "ExternalInsert" || nodeName == "Insert") {
    m_insertBypassed.store(bypass, std::memory_order_relaxed);
    if (m_insertNode) {
      m_insertNode->setBypassed(bypass);
    }
  } else if (nodeName == "Reverb") {
    m_reverbBypassed.store(bypass, std::memory_order_relaxed);
    m_uiReverb.setEnabled(!bypass);
  } else if (nodeName == "Limiter") {
    m_limiterBypassed.store(bypass, std::memory_order_relaxed);
    m_uiLimiter.setEnabled(!bypass);
  } else if (nodeName == "ListeningCorrection") {
    m_listeningCorrectionBypassed.store(bypass, std::memory_order_relaxed);
  }

  emitDspNodeSignal(nodeName);
  emit diagnosticsChanged();
  log::LogService::instance().log(
      "DSP", "NodeBypass", "Success",
      QString("%1 Bypass %2").arg(nodeName, bypass ? "Engaged" : "Disengaged"),
      "DSPNode", nodeName);
}

bool EngineController::resyncDspNodeBypass(const QString &nodeName) {
  if (!m_pipeline)
    return false;
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
        "DSP", "NodeBypassResync", "Success",
        QString("%1 bypass resynchronized to %2")
            .arg(nodeName, state ? "true" : "false"),
        "DSPNode", nodeName);
    emitDspNodeSignal(nodeName);
    return true;
  }

  log::LogService::instance().log(
      "DSP", "NodeBypassResync", "Failed",
      QString("Failed to resynchronize bypass for %1: queue full (code: 3006)")
          .arg(nodeName),
      "DSPNode", nodeName);
  return false;
}

bool EngineController::isDspNodeBypassed(const QString &nodeName) {
  if (nodeName == "PEQ")
    return m_peqBypassed.load(std::memory_order_relaxed);
  if (nodeName == "GEQ")
    return m_geqBypassed.load(std::memory_order_relaxed);
  if (nodeName == "Filter")
    return m_filterBypassed.load(std::memory_order_relaxed);
  if (nodeName == "Compressor")
    return m_compBypassed.load(std::memory_order_relaxed);
  if (nodeName == "ExternalInsert" || nodeName == "Insert")
    return m_insertBypassed.load(std::memory_order_relaxed);
  if (nodeName == "Limiter")
    return m_limiterBypassed.load(std::memory_order_relaxed);
  if (nodeName == "Reverb")
    return m_reverbBypassed.load(std::memory_order_relaxed);
  if (nodeName == "ListeningCorrection")
    return m_listeningCorrectionBypassed.load(std::memory_order_relaxed);
  return false;
}

bool EngineController::isDspNodeAppliedBypassed(
    const QString &nodeName) const noexcept {
  if (!m_pipeline)
    return false;
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

void EngineController::renderOfflineBlock(float *outInterleaved,
                                          size_t frames) {
  if (m_audioOutput) {
    m_audioOutput->renderOfflineBlock(outInterleaved, frames);
  }
}

void EngineController::renderOfflineRawBlock(void *outInterleaved,
                                             size_t frames) {
  if (m_audioOutput) {
    m_audioOutput->renderOfflineRawBlock(outInterleaved, frames);
  }
}

QVariantMap EngineController::getDeckState(const QString &deckId) {
  QVariantMap map;
  if (!m_deckManager || deckId.isEmpty())
    return map;
  char id = deckId.toUpper().at(0).toLatin1();
  auto *d = m_deckManager->deck(id);
  if (!d)
    return map;

  if (d->checkAndResetTrackChanged()) {
    emit deckStateChanged(deckId);
  }

  map["id"] = QString(d->id());
  map["state"] = static_cast<int>(d->state());
  map["playhead"] = QVariant::fromValue(d->playhead());
  map["offsetFrames"] = QVariant::fromValue(d->offsetFrames());
  map["levelMatchDb"] = d->levelMatchGainDb();
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
    map["durationSeconds"] =
        (d->trackInfo().sampleRate > 0)
            ? (static_cast<double>(d->trackInfo().totalFrames) /
               d->trackInfo().sampleRate)
            : 0.0;
  }

  const auto &fs = d->folderSession();
  map["folderActive"] = fs.isActive();
  map["folderPath"] = QString::fromStdString(fs.folderPath());
  map["folderCurrentIndex"] = static_cast<int>(fs.currentIndex());
  map["folderTotalTracks"] = static_cast<int>(fs.totalTracks());

  const auto *ra = fs.readAheadTrack();
  map["readAheadFile"] = ra ? QString::fromStdString(ra->fileName) : QString();

  const auto &wsMgr = engine::workspace::WorkspaceManager::instance();
  const bool wsOpen = wsMgr.hasOpenWorkspace();
  const QString trackQPath = QString::fromStdString(d->trackInfo().path);
  const bool insideOpenWs =
      wsOpen && !trackQPath.isEmpty() &&
      wsMgr.isPathInsideOpenWorkspace(trackQPath, false);
  map["isInsideOpenWorkspace"] = insideOpenWs;
  map["canCopyToWorkspace"] = wsOpen && !trackQPath.isEmpty() && !insideOpenWs;

  return map;
}

// =========================================================================
// Phase 3: External Insert Host Operations (Section 6.0)
// =========================================================================

QVariantList EngineController::discoverExternalInsertPorts() const {
  QVariantList out;
  const double sr = m_pipeline ? m_pipeline->graphSampleRate() : 48000.0;
  const size_t bs = m_audioOutput ? m_audioOutput->diagnostics().bufferFrames : 256;
  const auto ports = engine::insert::ExternalInsertBridge::discoverPorts(sr, bs);
  for (const auto &p : ports) {
    QVariantMap m;
    m["clientName"] = QString::fromStdString(p.clientName);
    m["sendPortL"] = QString::fromStdString(p.sendPortL);
    m["sendPortR"] = QString::fromStdString(p.sendPortR);
    m["returnPortL"] = QString::fromStdString(p.returnPortL);
    m["returnPortR"] = QString::fromStdString(p.returnPortR);
    m["sampleRate"] = p.sampleRate;
    m["bufferSize"] = static_cast<int>(p.bufferSize);
    m["reportedLatencyFrames"] = static_cast<int>(p.reportedLatencyFrames);
    m["latencyReported"] = p.latencyReported;
    out.append(m);
  }
  return out;
}

bool EngineController::configureExternalInsert(
    const QString &clientName, const QString &sendL, const QString &sendR,
    const QString &returnL, const QString &returnR, double sampleRate,
    int bufferSize, int reportedLatencyFrames) {
  return configureExternalInsertWithMetadata(
      clientName, sendL, sendR, returnL, returnR, sampleRate, bufferSize,
      reportedLatencyFrames, reportedLatencyFrames >= 0, QString(), QString(),
      QString());
}

bool EngineController::configureExternalInsertWithMetadata(
    const QString &clientName, const QString &sendL, const QString &sendR,
    const QString &returnL, const QString &returnR, double sampleRate,
    int bufferSize, int reportedLatencyFrames, bool latencyReported,
    const QString &pluginName, const QString &pluginUri,
    const QString &hostPresetReference) {
  if (!m_insertNode)
    return false;

  double sRate = (sampleRate > 0.0)
                     ? sampleRate
                     : (m_pipeline ? m_pipeline->graphSampleRate() : 48000.0);
  size_t bSize = (bufferSize > 0) ? static_cast<size_t>(bufferSize) : 256;

  engine::insert::ExternalPortInfo info{
      clientName.toStdString(),
      sendL.toStdString(),
      sendR.toStdString(),
      returnL.toStdString(),
      returnR.toStdString(),
      sRate,
      bSize,
      static_cast<uint32_t>(std::max(0, reportedLatencyFrames)),
      latencyReported,
      pluginName.toStdString(),
      pluginUri.toStdString(),
      hostPresetReference.toStdString()};

  bool ok = m_insertNode->bridge().configure(info, sRate, bSize);
  if (ok) {
    m_insertNode->setBypassed(false);
    m_insertBypassed.store(false, std::memory_order_relaxed);
    QJsonObject det;
    det.insert(QStringLiteral("host_name"), clientName);
    det.insert(QStringLiteral("plugin_id"),
               pluginUri.isEmpty() ? QStringLiteral("external_jack_bridge") : pluginUri);
    det.insert(QStringLiteral("plugin_name"), pluginName);
    det.insert(QStringLiteral("host_preset_reference"), hostPresetReference);
    det.insert(QStringLiteral("sample_rate_hz"), sRate);
    det.insert(QStringLiteral("buffer_size"), static_cast<int>(bSize));
    det.insert(QStringLiteral("reported_latency_frames"),
               reportedLatencyFrames);
    det.insert(QStringLiteral("latency_reported"), latencyReported);
    det.insert(QStringLiteral("reported_latency_ms"),
               (sRate > 0.0) ? (reportedLatencyFrames * 1000.0 / sRate) : 0.0);
    det.insert(QStringLiteral("latency_source"),
               latencyReported ? QStringLiteral("host_reported")
                               : QStringLiteral("not_reported"));
    log::LogService::instance().log(
        "ExternalInsert", "Configure", "Success",
        QString("Connected external insert host '%1' [Send: %2/%3, Return: "
                "%4/%5, Latency: %6]")
            .arg(clientName, sendL, sendR, returnL, returnR,
                 latencyReported ? (QString::number(reportedLatencyFrames) + " frames")
                                 : QStringLiteral("Not Reported")),
        "InsertBridge", clientName,
        QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)));
  } else {
    QJsonObject det;
    det.insert(QStringLiteral("host_name"), clientName);
    det.insert(QStringLiteral("stage"), QStringLiteral("bridge_configure"));
    det.insert(QStringLiteral("error_detail"),
               QString::fromStdString(m_insertNode->bridge().lastError()));
    log::LogService::instance().log(
        "ExternalInsert", "Configure", "Failed",
        QString("Failed to configure external insert host '%1': %2")
            .arg(clientName,
                 QString::fromStdString(m_insertNode->bridge().lastError())),
        "InsertBridge", clientName,
        QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)),
        QString(), QString(), log::ErrorCode::ExternalHostFailed, 0.0);
  }

  emit externalInsertStatusChanged();
  emit diagnosticsChanged();
  return ok;
}

void EngineController::setExternalInsertPosition(int positionIndex) {
  const int clamped = std::clamp(positionIndex, 0, 6);
  if (m_pipeline) {
    std::array<uint8_t, 7> newIndices{};
    if (m_pipeline->repositionExternalInsert(static_cast<size_t>(clamped), newIndices)) {
      for (size_t k = 0; k < 7; ++k) {
        m_masterNodeIndices[k].store(newIndices[k], std::memory_order_relaxed);
      }
      m_externalInsertPosition.store(clamped, std::memory_order_relaxed);
      emit externalInsertStatusChanged();
      emit diagnosticsChanged();
      log::LogService::instance().log(
          QStringLiteral("ExternalInsert"), QStringLiteral("Reposition"),
          QStringLiteral("Success"),
          QString("Repositioned External Insert to master slot %1").arg(clamped),
          QStringLiteral("InsertBridge"), QStringLiteral("External Insert"));
    }
  }
}

int EngineController::externalInsertPosition() const noexcept {
  return m_externalInsertPosition.load(std::memory_order_relaxed);
}

void EngineController::setExternalInsertDisconnectPolicy(int policy) {
  if (!m_insertNode)
    return;
  const int clamped = std::clamp(policy, 0, 2);
  m_insertNode->bridge().setDisconnectPolicy(
      static_cast<engine::insert::DisconnectPolicy>(clamped));
  emit externalInsertStatusChanged();
  emit diagnosticsChanged();
}

int EngineController::externalInsertDisconnectPolicy() const noexcept {
  if (!m_insertNode)
    return 0;
  return static_cast<int>(m_insertNode->bridge().disconnectPolicy());
}

void EngineController::setExternalInsertBypass(bool bypass) {
  if (!m_insertNode)
    return;
  m_insertBypassed.store(bypass, std::memory_order_relaxed);
  m_insertNode->setBypassed(bypass);
  if (m_pipeline) {
    uint8_t chain = 0, idx = 0;
    if (findDspNode(QStringLiteral("ExternalInsert"), chain, idx)) {
      engine::dsp::DspCommand cmd;
      cmd.type = engine::dsp::DspCommandType::SetNodeBypass;
      cmd.subIndex = chain;
      cmd.index = idx;
      cmd.boolFlag1 = bypass ? 1 : 0;
      m_pipeline->paramQueue().push(cmd);
    }
  }
  log::LogService::instance().log(
      "ExternalInsert", "SetBypass", "Success",
      bypass ? "External insert bypassed (Dry)."
             : "External insert active (Wet).",
      "InsertBridge",
      QString::fromStdString(m_insertNode->bridge().config().clientName));
  emit externalInsertStatusChanged();
  emit diagnosticsChanged();
}

void EngineController::triggerInsertLatencyMeasurement() {
  if (!m_insertNode)
    return;
  m_insertNode->bridge().triggerLatencyMeasurement();
  log::LogService::instance().log(
      "ExternalInsert", "MeasureLatency", "Success",
      "Triggered roundtrip impulse latency measurement.", "InsertBridge",
      QString::fromStdString(m_insertNode->bridge().config().clientName));
  emit externalInsertStatusChanged();
}

void EngineController::calibrateInsertLatency() {
  if (!m_insertNode)
    return;
  triggerInsertLatencyMeasurement();
  uint64_t sendFrame = m_insertNode->bridge().impulseSendFrame();
  uint32_t repLat = m_insertNode->bridge().reportedLatencyFrames();
  uint64_t delta = (repLat > 0) ? static_cast<uint64_t>(repLat) : 128ULL;
  uint64_t returnFrame = sendFrame + delta;
  m_insertNode->bridge().recordImpulseReturn(
      returnFrame, engine::insert::LatencySource::SimulatedMock);
  log::LogService::instance().log(
      "ExternalInsert", "CalibrateLatency", "Simulated",
      "Calibrated latency using simulated mock loopback (NOT physical hardware "
      "measurement)",
      "InsertBridge",
      QString::fromStdString(m_insertNode->bridge().config().clientName));
  emit externalInsertStatusChanged();
}

void EngineController::recordInsertImpulseReturn(qint64 returnFrame) {
  if (!m_insertNode)
    return;
  m_insertNode->bridge().recordImpulseReturn(
      static_cast<uint64_t>(returnFrame),
      engine::insert::LatencySource::HardwareMeasured);
  const auto &b = m_insertNode->bridge();
  const double sr = b.config().sampleRate;
  const double reportedMs =
      (sr > 0.0)
          ? (static_cast<double>(b.reportedLatencyFrames()) * 1000.0 / sr)
          : 0.0;
  QJsonObject det;
  det.insert(QStringLiteral("host_name"),
             QString::fromStdString(b.config().clientName));
  det.insert(QStringLiteral("reported_latency_frames"),
             static_cast<int>(b.reportedLatencyFrames()));
  det.insert(QStringLiteral("reported_latency_ms"), reportedMs);
  det.insert(QStringLiteral("measured_latency_frames"),
             static_cast<int>(b.measuredLatencyFrames()));
  det.insert(QStringLiteral("measured_latency_ms"), b.measuredLatencyMs());
  det.insert(QStringLiteral("latency_source"),
             QString::fromUtf8(
                 engine::insert::ExternalInsertBridge::latencySourceToString(
                     b.latencySource())));
  log::LogService::instance().log(
      "ExternalInsert", "RecordImpulseReturn", "Success",
      QString(
          "Measured physical hardware round-trip latency: %1 frames (%2 ms).")
          .arg(b.measuredLatencyFrames())
          .arg(b.measuredLatencyMs(), 0, 'f', 2),
      "InsertBridge", QString::fromStdString(b.config().clientName),
      QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)));
  emit externalInsertStatusChanged();
}

void EngineController::beginInsertLatencyCalibration() {
  if (!m_insertNode)
    return;
  m_insertNode->bridge().beginLatencyCalibrationSession();
  emit externalInsertStatusChanged();
}

bool EngineController::recordInsertLatencyCalibrationSample(qint64 measuredFrames,
                                                            double peakSnrDb) {
  if (!m_insertNode)
    return false;
  auto &b = m_insertNode->bridge();
  const bool stable = b.recordLatencyCalibrationSample(
      static_cast<int64_t>(measuredFrames), peakSnrDb);
  const auto cand = b.latencyCalibrationCandidate();
  if (!stable &&
      (cand.confidence == engine::insert::LatencyConfidence::Unstable ||
       cand.confidence == engine::insert::LatencyConfidence::LowSnr)) {
    log::LogService::instance().log(
        QStringLiteral("ExternalInsert"), QStringLiteral("LatencyCalibration"),
        QStringLiteral("Warning"),
        QString("Latency calibration held (%1): %2")
            .arg(QString::fromUtf8(
                     engine::insert::ExternalInsertBridge::latencyConfidenceToString(
                         cand.confidence)),
                 QString::fromStdString(cand.summary)),
        QStringLiteral("InsertBridge"),
        QString::fromStdString(b.config().clientName), QStringLiteral("{}"),
        QString(), QString(), log::ErrorCode::ExternalInsertLatencyUnstable, 0.0);
  }
  emit externalInsertStatusChanged();
  return stable;
}

bool EngineController::confirmRecommendedInsertLatency(bool allowOverrideUnsafe) {
  if (!m_insertNode)
    return false;
  auto &b = m_insertNode->bridge();
  const bool applied = b.confirmRecommendedLatency(allowOverrideUnsafe);
  if (applied) {
    m_insertNode->delayBuffer().setDelayFrames(
        static_cast<size_t>(std::max<int64_t>(0, b.delayCompensationFrames())));
    const auto cand = b.latencyCalibrationCandidate();
    log::LogService::instance().log(
        QStringLiteral("ExternalInsert"), QStringLiteral("LatencyConfirm"),
        QStringLiteral("Success"),
        QString("Confirmed and applied %1 frames to DelayCompensationBuffer")
            .arg(cand.recommendedFrames),
        QStringLiteral("InsertBridge"),
        QString::fromStdString(b.config().clientName));
    emit externalInsertStatusChanged();
    emit diagnosticsChanged();
  }
  return applied;
}

void EngineController::cancelInsertLatencyCalibration() {
  if (!m_insertNode)
    return;
  m_insertNode->bridge().cancelLatencyCalibrationSession();
  emit externalInsertStatusChanged();
}

QVariantMap EngineController::getLatencyAlignmentReport() const {
  QVariantMap map;
  if (!m_insertNode)
    return map;
  const auto &b = m_insertNode->bridge();
  const auto cand = b.latencyCalibrationCandidate();
  map["reportedLatencyFrames"] = static_cast<int>(b.reportedLatencyFrames());
  map["latencyReported"] = b.hasReportedLatency();
  map["reportedLatencyText"] = QString::fromStdString(b.reportedLatencyText());
  map["measuredLatencyFrames"] = static_cast<int>(b.measuredLatencyFrames());
  map["measuredLatencyMs"] = b.measuredLatencyMs();
  map["delayCompensationFrames"] =
      static_cast<int>(m_insertNode->delayBuffer().delayFrames());
  map["latencySource"] = QString::fromUtf8(
      engine::insert::ExternalInsertBridge::latencySourceToString(
          b.latencySource()));
  map["sessionActive"] = cand.sessionActive;
  map["hasMeasurement"] = cand.hasMeasurement;
  map["confirmed"] = cand.confirmed;
  map["recommendedFrames"] = QVariant::fromValue(static_cast<qint64>(cand.recommendedFrames));
  map["minFrames"] = QVariant::fromValue(static_cast<qint64>(cand.minFrames));
  map["maxFrames"] = QVariant::fromValue(static_cast<qint64>(cand.maxFrames));
  map["spreadFrames"] = QVariant::fromValue(static_cast<qint64>(cand.spreadFrames));
  map["peakSnrDb"] = cand.peakSnrDb;
  map["sampleCount"] = static_cast<int>(cand.sampleCount);
  map["confidence"] = static_cast<int>(cand.confidence);
  map["confidenceText"] = QString::fromUtf8(
      engine::insert::ExternalInsertBridge::latencyConfidenceToString(
          cand.confidence));
  map["summary"] = QString::fromStdString(cand.summary);
  return map;
}

QVariantMap EngineController::getExternalInsertStatus() const {
  QVariantMap map;
  if (!m_insertNode)
    return map;
  const auto &b = m_insertNode->bridge();
  const auto &cfg = b.config();
  const auto trDiag = b.transportDiagnostics();

  map["state"] = static_cast<int>(b.state());
  map["stateText"] = QString::fromUtf8(
      engine::insert::ExternalInsertBridge::stateToString(b.state()));
  map["isBypassed"] = m_insertNode->isBypassed();
  map["clientName"] = QString::fromStdString(cfg.clientName);
  map["sendPortL"] = QString::fromStdString(cfg.sendPortL);
  map["sendPortR"] = QString::fromStdString(cfg.sendPortR);
  map["returnPortL"] = QString::fromStdString(cfg.returnPortL);
  map["returnPortR"] = QString::fromStdString(cfg.returnPortR);
  map["sampleRate"] = cfg.sampleRate;
  map["bufferSize"] = static_cast<int>(cfg.bufferSize);
  map["reportedLatencyFrames"] = static_cast<int>(b.reportedLatencyFrames());
  map["latencyReported"] = b.hasReportedLatency();
  map["reportedLatencyText"] = QString::fromStdString(b.reportedLatencyText());
  map["measuredLatencyFrames"] = static_cast<int>(b.measuredLatencyFrames());
  map["measuredLatencyMs"] = b.measuredLatencyMs();
  map["delayCompensationFrames"] =
      static_cast<int>(m_insertNode->delayBuffer().delayFrames());
  map["latencySource"] = QString::fromUtf8(
      engine::insert::ExternalInsertBridge::latencySourceToString(
          b.latencySource()));
  map["insertPosition"] = m_externalInsertPosition.load(std::memory_order_relaxed);
  map["disconnectPolicy"] = static_cast<int>(b.disconnectPolicy());
  map["pluginName"] = QString::fromStdString(cfg.pluginName);
  map["pluginUri"] = QString::fromStdString(cfg.pluginUri);
  map["hostPresetReference"] = QString::fromStdString(cfg.hostPresetReference);
  map["xrunCount"] = static_cast<int>(b.xrunCount());
  map["lastError"] = QString::fromStdString(b.lastError());
  map["transportConnected"] = trDiag.connected;
  map["transportSimulationMode"] = trDiag.simulationMode;
  map["transportResolvedPorts"] = static_cast<int>(trDiag.resolvedTargetPorts);
  map["transportActiveLinks"] = static_cast<int>(trDiag.activeLinksCreated);
  map["transportError"] = QString::fromStdString(trDiag.errorDetail);

  return map;
}

void EngineController::disconnectExternalInsert() {
  if (!m_insertNode)
    return;
  m_insertNode->bridge().handleDisconnect();
  m_insertNode->setBypassed(true);
  log::LogService::instance().log(
      "ExternalInsert", "Disconnect", "Success",
      "External insert host disconnected by user.", "InsertBridge",
      QString::fromStdString(m_insertNode->bridge().config().clientName));
  emit externalInsertStatusChanged();
  emit diagnosticsChanged();
}

void EngineController::resetInsertXRuns() {
  if (!m_insertNode)
    return;
  m_insertNode->bridge().resetXRunCount();
  emit externalInsertStatusChanged();
}

void EngineController::simulateInsertDisconnect() {
  if (!m_insertNode)
    return;
  m_insertNode->bridge().handleDisconnect();
  log::LogService::instance().log(
      "ExternalInsert", "Disconnect", "Warning",
      "External host disconnected or port lost. Engaged automatic bypass.",
      "InsertBridge",
      QString::fromStdString(m_insertNode->bridge().config().clientName));
  emit externalInsertStatusChanged();
}

void EngineController::simulateInsertRecovery() {
  if (!m_insertNode)
    return;
  m_insertNode->bridge().handleRecovery();
  log::LogService::instance().log(
      "ExternalInsert", "Recovery",
      m_insertNode->bridge().state() == engine::insert::InsertState::Connected
          ? "Success"
          : "Failed",
      "Attempted external host reconnection.", "InsertBridge",
      QString::fromStdString(m_insertNode->bridge().config().clientName));
  emit externalInsertStatusChanged();
}

void EngineController::setInsertDelayCompensation(int frames) {
  if (!m_insertNode)
    return;
  m_insertNode->setDelayCompensationFrames(
      static_cast<size_t>(std::max(0, frames)));
  emit externalInsertStatusChanged();
}

// =========================================================================
// Phase 3: Common Preset Management (Specification Section 11.1)
// =========================================================================

QStringList EngineController::listPresets(const QString &category) const {
  return m_presetManager.listPresets(category);
}

bool EngineController::savePreset(const QString &category,
                                  const QString &presetName,
                                  const QVariantMap &data) {
  QJsonObject json = QJsonObject::fromVariantMap(data);
  bool ok = m_presetManager.savePreset(category, presetName, json);
  if (ok) {
    log::LogService::instance().log(
        "PresetManagement", "SavePreset", "Success",
        QString("Preset '%1' saved successfully under category '%2'.")
            .arg(presetName, category),
        category, presetName);
    emit presetManagementChanged();
  } else {
    log::LogService::instance().log(
        "PresetManagement", "SavePreset", "Failed",
        QString("Failed to save preset '%1' under category '%2'.")
            .arg(presetName, category),
        category, presetName);
  }
  return ok;
}

QVariantMap EngineController::loadPreset(const QString &category,
                                         const QString &presetName) {
  QJsonObject json = m_presetManager.loadPreset(category, presetName);
  return json.toVariantMap();
}

bool EngineController::deletePreset(const QString &category,
                                    const QString &presetName) {
  bool ok = m_presetManager.deletePreset(category, presetName);
  if (ok) {
    log::LogService::instance().log(
        "PresetManagement", "DeletePreset", "Success",
        QString("Preset '%1' deleted from category '%2'.")
            .arg(presetName, category),
        category, presetName);
    emit presetManagementChanged();
  }
  return ok;
}

bool EngineController::applyListeningCorrectionJson(const QJsonObject &obj,
                                                    const QString &presetLabel) {
  std::array<ParametricEQ::BandConfig, ParametricEQ::BANDS_PER_CHANNEL> lBands{};
  std::array<ParametricEQ::BandConfig, ParametricEQ::BANDS_PER_CHANNEL> rBands{};
  ParametricEQ::getPresetBandConfigs("Monitor Flat", lBands, rBands);

  std::vector<ParametricEQ::BandConfig> lVec(lBands.begin(), lBands.end());
  std::vector<ParametricEQ::BandConfig> rVec(rBands.begin(), rBands.end());

  auto parseArrayDynamic = [](const QJsonArray &arr,
                              std::vector<ParametricEQ::BandConfig> &out) {
    if (arr.isEmpty())
      return;
    const size_t maxNeed = std::max(out.size(), static_cast<size_t>(arr.size()));
    out.resize(maxNeed);
    for (int i = 0; i < arr.size(); ++i) {
      const QJsonObject bo = arr[i].toObject();
      const int idx = bo.contains("index") ? bo["index"].toInt(i) : i;
      if (idx < 0)
        continue;
      if (static_cast<size_t>(idx) >= out.size()) {
        out.resize(static_cast<size_t>(idx) + 1);
      }
      auto &cfg = out[static_cast<size_t>(idx)];
      cfg.frequency = bo["frequency"].toDouble(cfg.frequency);
      cfg.gainDb = bo.contains("gain_db") ? bo["gain_db"].toDouble(0.0)
                                          : bo["gainDb"].toDouble(0.0);
      cfg.Q = bo.contains("q") ? bo["q"].toDouble(1.0) : bo["Q"].toDouble(1.0);
      cfg.type = static_cast<BiquadFilterType>(
          std::clamp(bo["type"].toInt(0), 0, 5));
      cfg.enabled = bo.contains("enabled") ? bo["enabled"].toBool(true) : true;
    }
  };

  if (obj.contains("left_bands")) {
    parseArrayDynamic(obj["left_bands"].toArray(), lVec);
  } else if (obj.contains("bands")) {
    parseArrayDynamic(obj["bands"].toArray(), lVec);
  }
  if (obj.contains("right_bands")) {
    parseArrayDynamic(obj["right_bands"].toArray(), rVec);
  } else if (obj.contains("bands")) {
    parseArrayDynamic(obj["bands"].toArray(), rVec);
  }

  const auto report = m_uiListeningCorrection.importBandsWithReport(lVec, rVec);
  for (size_t b = 0; b < ParametricEQ::BANDS_PER_CHANNEL; ++b) {
    lBands[b] = m_uiListeningCorrection.leftBand(b);
    rBands[b] = m_uiListeningCorrection.rightBand(b);
  }

  if (m_pipeline) {
    m_pipeline->paramMailbox().applyListeningBands(lBands, rBands);
  }

  {
    std::lock_guard<std::mutex> lock(m_presetReportMutex);
    m_lastPresetImportReport.clear();
    m_lastPresetImportReport["target"] = QStringLiteral("ListeningCorrection");
    m_lastPresetImportReport["requestedLeftBands"] = static_cast<int>(report.requestedLeft);
    m_lastPresetImportReport["requestedRightBands"] = static_cast<int>(report.requestedRight);
    m_lastPresetImportReport["activeLimitPerChannel"] =
        static_cast<int>(ParametricEQ::BANDS_PER_CHANNEL);
    m_lastPresetImportReport["overflowLeftBands"] = static_cast<int>(report.overflowLeft);
    m_lastPresetImportReport["overflowRightBands"] = static_cast<int>(report.overflowRight);
    m_lastPresetImportReport["truncated"] = report.truncated;
    m_lastPresetImportReport["message"] =
        report.truncated
            ? QString("Listening Correction imported %1L/%2R bands: 16 bands active per channel, %3L/%4R excess bands retained in variable overflow model.")
                  .arg(report.requestedLeft)
                  .arg(report.requestedRight)
                  .arg(report.overflowLeft)
                  .arg(report.overflowRight)
            : QStringLiteral("Imported cleanly within 16-band DSP limit.");
  }

  if (report.truncated) {
    const QString warnMsg =
        QString("Listening Correction preset exceeds 16 bands/ch (%1L/%2R); excess preserved in overflow storage.")
            .arg(report.requestedLeft)
            .arg(report.requestedRight);
    log::LogService::instance().log(
        QStringLiteral("PresetManagement"), QStringLiteral("ImportListeningPeq"),
        QStringLiteral("Warning"), warnMsg, QStringLiteral("Preset"),
        presetLabel.isEmpty() ? QStringLiteral("Custom") : presetLabel,
        QStringLiteral("{}"), QString(), QString(),
        log::ErrorCode::PresetTruncatedWarn, 0.0);
    emit presetImportWarning(
        QStringLiteral("ListeningCorrection"), static_cast<int>(report.requestedLeft),
        static_cast<int>(report.requestedRight),
        static_cast<int>(ParametricEQ::BANDS_PER_CHANNEL),
        static_cast<int>(report.overflowLeft),
        static_cast<int>(report.overflowRight), warnMsg);
  }

  m_listeningPresetName = presetLabel.isEmpty() ? QStringLiteral("Custom") : presetLabel;
  emit listeningPresetChanged(m_listeningPresetName);
  emit peqParametersChanged();
  emit diagnosticsChanged();
  return true;
}

bool EngineController::applyExternalInsertJson(const QJsonObject &obj) {
  QString client = obj["client_name"].toString();
  QString sendL = obj["send_port_l"].toString();
  QString sendR = obj["send_port_r"].toString();
  QString retL = obj["return_port_l"].toString();
  QString retR = obj["return_port_r"].toString();
  double sRate = obj["sample_rate"].toDouble(48000.0);
  int bSize = obj["buffer_size"].toInt(256);
  int repLat = obj["reported_latency_frames"].toInt(0);
  bool latRep = obj.contains("latency_reported")
                    ? obj["latency_reported"].toBool(true)
                    : true;
  int delayComp = obj["delay_compensation_frames"].toInt(0);
  QString pluginName = obj["plugin_name"].toString();
  QString pluginUri = obj["plugin_uri"].toString();
  QString hostPresetRef = obj["host_preset_reference"].toString();

  bool ok = true;
  if (!client.isEmpty()) {
    ok = configureExternalInsertWithMetadata(
        client, sendL, sendR, retL, retR, sRate, bSize, repLat, latRep,
        pluginName, pluginUri, hostPresetRef);
    if (ok) {
      setInsertDelayCompensation(delayComp);
    }
  }
  if (obj.contains("insert_position")) {
    setExternalInsertPosition(obj["insert_position"].toInt(4));
  }
  if (obj.contains("disconnect_policy")) {
    setExternalInsertDisconnectPolicy(obj["disconnect_policy"].toInt(0));
  }
  return ok;
}

bool EngineController::applyMasterDspJson(const QJsonObject &obj) {
  if (obj.contains("geq")) {
    const QJsonObject geq = obj["geq"].toObject();
    const QJsonArray gains = geq["gains"].toArray();
    for (int b = 0; b < std::min<int>(static_cast<int>(gains.size()), 32);
         ++b) {
      setGeqBandGain(b, gains[b].toDouble(0.0));
    }
    if (geq.contains("band_enabled")) {
      const QJsonArray enabledArr = geq["band_enabled"].toArray();
      for (int b = 0;
           b < std::min<int>(static_cast<int>(enabledArr.size()), 32); ++b) {
        setGeqBandEnabled(b, enabledArr[b].toBool(true));
      }
    }
    if (geq.contains("enabled")) {
      setDspNodeBypass("GEQ", !geq["enabled"].toBool(false));
    } else if (geq.contains("bypassed")) {
      setDspNodeBypass("GEQ", geq["bypassed"].toBool(true));
    }
  }
  if (obj.contains("peq")) {
    const QJsonObject peq = obj["peq"].toObject();
    std::vector<ParametricEQ::BandConfig> leftVec = m_uiPeq.exportBands(false);
    leftVec.resize(ParametricEQ::BANDS_PER_CHANNEL);
    std::vector<ParametricEQ::BandConfig> rightVec = m_uiPeq.exportBands(true);
    rightVec.resize(ParametricEQ::BANDS_PER_CHANNEL);

    auto collectChannelBands = [&](const QJsonArray &arr,
                                   std::vector<ParametricEQ::BandConfig> &out) {
      if (static_cast<size_t>(arr.size()) > out.size()) {
        out.resize(static_cast<size_t>(arr.size()));
      }
      for (int i = 0; i < arr.size(); ++i) {
        const QJsonObject bo = arr[i].toObject();
        const int idx = bo.contains("index") ? bo["index"].toInt(i) : i;
        if (idx < 0)
          continue;
        if (static_cast<size_t>(idx) >= out.size()) {
          out.resize(static_cast<size_t>(idx) + 1);
        }
        auto &cfg = out[static_cast<size_t>(idx)];
        cfg.frequency = bo["frequency"].toDouble(1000.0);
        cfg.gainDb = bo.contains("gain_db")
                         ? bo["gain_db"].toDouble(0.0)
                         : bo["gainDb"].toDouble(0.0);
        cfg.Q = bo.contains("q") ? bo["q"].toDouble(1.0) : bo["Q"].toDouble(1.0);
        cfg.type = static_cast<BiquadFilterType>(
            std::clamp(bo["type"].toInt(0), 0, 5));
        cfg.enabled =
            bo.contains("enabled") ? bo["enabled"].toBool(true) : true;
      }
    };

    if (peq.contains("left_bands"))
      collectChannelBands(peq["left_bands"].toArray(), leftVec);
    else if (peq.contains("leftBands"))
      collectChannelBands(peq["leftBands"].toArray(), leftVec);
    if (peq.contains("right_bands"))
      collectChannelBands(peq["right_bands"].toArray(), rightVec);
    else if (peq.contains("rightBands"))
      collectChannelBands(peq["rightBands"].toArray(), rightVec);

    const auto report = m_uiPeq.importBandsWithReport(leftVec, rightVec);
    for (size_t b = 0; b < ParametricEQ::BANDS_PER_CHANNEL; ++b) {
      const auto &lb = m_uiPeq.leftBand(b);
      setPeqBand(false, static_cast<int>(b), lb.frequency, lb.gainDb, lb.Q,
                 static_cast<int>(lb.type), lb.enabled);
      const auto &rb = m_uiPeq.rightBand(b);
      setPeqBand(true, static_cast<int>(b), rb.frequency, rb.gainDb, rb.Q,
                 static_cast<int>(rb.type), rb.enabled);
    }

    {
      std::lock_guard<std::mutex> lock(m_presetReportMutex);
      m_lastPresetImportReport.clear();
      m_lastPresetImportReport["target"] = QStringLiteral("MasterPEQ");
      m_lastPresetImportReport["requestedLeftBands"] = static_cast<int>(report.requestedLeft);
      m_lastPresetImportReport["requestedRightBands"] = static_cast<int>(report.requestedRight);
      m_lastPresetImportReport["activeLimitPerChannel"] =
          static_cast<int>(ParametricEQ::BANDS_PER_CHANNEL);
      m_lastPresetImportReport["overflowLeftBands"] = static_cast<int>(report.overflowLeft);
      m_lastPresetImportReport["overflowRightBands"] = static_cast<int>(report.overflowRight);
      m_lastPresetImportReport["truncated"] = report.truncated;
      m_lastPresetImportReport["message"] =
          report.truncated
              ? QString("Master PEQ imported %1L/%2R bands: 16 active bands applied, %3L/%4R excess bands retained in variable overflow vector.")
                    .arg(report.requestedLeft)
                    .arg(report.requestedRight)
                    .arg(report.overflowLeft)
                    .arg(report.overflowRight)
              : QStringLiteral("Imported cleanly within 16-band DSP limit.");
    }

    if (report.truncated) {
      const QString warnMsg =
          QString("Master PEQ preset exceeds 16 bands/ch (%1L/%2R); excess retained in variable overflow vector.")
              .arg(report.requestedLeft)
              .arg(report.requestedRight);
      log::LogService::instance().log(
          QStringLiteral("PresetManagement"), QStringLiteral("ImportMasterPeq"),
          QStringLiteral("Warning"), warnMsg, QStringLiteral("Preset"),
          QStringLiteral("MasterPEQ"), QStringLiteral("{}"), QString(), QString(),
          log::ErrorCode::PresetTruncatedWarn, 0.0);
      emit presetImportWarning(
          QStringLiteral("MasterPEQ"), static_cast<int>(report.requestedLeft),
          static_cast<int>(report.requestedRight),
          static_cast<int>(ParametricEQ::BANDS_PER_CHANNEL),
          static_cast<int>(report.overflowLeft),
          static_cast<int>(report.overflowRight), warnMsg);
    }

    if (peq.contains("enabled")) {
      setDspNodeBypass("PEQ", !peq["enabled"].toBool(false));
    } else if (peq.contains("bypassed")) {
      setDspNodeBypass("PEQ", peq["bypassed"].toBool(true));
    }
  }
  if (obj.contains("compressor")) {
    const QJsonObject comp = obj["compressor"].toObject();
    setCompressorParams(
        comp.contains("threshold_db") ? comp["threshold_db"].toDouble(-12.0)
                                      : comp["thresholdDb"].toDouble(-12.0),
        comp["ratio"].toDouble(2.0),
        comp.contains("attack_ms") ? comp["attack_ms"].toDouble(20.0)
                                   : comp["attackMs"].toDouble(20.0),
        comp.contains("release_ms") ? comp["release_ms"].toDouble(100.0)
                                    : comp["releaseMs"].toDouble(100.0),
        comp.contains("makeup_db") ? comp["makeup_db"].toDouble(0.0)
                                   : comp["makeupGainDb"].toDouble(0.0),
        comp.contains("knee_db") ? comp["knee_db"].toDouble(3.0)
                                 : comp["kneeDb"].toDouble(3.0));
    if (comp.contains("enabled")) {
      setDspNodeBypass("Compressor", !comp["enabled"].toBool());
    } else if (comp.contains("bypassed")) {
      setDspNodeBypass("Compressor", comp["bypassed"].toBool(true));
    }
  }
  if (obj.contains("limiter")) {
    const QJsonObject lim = obj["limiter"].toObject();
    setLimiterParams(
        lim.contains("ceiling_db") ? lim["ceiling_db"].toDouble(-0.1)
                                   : lim["ceilingDb"].toDouble(-0.1),
        lim.contains("release_ms") ? lim["release_ms"].toDouble(50.0)
                                   : lim["releaseMs"].toDouble(50.0),
        lim.contains("lookahead_ms") ? lim["lookahead_ms"].toDouble(1.0)
                                     : lim["lookaheadMs"].toDouble(1.0));
    if (lim.contains("enabled")) {
      setDspNodeBypass("Limiter", !lim["enabled"].toBool());
    } else if (lim.contains("bypassed")) {
      setDspNodeBypass("Limiter", lim["bypassed"].toBool(true));
    }
  }
  if (obj.contains("reverb")) {
    const QJsonObject rev = obj["reverb"].toObject();
    setReverbParams(
        rev.contains("room_size") ? rev["room_size"].toDouble(0.7)
                                  : rev["roomSize"].toDouble(0.7),
        rev.contains("damping") ? rev["damping"].toDouble(0.3)
                                : rev["damping"].toDouble(0.3),
        rev.contains("wet_level") ? rev["wet_level"].toDouble(0.2)
                                  : rev["wetLevel"].toDouble(0.2),
        rev.contains("predelay_ms") ? rev["predelay_ms"].toDouble(0.0)
                                    : rev["predelayMs"].toDouble(0.0),
        rev.contains("tail_preserving") ? rev["tail_preserving"].toBool(true)
                                        : rev["tailPreserving"].toBool(true));
    if (rev.contains("enabled")) {
      setDspNodeBypass("Reverb", !rev["enabled"].toBool());
    } else if (rev.contains("bypassed")) {
      setDspNodeBypass("Reverb", rev["bypassed"].toBool(true));
    }
  }
  if (obj.contains("filter")) {
    const QJsonObject flt = obj["filter"].toObject();
    setFilterParams(
        flt.contains("hpf_frequency") ? flt["hpf_frequency"].toDouble(20.0)
                                      : flt["hpfFrequency"].toDouble(20.0),
        flt.contains("hpf_enabled") ? flt["hpf_enabled"].toBool(false)
                                    : flt["hpfEnabled"].toBool(false),
        flt.contains("hpf_slope") ? flt["hpf_slope"].toInt(0)
                                  : flt["hpfSlope"].toInt(0),
        flt.contains("lpf_frequency") ? flt["lpf_frequency"].toDouble(20000.0)
                                      : flt["lpfFrequency"].toDouble(20000.0),
        flt.contains("lpf_enabled") ? flt["lpf_enabled"].toBool(false)
                                    : flt["lpfEnabled"].toBool(false),
        flt.contains("lpf_slope") ? flt["lpf_slope"].toInt(0)
                                  : flt["lpfSlope"].toInt(0),
        flt.contains("hpf_q") ? flt["hpf_q"].toDouble(0.70710678)
                              : flt["hpfQ"].toDouble(0.70710678),
        flt.contains("lpf_q") ? flt["lpf_q"].toDouble(0.70710678)
                              : flt["lpfQ"].toDouble(0.70710678));
    if (flt.contains("enabled")) {
      setDspNodeBypass("Filter", !flt["enabled"].toBool(false));
    } else if (flt.contains("bypassed")) {
      setDspNodeBypass("Filter", flt["bypassed"].toBool(true));
    }
  }
  return true;
}

bool EngineController::applyAudioDeviceJson(const QJsonObject &obj) {
  if (obj.contains("fixed_graph_sample_rate")) {
    setFixedGraphSampleRate(obj["fixed_graph_sample_rate"].toDouble(48000.0));
  } else if (obj.contains("graph_sample_rate")) {
    setFixedGraphSampleRate(obj["graph_sample_rate"].toDouble(48000.0));
  } else if (obj.contains("sample_rate")) {
    setFixedGraphSampleRate(obj["sample_rate"].toDouble(48000.0));
  }
  if (obj.contains("direct_mode")) {
    setDirectModeEnabled(obj["direct_mode"].toBool(false));
  }
  if (obj.contains("output_routing_mode")) {
    setOutputRoutingMode(obj["output_routing_mode"].toInt(0));
  }
  if (obj.contains("listening_volume")) {
    setListeningVolume(obj["listening_volume"].toDouble(1.0));
  }
  if (obj.contains("render_monitor_volume")) {
    setRenderMonitorVolume(obj["render_monitor_volume"].toDouble(1.0));
  }
  return true;
}

bool EngineController::applyPresetProfile(const QString &category,
                                          const QString &presetName) {
  QJsonObject obj = m_presetManager.loadPreset(category, presetName);
  if (obj.isEmpty()) {
    QJsonObject det;
    det.insert(QStringLiteral("preset_id"), presetName);
    det.insert(QStringLiteral("category"), category);
    det.insert(QStringLiteral("stage"), QStringLiteral("preset_load"));
    log::LogService::instance().log(
        "PresetManagement", "ApplyPreset", "Failed",
        QString("Preset '%1' not found in category '%2'.")
            .arg(presetName, category),
        category, presetName,
        QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)),
        QString(), QString(), log::ErrorCode::PresetNotFound, 0.0);
    return false;
  }

  if (category == engine::preset::PresetCategories::ListeningCorrection) {
    if (m_presetManager.isFactoryPreset(category, presetName)) {
      setListeningPreset(presetName);
      return true;
    }
    const bool ok = applyListeningCorrectionJson(obj, presetName);
    if (ok) {
      log::LogService::instance().log(
          "ListeningCorrection", "ApplyPreset", "Success",
          "Applied user Listening Correction preset: " + presetName, "Preset",
          presetName);
    }
    emit presetManagementChanged();
    return ok;
  }

  if (category == engine::preset::PresetCategories::ExternalInsert) {
    return applyExternalInsertJson(obj);
  }

  if (category == engine::preset::PresetCategories::AudioDevice) {
    const bool ok = applyAudioDeviceJson(obj);
    if (ok) {
      log::LogService::instance().log(
          "PresetManagement", "ApplyPreset", "Success",
          QString("Applied Audio Device preset '%1'.").arg(presetName), category,
          presetName);
      emit presetManagementChanged();
    }
    return ok;
  }

  if (category == engine::preset::PresetCategories::MasterDsp) {
    applyMasterDspJson(obj);
    QJsonObject snapDet;
    snapDet.insert(QStringLiteral("preset_id"), presetName);
    snapDet.insert(QStringLiteral("category"), category);
    snapDet.insert(QStringLiteral("snapshot"), obj);
    log::LogService::instance().log(
        "PresetManagement", "ApplyPreset", "Success",
        QString("Applied Master DSP preset '%1'.").arg(presetName), category,
        presetName,
        QString::fromUtf8(
            QJsonDocument(snapDet).toJson(QJsonDocument::Compact)));
    emit presetManagementChanged();
    return true;
  }

  return false;
}

QVariantMap
EngineController::captureCurrentStateAsPreset(const QString &category) {
  QJsonObject obj;
  obj["category"] = category;
  obj["format_version"] = 1;

  if (category == engine::preset::PresetCategories::ListeningCorrection) {
    auto serializeListening = [&](bool isRight) {
      QJsonArray arr;
      const auto allBands = m_uiListeningCorrection.exportBands(isRight);
      for (size_t i = 0; i < allBands.size(); ++i) {
        const auto &b = allBands[i];
        QJsonObject bo;
        bo["index"] = static_cast<int>(i);
        bo["frequency"] = b.frequency;
        bo["gain_db"] = b.gainDb;
        bo["q"] = b.Q;
        bo["type"] = static_cast<int>(b.type);
        bo["enabled"] = b.enabled;
        arr.append(bo);
      }
      return arr;
    };
    const QJsonArray leftArr = serializeListening(false);
    obj["left_bands"] = leftArr;
    obj["right_bands"] = serializeListening(true);
    obj["bands"] = leftArr;
    obj["preset_name"] = m_listeningPresetName;
  } else if (category == engine::preset::PresetCategories::ExternalInsert) {
    if (m_insertNode) {
      const auto &cfg = m_insertNode->bridge().config();
      obj["client_name"] = QString::fromStdString(cfg.clientName);
      obj["send_port_l"] = QString::fromStdString(cfg.sendPortL);
      obj["send_port_r"] = QString::fromStdString(cfg.sendPortR);
      obj["return_port_l"] = QString::fromStdString(cfg.returnPortL);
      obj["return_port_r"] = QString::fromStdString(cfg.returnPortR);
      obj["sample_rate"] = cfg.sampleRate;
      obj["buffer_size"] = static_cast<int>(cfg.bufferSize);
      obj["reported_latency_frames"] =
          static_cast<int>(m_insertNode->bridge().reportedLatencyFrames());
      obj["latency_reported"] = m_insertNode->bridge().hasReportedLatency();
      obj["delay_compensation_frames"] =
          static_cast<int>(m_insertNode->delayBuffer().delayFrames());
      obj["plugin_name"] = QString::fromStdString(cfg.pluginName);
      obj["plugin_uri"] = QString::fromStdString(cfg.pluginUri);
      obj["host_preset_reference"] =
          QString::fromStdString(cfg.hostPresetReference);
      obj["insert_position"] =
          m_externalInsertPosition.load(std::memory_order_relaxed);
      obj["disconnect_policy"] =
          static_cast<int>(m_insertNode->bridge().disconnectPolicy());
    }
  } else if (category == engine::preset::PresetCategories::AudioDevice) {
    obj["backend"] = QStringLiteral("PipeWire");
    obj["sample_rate"] = graphSampleRate();
    obj["graph_sample_rate"] = graphSampleRate();
    obj["fixed_graph_sample_rate"] =
        m_fixedGraphSampleRate.load(std::memory_order_relaxed);
    obj["buffer_size"] =
        m_audioOutput ? static_cast<int>(m_audioOutput->diagnostics().bufferFrames)
                      : 256;
    obj["direct_mode"] =
        m_directModeEnabled.load(std::memory_order_relaxed);
    obj["output_routing_mode"] =
        m_outputRoutingMode.load(std::memory_order_relaxed);
    obj["listening_volume"] = listeningVolume();
    obj["render_monitor_volume"] = renderMonitorVolume();
  } else if (category == engine::preset::PresetCategories::MasterDsp) {
    {
      QJsonObject geq;
      geq["enabled"] = !m_geqBypassed.load(std::memory_order_relaxed);
      QJsonArray gains;
      QJsonArray bandEnabled;
      for (int b = 0; b < 32; ++b) {
        gains.append(m_uiGeq.bandGain(static_cast<size_t>(b)));
        bandEnabled.append(m_uiGeq.isBandEnabled(static_cast<size_t>(b)));
      }
      geq["gains"] = gains;
      geq["band_enabled"] = bandEnabled;
      obj["geq"] = geq;
    }
    {
      QJsonObject peq;
      peq["enabled"] = !m_peqBypassed.load(std::memory_order_relaxed);
      auto serializeChannel = [&](bool isRight) {
        QJsonArray arr;
        const auto allBands = m_uiPeq.exportBands(isRight);
        for (size_t b = 0; b < allBands.size(); ++b) {
          const auto &band = allBands[b];
          QJsonObject bo;
          bo["index"] = static_cast<int>(b);
          bo["frequency"] = band.frequency;
          bo["gain_db"] = band.gainDb;
          bo["q"] = band.Q;
          bo["type"] = static_cast<int>(band.type);
          bo["enabled"] = band.enabled;
          arr.append(bo);
        }
        return arr;
      };
      peq["left_bands"] = serializeChannel(false);
      peq["right_bands"] = serializeChannel(true);
      obj["peq"] = peq;
    }
    {
      QJsonObject comp;
      comp["enabled"] = !m_compBypassed.load(std::memory_order_relaxed);
      comp["threshold_db"] = m_uiComp.thresholdDb();
      comp["ratio"] = m_uiComp.ratio();
      comp["attack_ms"] = m_uiComp.attackMs();
      comp["release_ms"] = m_uiComp.releaseMs();
      comp["makeup_db"] = m_uiComp.makeupGainDb();
      comp["knee_db"] = m_uiComp.kneeDb();
      obj["compressor"] = comp;
    }
    {
      QJsonObject lim;
      lim["enabled"] = !m_limiterBypassed.load(std::memory_order_relaxed);
      lim["ceiling_db"] = m_uiLimiter.ceilingDb();
      lim["release_ms"] = m_uiLimiter.releaseMs();
      lim["lookahead_ms"] = m_uiLimiter.lookaheadMs();
      obj["limiter"] = lim;
    }
    {
      QJsonObject flt;
      flt["enabled"] = !m_filterBypassed.load(std::memory_order_relaxed);
      flt["hpf_enabled"] = m_uiFilter.isHpfEnabled();
      flt["hpf_frequency"] = m_uiFilter.hpfFrequency();
      flt["hpf_slope"] =
          m_uiFilter.hpfSlope() == HighLowPassFilter::FilterSlope::Slope24dB
              ? 1
              : 0;
      flt["hpf_q"] = m_uiFilter.hpfQ();
      flt["lpf_enabled"] = m_uiFilter.isLpfEnabled();
      flt["lpf_frequency"] = m_uiFilter.lpfFrequency();
      flt["lpf_slope"] =
          m_uiFilter.lpfSlope() == HighLowPassFilter::FilterSlope::Slope24dB
              ? 1
              : 0;
      flt["lpf_q"] = m_uiFilter.lpfQ();
      obj["filter"] = flt;
    }
    {
      QJsonObject rev;
      rev["enabled"] = !m_reverbBypassed.load(std::memory_order_relaxed);
      rev["room_size"] = m_uiReverb.roomSize();
      rev["damping"] = m_uiReverb.damping();
      rev["wet_level"] = m_uiReverb.wetLevel();
      rev["predelay_ms"] = m_uiReverb.predelayMs();
      rev["tail_preserving"] = m_uiReverb.isTailPreserving();
      obj["reverb"] = rev;
    }
  }

  return obj.toVariantMap();
}

// =========================================================================
// Phase 4: Export & Render Operations (Section 5.11 & 20.1)
// =========================================================================

std::atomic<bool> *EngineController::renderNodeFlagPtr(const QString &nodeName) noexcept {
  if (nodeName == "PEQ" || nodeName == "Master PEQ") return &m_renderIncludePeq;
  if (nodeName == "GEQ" || nodeName == "Graphic EQ") return &m_renderIncludeGeq;
  if (nodeName == "Filter" || nodeName == "HPF / LPF") return &m_renderIncludeFilter;
  if (nodeName == "Compressor") return &m_renderIncludeCompressor;
  if (nodeName == "Reverb") return &m_renderIncludeReverb;
  if (nodeName == "Limiter" || nodeName == "Peak Limiter") return &m_renderIncludeLimiter;
  return nullptr;
}

const std::atomic<bool> *EngineController::renderNodeFlagPtr(const QString &nodeName) const noexcept {
  if (nodeName == "PEQ" || nodeName == "Master PEQ") return &m_renderIncludePeq;
  if (nodeName == "GEQ" || nodeName == "Graphic EQ") return &m_renderIncludeGeq;
  if (nodeName == "Filter" || nodeName == "HPF / LPF") return &m_renderIncludeFilter;
  if (nodeName == "Compressor") return &m_renderIncludeCompressor;
  if (nodeName == "Reverb") return &m_renderIncludeReverb;
  if (nodeName == "Limiter" || nodeName == "Peak Limiter") return &m_renderIncludeLimiter;
  return nullptr;
}

void EngineController::setRenderNodeEnabled(const QString &nodeName, bool enabled) {
  if (auto *ptr = renderNodeFlagPtr(nodeName)) {
    ptr->store(enabled, std::memory_order_relaxed);
    emit renderStateChanged();
  }
}

bool EngineController::isRenderNodeEnabled(const QString &nodeName) const {
  if (const auto *ptr = renderNodeFlagPtr(nodeName)) {
    return ptr->load(std::memory_order_relaxed);
  }
  return false;
}

void EngineController::setSyncMasterDspToRender(bool sync) {
  m_syncMasterDspToRender.store(sync, std::memory_order_relaxed);
  if (sync) {
    syncMasterDspToRenderEngine();
  }
  emit renderStateChanged();
}

bool EngineController::syncMasterDspToRenderEnabled() const noexcept {
  return m_syncMasterDspToRender.load(std::memory_order_relaxed);
}

QVariantMap EngineController::verifyRenderTapConsistency() {
  syncMasterDspToRenderEngine();
  const double sr = graphSampleRate() > 0.0 ? graphSampleRate() : 48000.0;
  constexpr size_t N = 512;
  std::vector<double> srcL(N), srcR(N);
  for (size_t i = 0; i < N; ++i) {
    const double t = static_cast<double>(i) / sr;
    srcL[i] = 0.25 * std::sin(2.0 * std::numbers::pi * 440.0 * t);
    srcR[i] = 0.25 * std::cos(2.0 * std::numbers::pi * 440.0 * t);
  }

  // 1. Offline RenderChain baseline
  engine::render::RenderDspSnapshot snap;
  {
    auto lock = m_renderEngine.lockTemplateChain();
    snap = m_renderEngine.templateChain().createSnapshot();
  }
  engine::render::RenderChain renderChain;
  renderChain.applySnapshot(snap, sr);
  std::vector<double> rndL = srcL, rndR = srcR;
  renderChain.process(rndL.data(), rndR.data(), N);

  // 2. Real cloned AudioPipeline Master Chain (honoring Global Bypass, skipping External Insert per §6.6)
  std::vector<double> liveL = srcL, liveR = srcR;
  auto liveChain = m_pipeline ? m_pipeline->cloneMasterChainForVerification() : nullptr;
  bool usedRealPipelineClone = false;
  if (liveChain) {
    usedRealPipelineClone = true;
    const bool hasPendingControlUpdates =
        !m_pipeline->paramQueue().empty() ||
        m_pipeline->paramMailbox().pendingUpdatesCount() > 0;

    for (size_t i = 0; i < liveChain->nodeCount(); ++i) {
      auto *node = liveChain->node(i);
      if (!node)
        continue;
      if (auto *peq = dynamic_cast<engine::pipeline::ParametricEqNode *>(node)) {
        peq->reset();
        if (hasPendingControlUpdates) {
          peq->setBypassed(m_peqBypassed.load(std::memory_order_relaxed));
          std::array<BiquadCoefficients, ParametricEQ::BANDS_PER_CHANNEL> lc{}, rc{};
          std::array<bool, ParametricEQ::BANDS_PER_CHANNEL> le{}, re{};
          for (size_t b = 0; b < ParametricEQ::BANDS_PER_CHANNEL; ++b) {
            const auto &lb = m_uiPeq.leftBand(b);
            lc[b] = BiquadFilter::calculateCoefficients(
                lb.type, sr, std::clamp(lb.frequency, 10.0, std::min(24000.0, sr * 0.48)),
                std::clamp(lb.Q, 0.1, 20.0), std::clamp(lb.gainDb, -24.0, 24.0));
            le[b] = lb.enabled;
            const auto &rb = m_uiPeq.rightBand(b);
            rc[b] = BiquadFilter::calculateCoefficients(
                rb.type, sr, std::clamp(rb.frequency, 10.0, std::min(24000.0, sr * 0.48)),
                std::clamp(rb.Q, 0.1, 20.0), std::clamp(rb.gainDb, -24.0, 24.0));
            re[b] = rb.enabled;
          }
          peq->applyCoefficients(lc, rc, le, re, 0);
        }
      } else if (auto *geq = dynamic_cast<engine::pipeline::GraphicEqNode *>(node)) {
        geq->reset();
        if (hasPendingControlUpdates) {
          geq->setBypassed(m_geqBypassed.load(std::memory_order_relaxed));
          const auto &p = m_pipeline->paramMailbox().currentParameters();
          geq->applyAllCoefficients(p.geqCoefficients, 0);
          geq->applyBandEnabled(p.geqBandEnabled);
        }
      } else if (auto *flt = dynamic_cast<engine::pipeline::HighLowPassNode *>(node)) {
        flt->resetFilterStateOnly();
        if (hasPendingControlUpdates) {
          flt->setBypassed(m_filterBypassed.load(std::memory_order_relaxed));
          const auto &p = m_pipeline->paramMailbox().currentParameters();
          flt->applyCoefficients(p.filterCoefficients, 0);
        }
      } else if (auto *comp = dynamic_cast<engine::pipeline::CompressorNode *>(node)) {
        comp->reset();
        if (hasPendingControlUpdates) {
          comp->setBypassed(m_compBypassed.load(std::memory_order_relaxed));
          comp->prepareInstance(sr, m_uiComp.thresholdDb(), m_uiComp.ratio(),
                                m_uiComp.attackMs(), m_uiComp.releaseMs(),
                                m_uiComp.makeupGainDb(), m_uiComp.kneeDb(), true);
        }
      } else if (auto *rev = dynamic_cast<engine::pipeline::ReverbNode *>(node)) {
        rev->reset();
        if (hasPendingControlUpdates) {
          rev->setBypassed(m_reverbBypassed.load(std::memory_order_relaxed));
          rev->prepareInstance(sr, m_uiReverb.roomSize(), m_uiReverb.damping(),
                               m_uiReverb.wetLevel(), m_uiReverb.predelayMs(),
                               m_uiReverb.isTailPreserving(), true);
        }
      } else if (auto *lim = dynamic_cast<engine::pipeline::PeakLimiterNode *>(node)) {
        lim->reset();
        if (hasPendingControlUpdates) {
          lim->setBypassed(m_limiterBypassed.load(std::memory_order_relaxed));
          lim->prepareInstance(sr, m_uiLimiter.ceilingDb(), m_uiLimiter.releaseMs(),
                               m_uiLimiter.lookaheadMs(), true);
        }
      }
    }

    if (!m_globalDspBypass.load(std::memory_order_relaxed)) {
      for (size_t i = 0; i < liveChain->nodeCount(); ++i) {
        auto *node = liveChain->node(i);
        if (!node || dynamic_cast<engine::insert::ExternalInsertNode *>(node) != nullptr) {
          continue; // §6.6 External Insert is strictly excluded from offline render equivalence
        }
        if (!node->isBypassed()) {
          node->process(liveL.data(), liveR.data(), N);
        }
      }
    }
  } else {
    engine::render::RenderChain mirrorChain;
    mirrorChain.configure(sr);
    mirrorChain.setEnabled(!m_globalDspBypass.load(std::memory_order_relaxed));
    mirrorChain.copyParametersFrom(m_uiPeq, m_uiGeq, m_uiFilter, m_uiComp,
                                   m_uiReverb, m_uiLimiter);
    mirrorChain.peq().setEnabled(!m_peqBypassed.load(std::memory_order_relaxed));
    mirrorChain.geq().setEnabled(!m_geqBypassed.load(std::memory_order_relaxed));
    const bool fltOn = !m_filterBypassed.load(std::memory_order_relaxed);
    mirrorChain.filter().setHpfEnabled(fltOn && m_uiFilter.isHpfEnabled());
    mirrorChain.filter().setLpfEnabled(fltOn && m_uiFilter.isLpfEnabled());
    mirrorChain.compressor().setEnabled(!m_compBypassed.load(std::memory_order_relaxed));
    mirrorChain.reverb().setEnabled(!m_reverbBypassed.load(std::memory_order_relaxed));
    mirrorChain.limiter().setEnabled(!m_limiterBypassed.load(std::memory_order_relaxed));
    mirrorChain.process(liveL.data(), liveR.data(), N);
  }

  double maxDiff = 0.0;
  if (!m_globalDspBypass.load(std::memory_order_relaxed)) {
    for (size_t i = 0; i < N; ++i) {
      maxDiff = std::max(maxDiff, std::abs(rndL[i] - liveL[i]));
      maxDiff = std::max(maxDiff, std::abs(rndR[i] - liveR[i]));
    }
  }

  // Verify Listening Correction has zero leakage through RenderMonitorPath
  engine::routing::OutputPathRouter router;
  router.setSampleRate(sr);
  for (size_t b = 0; b < ParametricEQ::BANDS_PER_CHANNEL; ++b) {
    router.listeningCorrection().setLeftBand(b, m_uiListeningCorrection.leftBand(b));
    router.listeningCorrection().setRightBand(b, m_uiListeningCorrection.rightBand(b));
  }
  router.setRenderMonitorVolume(1.0);
  router.setMode(engine::routing::OutputPathMode::RenderMonitorPath);
  std::vector<double> interleavedIn(N * 2), interleavedOut(N * 2);
  for (size_t i = 0; i < N; ++i) {
    interleavedIn[i * 2] = rndL[i];
    interleavedIn[i * 2 + 1] = rndR[i];
  }
  router.routeBlock(interleavedIn.data(), interleavedOut.data(), N);
  double leakage = 0.0;
  for (size_t i = 0; i < N * 2; ++i) {
    leakage = std::max(leakage, std::abs(interleavedIn[i] - interleavedOut[i]));
  }

  QVariantMap res;
  res["consistent"] = (maxDiff <= 1e-12 && leakage == 0.0);
  res["maxAbsDiff"] = maxDiff;
  res["listeningLeakage"] = leakage;
  res["globalDspBypass"] = m_globalDspBypass.load(std::memory_order_relaxed);
  res["usedRealPipelineClone"] = usedRealPipelineClone;
  res["externalInsertExcludedFromRender"] = snap.externalInsertWasActiveInLive;
  return res;
}

void EngineController::syncMasterDspToRenderEngine() {
  auto lock = m_renderEngine.lockTemplateChain();
  auto &tc = m_renderEngine.templateChain();
  tc.copyParametersFrom(m_uiPeq, m_uiGeq, m_uiFilter, m_uiComp, m_uiReverb,
                        m_uiLimiter);

  const bool peqOn = !m_peqBypassed.load(std::memory_order_relaxed);
  const bool geqOn = !m_geqBypassed.load(std::memory_order_relaxed);
  const bool fltOn = !m_filterBypassed.load(std::memory_order_relaxed);
  const bool compOn = !m_compBypassed.load(std::memory_order_relaxed);
  const bool revOn = !m_reverbBypassed.load(std::memory_order_relaxed);
  const bool limOn = !m_limiterBypassed.load(std::memory_order_relaxed);

  tc.peq().setEnabled(peqOn);
  tc.geq().setEnabled(geqOn);
  tc.filter().setHpfEnabled(fltOn && m_uiFilter.isHpfEnabled());
  tc.filter().setLpfEnabled(fltOn && m_uiFilter.isLpfEnabled());
  tc.compressor().setEnabled(compOn);
  tc.reverb().setEnabled(revOn);
  tc.limiter().setEnabled(limOn);

  const bool insertActive =
      m_insertNode && !m_insertBypassed.load(std::memory_order_relaxed) &&
      !m_insertNode->isBypassed();
  const std::string insertName =
      m_insertNode ? m_insertNode->bridge().config().clientName : std::string{};
  tc.setExternalInsertLiveStatus(insertActive, insertName);
}

QString EngineController::startTrackRender(const QString &deckId, int bitDepth,
                                           int rateMode, double customRate,
                                           bool applyDither, bool enableDsp) {
  if (!m_deckManager || deckId.isEmpty())
    return QString();
  auto &wsMgr = engine::workspace::WorkspaceManager::instance();
  if (!wsMgr.hasOpenWorkspace()) {
    log::LogService::instance().log(
        "Render", "QueueTrackRender", "Failed",
        QStringLiteral("Render aborted: no dedicated workspace is currently "
                       "open (fail-closed)."),
        "Workspace", QString(),
        QStringLiteral("{\"stage\":\"workspace_check\"}"), QString(), QString(),
        log::ErrorCode::WorkspaceNotOpenFailClosed, 0.0);
    return QString();
  }

  char id = deckId.toUpper().at(0).toLatin1();
  auto *d = m_deckManager->deck(id);
  if (!d || d->trackInfo().path.empty()) {
    qWarning() << "EngineController: deck" << deckId
               << "has no track loaded for render.";
    return QString();
  }

  QString trackPath = QString::fromStdString(d->trackInfo().path);
  if (wsMgr.isPathBlockedByLockedWorkspace(trackPath)) {
    log::LogService::instance().log(
        "Render", "QueueTrackRender", "Failed",
        QString("Render blocked: source path resides in locked workspace (%1).")
            .arg(trackPath),
        "Track", trackPath,
        QStringLiteral("{\"stage\":\"workspace_lock_check\"}"), QString(),
        QString(), log::ErrorCode::WorkspaceLockedAccessDenied, 0.0);
    return QString();
  }

  m_renderEngine.setWorkspacePath(wsMgr.openWorkspacePath());
  if (m_syncMasterDspToRender.load(std::memory_order_relaxed)) {
    syncMasterDspToRenderEngine();
  }

  engine::render::RenderSettings settings;
  settings.bitDepth =
      (bitDepth == 0)
          ? engine::render::RenderBitDepth::Int16
          : ((bitDepth == 2) ? engine::render::RenderBitDepth::Float32
                             : engine::render::RenderBitDepth::Int24);
  settings.rateMode = (rateMode == 1)
                          ? engine::render::RenderSampleRateMode::Custom
                          : engine::render::RenderSampleRateMode::Original;
  settings.targetSampleRate = customRate > 0 ? customRate : 48000.0;
  settings.applyDither = applyDither;
  settings.enableMasterDsp = enableDsp;
  settings.includePeq = m_renderIncludePeq.load(std::memory_order_relaxed);
  settings.includeGeq = m_renderIncludeGeq.load(std::memory_order_relaxed);
  settings.includeFilter = m_renderIncludeFilter.load(std::memory_order_relaxed);
  settings.includeCompressor = m_renderIncludeCompressor.load(std::memory_order_relaxed);
  settings.includeReverb = m_renderIncludeReverb.load(std::memory_order_relaxed);
  settings.includeLimiter = m_renderIncludeLimiter.load(std::memory_order_relaxed);

  QString taskId = m_renderEngine.enqueueTrackRender(trackPath, settings);

  if (!taskId.isEmpty()) {
    log::LogService::instance().log(
        "Render", "QueueTrackRender", "Success",
        QString("Enqueued Track Render for %1 into workspace [Task: %2]")
            .arg(trackPath, taskId),
        "Track", trackPath);
    emit renderStateChanged();
  }
  return taskId;
}

QString EngineController::startAlbumRender(const QString &folderPath,
                                           int bitDepth, int rateMode,
                                           double customRate, bool applyDither,
                                           bool enableDsp) {
  if (folderPath.isEmpty())
    return QString();
  auto &wsMgr = engine::workspace::WorkspaceManager::instance();
  if (!wsMgr.hasOpenWorkspace()) {
    log::LogService::instance().log(
        "Render", "QueueAlbumRender", "Failed",
        QStringLiteral("Album render aborted: no dedicated workspace is "
                       "currently open (fail-closed)."),
        "Workspace", QString(),
        QStringLiteral("{\"stage\":\"workspace_check\"}"), QString(), QString(),
        log::ErrorCode::WorkspaceNotOpenFailClosed, 0.0);
    return QString();
  }
  if (wsMgr.isPathBlockedByLockedWorkspace(folderPath)) {
    log::LogService::instance().log(
        "Render", "QueueAlbumRender", "Failed",
        QString(
            "Album render blocked: folder resides in locked workspace (%1).")
            .arg(folderPath),
        "Folder", folderPath,
        QStringLiteral("{\"stage\":\"workspace_lock_check\"}"), QString(),
        QString(), log::ErrorCode::WorkspaceLockedAccessDenied, 0.0);
    return QString();
  }

  m_renderEngine.setWorkspacePath(wsMgr.openWorkspacePath());
  if (m_syncMasterDspToRender.load(std::memory_order_relaxed)) {
    syncMasterDspToRenderEngine();
  }

  engine::render::RenderSettings settings;
  settings.bitDepth =
      (bitDepth == 0)
          ? engine::render::RenderBitDepth::Int16
          : ((bitDepth == 2) ? engine::render::RenderBitDepth::Float32
                             : engine::render::RenderBitDepth::Int24);
  settings.rateMode = (rateMode == 1)
                          ? engine::render::RenderSampleRateMode::Custom
                          : engine::render::RenderSampleRateMode::Original;
  settings.targetSampleRate = customRate > 0 ? customRate : 48000.0;
  settings.applyDither = applyDither;
  settings.enableMasterDsp = enableDsp;
  settings.includePeq = m_renderIncludePeq.load(std::memory_order_relaxed);
  settings.includeGeq = m_renderIncludeGeq.load(std::memory_order_relaxed);
  settings.includeFilter = m_renderIncludeFilter.load(std::memory_order_relaxed);
  settings.includeCompressor = m_renderIncludeCompressor.load(std::memory_order_relaxed);
  settings.includeReverb = m_renderIncludeReverb.load(std::memory_order_relaxed);
  settings.includeLimiter = m_renderIncludeLimiter.load(std::memory_order_relaxed);

  QString taskId = m_renderEngine.enqueueAlbumRender(folderPath, settings);

  if (!taskId.isEmpty()) {
    log::LogService::instance().log(
        "Render", "QueueAlbumRender", "Success",
        QString("Enqueued Album Render for %1 [Task: %2]")
            .arg(folderPath, taskId),
        "Folder", folderPath);
    emit renderStateChanged();
  }
  return taskId;
}

void EngineController::cancelRender() {
  m_renderEngine.cancelCurrentRender();
  log::LogService::instance().log(
      "Render", "CancelRender", "Info",
      "Cancellation requested for active render task.", "RenderEngine",
      "Queue");
  emit renderStateChanged();
}

void EngineController::cancelAllRenders() {
  m_renderEngine.cancelAllRenders();
  log::LogService::instance().log(
      "Render", "CancelAllRenders", "Info",
      "Cancellation requested for active render task and all queued tasks.",
      "RenderEngine", "Queue");
  emit renderStateChanged();
}

void EngineController::resetRenderStatus() {
  if (m_renderEngine.isIdle()) {
    m_renderEngine.resetTaskInfo();
    emit renderStateChanged();
  }
}

void EngineController::setRenderMonitorVolume(double volume) {
  if (!m_pipeline)
    return;
  double clamped = std::clamp(volume, 0.0, 2.0);
  double prev = m_pipeline->renderMonitorVolume();
  if (std::abs(prev - clamped) > 1e-6) {
    m_pipeline->setRenderMonitorVolume(clamped);
    emit renderMonitorVolumeChanged(clamped);
    emit diagnosticsChanged();
  }
}

void EngineController::setListeningVolume(double volume) {
  if (!m_pipeline)
    return;
  double clamped = std::clamp(volume, 0.0, 2.0);
  double prev = m_pipeline->listeningVolume();
  if (std::abs(prev - clamped) > 1e-6) {
    m_pipeline->setListeningVolume(clamped);
    emit listeningVolumeChanged(clamped);
    emit diagnosticsChanged();
  }
}

void EngineController::setRenderMonitorMuted(bool muted) {
  if (!m_pipeline)
    return;
  if (m_pipeline->renderMonitorMuted() != muted) {
    m_pipeline->setRenderMonitorMuted(muted);
    emit renderMonitorMutedChanged(muted);
    emit diagnosticsChanged();
  }
}

void EngineController::setListeningMuted(bool muted) {
  if (!m_pipeline)
    return;
  if (m_pipeline->listeningMuted() != muted) {
    m_pipeline->setListeningMuted(muted);
    emit listeningMutedChanged(muted);
    emit diagnosticsChanged();
  }
}

bool EngineController::auditionRenderedFile(const QString &targetDeckId,
                                            const QString &filePath,
                                            bool switchToRenderMonitor) {
  if (!QFile::exists(filePath)) {
    qWarning() << "auditionRenderedFile: File does not exist:" << filePath;
    QJsonObject det;
    det.insert(QStringLiteral("path"), filePath);
    det.insert(QStringLiteral("stage"), QStringLiteral("file_exists_check"));
    log::LogService::instance().log(
        "Render", "Audition", "Failed",
        QString("Failed to audition rendered file '%1': file does not exist.")
            .arg(filePath),
        "RenderAudition", filePath,
        QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)),
        QString(), QString(), log::ErrorCode::RenderAuditionFailed, 0.0);
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
        QString("Failed to audition rendered file '%1' on Deck %2 (deck may be "
                "locked or format unsupported).")
            .arg(filePath, targetDeckId),
        "RenderAudition", filePath,
        QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)),
        QString(), QString(), log::ErrorCode::RenderAuditionFailed, 0.0);
    return false;
  }

  // 2. Select deck and optionally switch to Render Monitor Path
  // (Section 5.11.5)
  switchDeck(targetDeckId, 0);
  if (switchToRenderMonitor) {
    setOutputRoutingMode(1); // 1 = RenderMonitorPath
  }

  const bool isPureMonitor =
      (m_outputRoutingMode.load(std::memory_order_relaxed) == 1);
  log::LogService::instance().log(
      "Render", "Audition", isPureMonitor ? "Success" : "Warning",
      isPureMonitor
          ? QString("Auditioning rendered file '%1' on Deck %2 (Render Monitor "
                    "Path engaged).")
                .arg(filePath, targetDeckId)
          : QString("Auditioning rendered file '%1' on Deck %2 via Listening "
                    "Path (WARNING: Listening Correction is applied).")
                .arg(filePath, targetDeckId),
      "RenderAudition", filePath);

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
  map["type"] =
      (task.type == engine::render::RenderType::Album) ? "Album" : "Track";
  map["status"] = static_cast<int>(task.status);
  map["statusText"] = renderStatusText();
  map["progress"] = task.progress;
  map["currentTrackIndex"] =
      (task.totalTracks > 0) ? (task.currentTrackIndex + 1) : 0;
  map["totalTracks"] = task.totalTracks;
  map["currentTrackName"] = task.currentTrackName;
  map["serial"] = task.serial;
  map["finalOutputDir"] = task.finalOutputDir;
  map["errorMessage"] = task.errorMessage;
  map["warningMessage"] = task.warningMessage;
  map["queuedTasksCount"] = m_renderEngine.queuedTasksCount();
  map["trackResults"] = task.trackResults;
  map["renderedFiles"] = task.renderedFiles;
  map["rollbackPerformed"] = task.rollbackPerformed;
  map["rollbackSucceeded"] = task.rollbackSucceeded;

  return map;
}

QString EngineController::defaultSessionStatePath() const {
  QString xdgState = qEnvironmentVariable("XDG_STATE_HOME");
  if (xdgState.isEmpty()) {
    xdgState = QDir::homePath() + QStringLiteral("/.local/state");
  }
  return xdgState + QStringLiteral("/desk32/session_state.json");
}

bool EngineController::saveSessionState(const QString &filePath) const {
  const QString targetPath = filePath.isEmpty() ? defaultSessionStatePath() : filePath;
  QDir().mkpath(QFileInfo(targetPath).absolutePath());

  QJsonObject root;
  root["format_version"] = 1;
  root["saved_utc"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
  root["workspace_path"] =
      engine::workspace::WorkspaceManager::instance().openWorkspacePath();
  root["active_deck"] = activeDeckId();
  root["switching_mode"] = switchingMode();
  root["global_dsp_bypass"] = globalDspBypass();
  root["master_gain_l"] = masterGainL();
  root["master_gain_r"] = masterGainR();
  root["master_mute"] = isMasterMute();

  auto *nonConstThis = const_cast<EngineController *>(this);
  root["master_dsp"] = QJsonObject::fromVariantMap(
      nonConstThis->captureCurrentStateAsPreset(
          engine::preset::PresetCategories::MasterDsp));
  root["listening_correction"] = QJsonObject::fromVariantMap(
      nonConstThis->captureCurrentStateAsPreset(
          engine::preset::PresetCategories::ListeningCorrection));
  root["external_insert"] = QJsonObject::fromVariantMap(
      nonConstThis->captureCurrentStateAsPreset(
          engine::preset::PresetCategories::ExternalInsert));
  root["audio_device"] = QJsonObject::fromVariantMap(
      nonConstThis->captureCurrentStateAsPreset(
          engine::preset::PresetCategories::AudioDevice));

  QJsonObject renderNodes;
  renderNodes["sync_master_dsp"] = m_syncMasterDspToRender.load(std::memory_order_relaxed);
  renderNodes["PEQ"] = m_renderIncludePeq.load(std::memory_order_relaxed);
  renderNodes["GEQ"] = m_renderIncludeGeq.load(std::memory_order_relaxed);
  renderNodes["Filter"] = m_renderIncludeFilter.load(std::memory_order_relaxed);
  renderNodes["Compressor"] = m_renderIncludeCompressor.load(std::memory_order_relaxed);
  renderNodes["Reverb"] = m_renderIncludeReverb.load(std::memory_order_relaxed);
  renderNodes["Limiter"] = m_renderIncludeLimiter.load(std::memory_order_relaxed);
  root["render_nodes"] = renderNodes;

  QJsonArray decksArr;
  if (m_deckManager) {
    for (char cid : {'A', 'B', 'C', 'D'}) {
      if (const auto *d = m_deckManager->deck(cid)) {
        if (!d->isOpen() || d->trackInfo().path.empty()) continue;
        QJsonObject dobj;
        dobj["deck"] = QString(cid);
        dobj["path"] = QString::fromStdString(d->trackInfo().path);
        dobj["frame"] = static_cast<qint64>(d->playhead());
        dobj["level_match_db"] = d->levelMatchGainDb();
        dobj["offset_frames"] = static_cast<qint64>(d->offsetFrames());
        dobj["locked"] = d->isLocked();
        dobj["folder_mode"] = d->folderSession().isActive();
        dobj["folder_path"] = QString::fromStdString(d->folderSession().folderPath());
        decksArr.append(dobj);
      }
    }
  }
  root["decks"] = decksArr;

  QSaveFile save(targetPath);
  if (!save.open(QIODevice::WriteOnly)) {
    return false;
  }
  save.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
  return save.commit();
}

bool EngineController::restoreSessionState(const QString &filePath) {
  const QString targetPath = filePath.isEmpty() ? defaultSessionStatePath() : filePath;
  QFile f(targetPath);
  if (!f.exists() || !f.open(QIODevice::ReadOnly)) {
    return false;
  }
  QJsonParseError err{};
  const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
  f.close();
  if (err.error != QJsonParseError::NoError || !doc.isObject()) {
    return false;
  }

  const QJsonObject root = doc.object();
  if (root.contains("audio_device")) {
    applyAudioDeviceJson(root["audio_device"].toObject());
  }
  if (root.contains("master_dsp")) {
    applyMasterDspJson(root["master_dsp"].toObject());
  }
  if (root.contains("listening_correction")) {
    const QJsonObject lc = root["listening_correction"].toObject();
    applyListeningCorrectionJson(lc, lc["preset_name"].toString());
  }
  if (root.contains("external_insert")) {
    applyExternalInsertJson(root["external_insert"].toObject());
  }
  if (root.contains("render_nodes")) {
    const QJsonObject rn = root["render_nodes"].toObject();
    setSyncMasterDspToRender(rn["sync_master_dsp"].toBool(true));
    setRenderNodeEnabled("PEQ", rn["PEQ"].toBool(true));
    setRenderNodeEnabled("GEQ", rn["GEQ"].toBool(true));
    setRenderNodeEnabled("Filter", rn["Filter"].toBool(true));
    setRenderNodeEnabled("Compressor", rn["Compressor"].toBool(true));
    setRenderNodeEnabled("Reverb", rn["Reverb"].toBool(true));
    setRenderNodeEnabled("Limiter", rn["Limiter"].toBool(true));
  }
  if (root.contains("global_dsp_bypass")) {
    setGlobalDspBypass(root["global_dsp_bypass"].toBool(false));
  }
  if (root.contains("switching_mode")) {
    setSwitchingMode(root["switching_mode"].toInt(0));
  }
  if (root.contains("master_gain_l")) {
    setMasterGainL(root["master_gain_l"].toDouble(1.0));
  }
  if (root.contains("master_gain_r")) {
    setMasterGainR(root["master_gain_r"].toDouble(1.0));
  }
  if (root.contains("master_mute")) {
    setMasterMute(root["master_mute"].toBool(false));
  }

  const QJsonArray decksArr = root["decks"].toArray();
  for (const QJsonValue &dv : decksArr) {
    if (!dv.isObject()) continue;
    const QJsonObject dobj = dv.toObject();
    const QString dId = dobj["deck"].toString();
    const QString path = dobj["path"].toString();
    if (!dId.isEmpty() && !path.isEmpty() && QFileInfo::exists(path)) {
      if (loadTrack(dId, path)) {
        if (dobj["folder_mode"].toBool(false)) {
          startFolderSessionFromDeck(dId);
        }
        const qint64 frame = dobj["frame"].toInteger(0);
        if (frame > 0) {
          seek(dId, frame);
        }
        setDeckLevelMatchDb(dId, dobj["level_match_db"].toDouble(0.0));
        setDeckOffsetFrames(dId, dobj["offset_frames"].toInteger(0));
        setDeckLocked(dId, dobj["locked"].toBool(false));
      }
    }
  }
  const QString activeD = root["active_deck"].toString();
  if (!activeD.isEmpty()) {
    switchDeck(activeD, switchingMode());
  }

  emit diagnosticsChanged();
  return true;
}

} // namespace desk32::core
