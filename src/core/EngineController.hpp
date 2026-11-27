#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVariantList>
#include <QTimer>
#include <memory>
#include <array>
#include <atomic>
#include <thread>
#include "engine/deck/DeckManager.hpp"
#include "engine/pipeline/AudioPipeline.hpp"
#include "engine/pipeline/BuiltInDspNodes.hpp"
#include "engine/insert/ExternalInsertNode.hpp"
#include "engine/dsp/ParametricEQ.hpp"
#include "engine/dsp/GraphicEQ.hpp"
#include "engine/dsp/HighLowPassFilter.hpp"
#include "engine/dsp/Compressor.hpp"
#include "engine/dsp/PeakLimiter.hpp"
#include "engine/dsp/AlgorithmicReverb.hpp"
#include "engine/dsp/ParameterSmoother.hpp"
#include "engine/preset/PresetManager.hpp"
#include "engine/preset/PresetStateTracker.hpp"
#include "engine/render/RenderEngine.hpp"
#include "engine/backend/AudioControlManager.hpp"
#include "engine/backend/AudioOutput.hpp"
#include "engine/backend/PipeWireAudioOutput.hpp"
#include "engine/backend/PipeWireControlAdapter.hpp"
#include "engine/backend/JackAudioOutput.hpp"
#include "engine/backend/JackControlAdapter.hpp"
#include "engine/insert/BufferedInsertTransport.hpp"
#include "engine/insert/PipeWireInsertTransport.hpp"
#include "engine/insert/JackInsertTransport.hpp"
#include "engine/audio/WaveformExtractor.hpp"
#include "core/log/LogService.hpp"

namespace desk32::core {

/// Unified Studio Console Controller bridging QML UI with Engine Subsystems
/// Coordinates DeckManager, AudioPipeline, AudioOutput backends (PipeWire / JACK), LogService, ExternalInsert, and PresetManager.
class EngineController : public QObject {
    Q_OBJECT

    Q_PROPERTY(QString activeDeckId READ activeDeckId NOTIFY activeDeckChanged)
    Q_PROPERTY(double graphSampleRate READ graphSampleRate NOTIFY graphSampleRateChanged)
    Q_PROPERTY(int switchingMode READ switchingMode WRITE setSwitchingMode NOTIFY switchingModeChanged)
    Q_PROPERTY(int outputRoutingMode READ outputRoutingMode WRITE setOutputRoutingMode NOTIFY outputRoutingModeChanged)
    Q_PROPERTY(bool globalDspBypass READ globalDspBypass WRITE setGlobalDspBypass NOTIFY globalDspBypassChanged)
    Q_PROPERTY(double masterGain READ masterGain WRITE setMasterGain NOTIFY masterGainChanged)
    Q_PROPERTY(double masterGainL READ masterGainL WRITE setMasterGainL NOTIFY masterGainLChanged)
    Q_PROPERTY(double masterGainR READ masterGainR WRITE setMasterGainR NOTIFY masterGainRChanged)
    Q_PROPERTY(double masterBalance READ masterBalance WRITE setMasterBalance NOTIFY masterBalanceChanged)
    Q_PROPERTY(bool masterMute READ isMasterMute WRITE setMasterMute NOTIFY masterMuteChanged)
    Q_PROPERTY(bool masterMuteL READ isMasterMuteL WRITE setMasterMuteL NOTIFY masterMuteLChanged)
    Q_PROPERTY(bool masterMuteR READ isMasterMuteR WRITE setMasterMuteR NOTIFY masterMuteRChanged)
    Q_PROPERTY(bool monitorDimEnabled READ isMonitorDimEnabled WRITE setMonitorDimEnabled NOTIFY monitorDimEnabledChanged)
    Q_PROPERTY(double masterPeakL READ masterPeakL NOTIFY dspTelemetryChanged)
    Q_PROPERTY(double masterPeakR READ masterPeakR NOTIFY dspTelemetryChanged)
    Q_PROPERTY(double preDspPeakL READ preDspPeakL NOTIFY dspTelemetryChanged)
    Q_PROPERTY(double preDspPeakR READ preDspPeakR NOTIFY dspTelemetryChanged)
    Q_PROPERTY(double preDspRmsL READ preDspRmsL NOTIFY dspTelemetryChanged)
    Q_PROPERTY(double preDspRmsR READ preDspRmsR NOTIFY dspTelemetryChanged)
    Q_PROPERTY(double renderTapPeakL READ renderTapPeakL NOTIFY dspTelemetryChanged)
    Q_PROPERTY(double renderTapPeakR READ renderTapPeakR NOTIFY dspTelemetryChanged)
    Q_PROPERTY(double masterRmsL READ masterRmsL NOTIFY dspTelemetryChanged)
    Q_PROPERTY(double masterRmsR READ masterRmsR NOTIFY dspTelemetryChanged)
    Q_PROPERTY(bool masterClipped READ masterClipped NOTIFY dspTelemetryChanged)
    Q_PROPERTY(double masterHeadroomDb READ masterHeadroomDb NOTIFY dspTelemetryChanged)
    Q_PROPERTY(int xrunCount READ xrunCount NOTIFY diagnosticsChanged)
    Q_PROPERTY(bool isDirectMode READ isDirectMode NOTIFY diagnosticsChanged)
    Q_PROPERTY(bool directModeEnabled READ isDirectModeEnabled WRITE setDirectModeEnabled NOTIFY diagnosticsChanged)
    Q_PROPERTY(double lastRenegotiationSilenceMs READ lastRenegotiationSilenceMs NOTIFY diagnosticsChanged)
    Q_PROPERTY(int directModeStatus READ directModeStatus NOTIFY diagnosticsChanged)
    Q_PROPERTY(int bitPerfectStatus READ bitPerfectStatus NOTIFY diagnosticsChanged)
    Q_PROPERTY(int bitPerfectModeState READ bitPerfectModeState NOTIFY diagnosticsChanged)
    Q_PROPERTY(QString bitPerfectModeStateText READ bitPerfectModeStateText NOTIFY diagnosticsChanged)
    Q_PROPERTY(int backendStatus READ backendStatus NOTIFY diagnosticsChanged)
    Q_PROPERTY(QString backendStatusText READ backendStatusText NOTIFY diagnosticsChanged)
    Q_PROPERTY(QString audioBackend READ audioBackend WRITE setAudioBackend NOTIFY audioBackendChanged)
    Q_PROPERTY(QString graphicsBackend READ graphicsBackend NOTIFY diagnosticsChanged)
    Q_PROPERTY(QString gpuRendererName READ gpuRendererName NOTIFY diagnosticsChanged)

    // Phase 2: Dynamic DSP Telemetry & Status Properties
    Q_PROPERTY(double compressorGainReductionDb READ compressorGainReductionDb NOTIFY dspTelemetryChanged)
    Q_PROPERTY(double limiterGainReductionDb READ limiterGainReductionDb NOTIFY dspTelemetryChanged)
    Q_PROPERTY(int limiterLatencyFrames READ limiterLatencyFrames NOTIFY dspTelemetryChanged)
    Q_PROPERTY(QString listeningPresetName READ listeningPresetName NOTIFY listeningPresetChanged)
    Q_PROPERTY(bool listeningCorrectionActive READ listeningCorrectionActive NOTIFY outputRoutingModeChanged)
    Q_PROPERTY(bool geqBypassed READ isGeqBypassed WRITE setGeqBypassed NOTIFY geqParametersChanged)
    Q_PROPERTY(bool geqGlobalBypass READ isGeqGlobalBypass WRITE setGeqGlobalBypass NOTIFY geqParametersChanged)
    Q_PROPERTY(bool peqBypassed READ isPeqBypassed WRITE setPeqBypassed NOTIFY peqParametersChanged)

    // Spec v3.8.3 §11.1 / P5: Canonical DSP Preset State (Application Controller owned)
    Q_PROPERTY(QString activeGlobalPresetId READ activeGlobalPresetId NOTIFY dspPresetStateChanged)
    Q_PROPERTY(QString activeGlobalPresetName READ activeGlobalPresetName NOTIFY dspPresetStateChanged)
    Q_PROPERTY(bool globalPresetModified READ isGlobalPresetModified NOTIFY dspPresetStateChanged)
    Q_PROPERTY(QVariantMap dspPresetIndicators READ dspPresetIndicators NOTIFY dspPresetStateChanged)

    // Phase 3: External Insert & Host Bridge Telemetry
    Q_PROPERTY(int externalInsertState READ externalInsertState NOTIFY externalInsertStatusChanged)
    Q_PROPERTY(QString externalInsertStateText READ externalInsertStateText NOTIFY externalInsertStatusChanged)
    Q_PROPERTY(bool externalInsertBypassed READ isExternalInsertBypassed WRITE setExternalInsertBypass NOTIFY externalInsertStatusChanged)
    Q_PROPERTY(int externalInsertReportedLatency READ externalInsertReportedLatency NOTIFY externalInsertStatusChanged)
    Q_PROPERTY(int externalInsertMeasuredLatency READ externalInsertMeasuredLatency NOTIFY externalInsertStatusChanged)
    Q_PROPERTY(double externalInsertMeasuredLatencyMs READ externalInsertMeasuredLatencyMs NOTIFY externalInsertStatusChanged)
    Q_PROPERTY(int externalInsertDelayCompensation READ insertDelayCompensation WRITE setInsertDelayCompensation NOTIFY externalInsertStatusChanged)
    Q_PROPERTY(int externalInsertXRunCount READ externalInsertXRunCount NOTIFY externalInsertStatusChanged)
    Q_PROPERTY(QString externalInsertLastError READ externalInsertLastError NOTIFY externalInsertStatusChanged)
    Q_PROPERTY(QString externalInsertClientName READ externalInsertClientName NOTIFY externalInsertStatusChanged)
    Q_PROPERTY(QString externalInsertSendPortL READ externalInsertSendPortL NOTIFY externalInsertStatusChanged)
    Q_PROPERTY(QString externalInsertSendPortR READ externalInsertSendPortR NOTIFY externalInsertStatusChanged)
    Q_PROPERTY(QString externalInsertReturnPortL READ externalInsertReturnPortL NOTIFY externalInsertStatusChanged)
    Q_PROPERTY(QString externalInsertReturnPortR READ externalInsertReturnPortR NOTIFY externalInsertStatusChanged)
    Q_PROPERTY(int externalInsertPosition READ externalInsertPosition WRITE setExternalInsertPosition NOTIFY externalInsertStatusChanged)
    Q_PROPERTY(int externalInsertDisconnectPolicy READ externalInsertDisconnectPolicy WRITE setExternalInsertDisconnectPolicy NOTIFY externalInsertStatusChanged)
    Q_PROPERTY(bool externalInsertCalibrating READ isExternalInsertCalibrating NOTIFY externalInsertStatusChanged)
    Q_PROPERTY(bool externalInsertSimulationMode READ isExternalInsertSimulationMode NOTIFY externalInsertStatusChanged)
    Q_PROPERTY(double externalInsertSendPeak READ externalInsertSendPeak NOTIFY dspTelemetryChanged)
    Q_PROPERTY(double externalInsertReturnPeak READ externalInsertReturnPeak NOTIFY dspTelemetryChanged)
    Q_PROPERTY(bool syncMasterDspToRender READ syncMasterDspToRenderEnabled WRITE setSyncMasterDspToRender NOTIFY renderStateChanged)
    Q_PROPERTY(bool levelMatchInProgress READ isLevelMatchInProgress NOTIFY levelMatchStateChanged)

    // Phase 4: Export & Render Telemetry (Section 5.11 & 20.1)
    // All render properties delegate directly to RenderEngine as the single canonical state source.
    Q_PROPERTY(bool isRenderIdle READ isRenderIdle NOTIFY renderStateChanged)
    Q_PROPERTY(bool isRendering READ isRendering NOTIFY renderStateChanged)
    Q_PROPERTY(int renderWorkerState READ renderWorkerState NOTIFY renderStateChanged)
    Q_PROPERTY(QString renderWorkerStateText READ renderWorkerStateText NOTIFY renderStateChanged)
    Q_PROPERTY(int renderQueuedCount READ renderQueuedCount NOTIFY renderStateChanged)
    Q_PROPERTY(int renderStatus READ renderStatus NOTIFY renderStateChanged)
    Q_PROPERTY(QString renderStatusText READ renderStatusText NOTIFY renderStateChanged)
    Q_PROPERTY(QString renderTaskId READ renderTaskId NOTIFY renderStateChanged)
    Q_PROPERTY(QString renderType READ renderType NOTIFY renderStateChanged)
    Q_PROPERTY(double renderProgress READ renderProgress NOTIFY renderStateChanged)
    Q_PROPERTY(QString renderCurrentTrackName READ renderCurrentTrackName NOTIFY renderStateChanged)
    Q_PROPERTY(int renderTrackIndex READ renderTrackIndex NOTIFY renderStateChanged)
    Q_PROPERTY(int renderTotalTracks READ renderTotalTracks NOTIFY renderStateChanged)
    Q_PROPERTY(QString renderLastOutputDir READ renderLastOutputDir NOTIFY renderStateChanged)
    Q_PROPERTY(QString renderSerial READ renderSerial NOTIFY renderStateChanged)
    Q_PROPERTY(QString renderErrorMessage READ renderErrorMessage NOTIFY renderStateChanged)
    Q_PROPERTY(QVariantList renderTrackResults READ renderTrackResults NOTIFY renderStateChanged)
    Q_PROPERTY(QStringList renderOutputFiles READ renderOutputFiles NOTIFY renderStateChanged)
    Q_PROPERTY(bool renderRollbackPerformed READ renderRollbackPerformed NOTIFY renderStateChanged)
    Q_PROPERTY(bool renderRollbackSucceeded READ renderRollbackSucceeded NOTIFY renderStateChanged)
    Q_PROPERTY(double renderMonitorVolume READ renderMonitorVolume WRITE setRenderMonitorVolume NOTIFY renderMonitorVolumeChanged)
    Q_PROPERTY(double listeningVolume READ listeningVolume WRITE setListeningVolume NOTIFY listeningVolumeChanged)
    Q_PROPERTY(bool renderMonitorMuted READ renderMonitorMuted WRITE setRenderMonitorMuted NOTIFY renderMonitorMutedChanged)
    Q_PROPERTY(bool listeningMuted READ listeningMuted WRITE setListeningMuted NOTIFY listeningMutedChanged)
    Q_PROPERTY(QString outputPcmFormat READ outputPcmFormat WRITE setOutputPcmFormat NOTIFY diagnosticsChanged)
    Q_PROPERTY(bool liveDitherEnabled READ isLiveDitherEnabled WRITE setLiveDitherEnabled NOTIFY diagnosticsChanged)
    Q_PROPERTY(QString currentOutputDevice READ currentOutputDevice NOTIFY diagnosticsChanged)

public:
    static EngineController& instance();

    explicit EngineController(QObject* parent = nullptr);
    ~EngineController() override;

    bool initialize(bool offlineMode = false);

    // Session Lifecycle & Observability (§2, §5)
    void recordSessionStart(const QString& graphicsBackend = QStringLiteral("headless"),
                            const QString& gpuName = QStringLiteral("none"));
    void recordSessionEnd(const QString& reason = QStringLiteral("normal_quit"));
    Q_INVOKABLE void commitDspParameterChange(const QString& dspNode,
                                              const QVariantMap& beforeMap,
                                              const QVariantMap& afterMap,
                                              const QString& trigger = QStringLiteral("user"));
    Q_INVOKABLE void copyToClipboard(const QString& text);

    // Property getters
    [[nodiscard]] QString activeDeckId() const;
    [[nodiscard]] double graphSampleRate() const;
    [[nodiscard]] int switchingMode() const;
    [[nodiscard]] int outputRoutingMode() const;
    [[nodiscard]] bool globalDspBypass() const;
    [[nodiscard]] double masterGain() const;
    [[nodiscard]] double masterGainL() const;
    [[nodiscard]] double masterGainR() const;
    [[nodiscard]] double masterBalance() const;
    [[nodiscard]] bool isMasterMute() const;
    [[nodiscard]] bool isMasterMuteL() const;
    [[nodiscard]] bool isMasterMuteR() const;
    [[nodiscard]] bool isMonitorDimEnabled() const;
    [[nodiscard]] double masterPeakL() const;
    [[nodiscard]] double masterPeakR() const;
    [[nodiscard]] double preDspPeakL() const;
    [[nodiscard]] double preDspPeakR() const;
    [[nodiscard]] double preDspRmsL() const;
    [[nodiscard]] double preDspRmsR() const;
    [[nodiscard]] double renderTapPeakL() const;
    [[nodiscard]] double renderTapPeakR() const;
    [[nodiscard]] double masterRmsL() const;
    [[nodiscard]] double masterRmsR() const;
    [[nodiscard]] bool masterClipped() const;
    [[nodiscard]] double masterHeadroomDb() const;
    [[nodiscard]] int xrunCount() const;
    [[nodiscard]] bool isDirectMode() const;
    [[nodiscard]] bool isDirectModeEnabled() const noexcept { return m_directModeEnabled.load(std::memory_order_relaxed); }
    [[nodiscard]] double lastRenegotiationSilenceMs() const noexcept;
    [[nodiscard]] int directModeStatus() const;
    [[nodiscard]] int bitPerfectStatus() const;
    [[nodiscard]] int bitPerfectModeState() const;
    [[nodiscard]] QString bitPerfectModeStateText() const;
    Q_INVOKABLE QVariantMap getBitPerfectVerificationReport() const;
    Q_INVOKABLE bool exportBitPerfectReportToJson(
        const QString& filePath,
        const QVariantMap& reportSnapshot = QVariantMap()) const;
    void checkBitPerfectVerificationTransition();
    Q_INVOKABLE QVariantMap getAudioCapabilities() const;
    Q_INVOKABLE QStringList availableAudioBackends() const;
    [[nodiscard]] QString audioBackend() const;
    [[nodiscard]] QString graphicsBackend() const { return m_graphicsBackend; }
    [[nodiscard]] QString gpuRendererName() const { return m_gpuRendererName; }
    Q_INVOKABLE QString logDatabasePath() const { return log::LogService::defaultDatabasePath(); }
    Q_INVOKABLE bool setAudioBackend(const QString& backendName);
    Q_INVOKABLE QVariantList enumerateOutputDevices() const;
    Q_INVOKABLE bool selectOutputDevice(const QString& deviceIdOrName);
    [[nodiscard]] QString currentOutputDevice() const;
    Q_INVOKABLE bool setOutputBufferFrames(int bufferFrames);
    Q_INVOKABLE void setOutputPcmFormat(const QString& formatName);
    [[nodiscard]] QString outputPcmFormat() const;
    Q_INVOKABLE void setLiveDitherEnabled(bool enabled);
    [[nodiscard]] bool isLiveDitherEnabled() const noexcept;
    [[nodiscard]] int backendStatus() const;
    [[nodiscard]] QString backendStatusText() const;

    // Phase 2 Getters
    [[nodiscard]] double compressorGainReductionDb() const;
    [[nodiscard]] double limiterGainReductionDb() const;
    [[nodiscard]] int limiterLatencyFrames() const;
    [[nodiscard]] QString listeningPresetName() const;
    [[nodiscard]] bool listeningCorrectionActive() const;
    [[nodiscard]] bool isGeqBypassed() const;
    [[nodiscard]] bool isPeqBypassed() const;

    // Phase 3 Getters
    [[nodiscard]] int externalInsertState() const;
    [[nodiscard]] QString externalInsertStateText() const;
    [[nodiscard]] bool isExternalInsertBypassed() const;
    [[nodiscard]] int externalInsertReportedLatency() const;
    [[nodiscard]] int externalInsertMeasuredLatency() const;
    [[nodiscard]] double externalInsertMeasuredLatencyMs() const;
    [[nodiscard]] int insertDelayCompensation() const;
    [[nodiscard]] int externalInsertXRunCount() const;
    [[nodiscard]] QString externalInsertLastError() const;
    [[nodiscard]] QString externalInsertClientName() const;
    [[nodiscard]] QString externalInsertSendPortL() const;
    [[nodiscard]] QString externalInsertSendPortR() const;
    [[nodiscard]] QString externalInsertReturnPortL() const;
    [[nodiscard]] QString externalInsertReturnPortR() const;
    [[nodiscard]] double externalInsertSendPeak() const;
    [[nodiscard]] double externalInsertReturnPeak() const;

    // Phase 4 Getters (Delegating directly to RenderEngine & AudioPipeline)
    [[nodiscard]] bool isRenderIdle() const;
    [[nodiscard]] bool isRendering() const;
    [[nodiscard]] int renderWorkerState() const;
    [[nodiscard]] QString renderWorkerStateText() const;
    [[nodiscard]] int renderQueuedCount() const;
    [[nodiscard]] int renderStatus() const;
    [[nodiscard]] QString renderStatusText() const;
    [[nodiscard]] QString renderTaskId() const;
    [[nodiscard]] QString renderType() const;
    [[nodiscard]] double renderProgress() const;
    [[nodiscard]] QString renderCurrentTrackName() const;
    [[nodiscard]] int renderTrackIndex() const;
    [[nodiscard]] int renderTotalTracks() const;
    [[nodiscard]] QString renderLastOutputDir() const;
    [[nodiscard]] QString renderSerial() const;
    [[nodiscard]] QString renderErrorMessage() const;
    [[nodiscard]] QVariantList renderTrackResults() const;
    [[nodiscard]] QStringList renderOutputFiles() const;
    [[nodiscard]] bool renderRollbackPerformed() const;
    [[nodiscard]] bool renderRollbackSucceeded() const;
    [[nodiscard]] double renderMonitorVolume() const;
    [[nodiscard]] double listeningVolume() const;
    [[nodiscard]] bool renderMonitorMuted() const;
    [[nodiscard]] bool listeningMuted() const;
    [[nodiscard]] std::shared_ptr<engine::pipeline::AudioPipeline> pipeline() const noexcept { return m_pipeline; }

    // Property setters & QML invokables
    Q_INVOKABLE void setSwitchingMode(int mode);
    Q_INVOKABLE void setOutputRoutingMode(int path);
    Q_INVOKABLE void setGlobalDspBypass(bool bypass);
    Q_INVOKABLE bool resyncGlobalDspBypass();
    [[nodiscard]] bool isGlobalDspBypassDesynced() const noexcept { return m_globalBypassDesynced.load(std::memory_order_relaxed); }
    Q_INVOKABLE void setMasterGain(double gain);
    Q_INVOKABLE void setMasterGainL(double gain);
    Q_INVOKABLE void setMasterGainR(double gain);
    Q_INVOKABLE void setMasterBalance(double balance);
    Q_INVOKABLE void setMasterMute(bool mute);
    Q_INVOKABLE void setMasterMuteL(bool mute);
    Q_INVOKABLE void setMasterMuteR(bool mute);
    Q_INVOKABLE void setMonitorDimEnabled(bool dim);
    Q_INVOKABLE void setDirectModeEnabled(bool enabled);
    Q_INVOKABLE bool setFixedGraphSampleRate(double sampleRate);

    // QML-invokable Deck Operations
    Q_INVOKABLE void play(const QString& deckId);
    Q_INVOKABLE void pause(const QString& deckId);
    Q_INVOKABLE void stop(const QString& deckId);
    Q_INVOKABLE void seek(const QString& deckId, qint64 targetFrame);
    void seekWithTrigger(const QString& deckId, qint64 targetFrame, const QString& trigger);
    Q_INVOKABLE void switchDeck(const QString& targetDeckId, int mode);
    Q_INVOKABLE void quickSwitchAB();
    Q_INVOKABLE void setDeckLocked(const QString& deckId, bool locked);
    Q_INVOKABLE bool loadTrack(const QString& deckId, const QString& filePath);
    Q_INVOKABLE bool loadWorkspaceTrack(const QString& deckId, const QString& relativePath);
    Q_INVOKABLE bool copyDeckTrackToWorkspace(const QString& deckId);
    Q_INVOKABLE void setDeckOffsetFrames(const QString& deckId, qint64 offsetFrames);
    Q_INVOKABLE qint64 getDeckOffsetFrames(const QString& deckId) const;
    Q_INVOKABLE void setDeckLevelMatchDb(const QString& deckId, double gainDb);
    Q_INVOKABLE double getDeckLevelMatchDb(const QString& deckId) const;
    Q_INVOKABLE double getTrackMeasuredRmsDb(const QString& deckId) const;
    Q_INVOKABLE double getTrackMeasuredLufs(const QString& deckId) const;
    Q_INVOKABLE double measureAndApplyDeckLevelMatch(const QString& referenceDeckId, const QString& targetDeckId);
    Q_INVOKABLE void startAutoLevelMatch(const QString& referenceDeckId, const QStringList& targetDeckIds);
    [[nodiscard]] bool isLevelMatchInProgress() const noexcept {
        return m_levelMatchInProgress.load(std::memory_order_acquire);
    }

    // QML-invokable Folder Session Operations
    Q_INVOKABLE bool startFolderSession(const QString& deckId, const QString& folderPath, const QStringList& fileNames = {});
    Q_INVOKABLE bool startFolderSessionFromDeck(const QString& deckId);
    Q_INVOKABLE bool advanceFolderNext(const QString& deckId);
    Q_INVOKABLE bool retreatFolderPrevious(const QString& deckId);

    // Waveform Extraction (+ Disk Cache) & RT-Safe FFT Spectrum Analysis
    Q_INVOKABLE QVariantMap getDeckWaveformPeaks(const QString& deckId, int targetBins = 280) const;
    Q_INVOKABLE bool isDeckWaveformExtracting(const QString& deckId) const;
    Q_INVOKABLE void requestDeckWaveform(const QString& deckId);
    Q_INVOKABLE QString waveformCachePath() const;
    Q_INVOKABLE qint64 waveformCacheSizeBytes() const;
    Q_INVOKABLE int waveformCacheFileCount() const;
    Q_INVOKABLE bool clearWaveformCache();
    Q_INVOKABLE QVariantList getSpectrumBins(int channelMode = 0, int numBins = 160, bool autoPreferRight = false);

    // QML-invokable DSP & EQ Console Operations
    Q_INVOKABLE void setGeqBandGain(int bandIndex, double gainDb);
    Q_INVOKABLE void setGeqBandEnabled(int bandIndex, bool enabled);
    Q_INVOKABLE bool isGeqBandEnabled(int bandIndex) const;
    Q_INVOKABLE double getGeqBandGain(int bandIndex);
    Q_INVOKABLE void resetAllGeqBands();
    Q_INVOKABLE QVariantList getGeqCurve(int numPoints = 128);

    // 32-Band PEQ Operations (Left 16 + Right 16)
    Q_INVOKABLE QVariantMap getPeqBand(bool isRightChannel, int bandIndex);
    Q_INVOKABLE void setPeqBand(bool isRightChannel, int bandIndex, double frequency, double gainDb, double q, int filterType, bool enabled);
    Q_INVOKABLE void setPeqBandEnabled(bool isRightChannel, int bandIndex, bool enabled);
    Q_INVOKABLE QVariantList getPeqCurve(bool isRightChannel, int numPoints = 128);
    Q_INVOKABLE void applyPeqPreset(const QString& presetName);

    // Dynamic Processors & Filters
    Q_INVOKABLE void setFilterParams(double hpfFreq, bool hpfOn, int hpfSlope, double lpfFreq, bool lpfOn, int lpfSlope,
                                     double hpfQ = 0.7071067811865475, double lpfQ = 0.7071067811865475);
    Q_INVOKABLE QVariantMap getFilterParams();
    Q_INVOKABLE QVariantList getFilterCurve(int numPoints = 128);

    Q_INVOKABLE void setCompressorParams(double thresholdDb, double ratio, double attackMs, double releaseMs, double makeupDb, double kneeDb);
    Q_INVOKABLE QVariantMap getCompressorParams();

    Q_INVOKABLE void setLimiterParams(double ceilingDb, double releaseMs, double lookaheadMs);
    Q_INVOKABLE QVariantMap getLimiterParams();

    Q_INVOKABLE void setReverbParams(double roomSize, double damping, double wetLevel, double predelayMs, bool tailPreserving,
                                     double decaySeconds = -1.0);
    Q_INVOKABLE void setReverbLimits(double maxTailRetentionSeconds, double maxProcessingTimeMs);
    Q_INVOKABLE QVariantMap getReverbParams();
    Q_INVOKABLE QVariantMap getLastPresetImportReport() const;
    Q_INVOKABLE QVariantList getAudioNodeContracts() const;

    // DSP Input / Output Trim (±12 dB, 0.1 dB step, excluded from bypass & presets)
    Q_INVOKABLE void setDspTrim(const QString& dspName, bool isOutput, double gainDb);
    Q_INVOKABLE double getDspTrim(const QString& dspName, bool isOutput) const;
    Q_INVOKABLE QVariantMap getAllDspTrims() const;
    Q_INVOKABLE void resetAllDspTrims();
    Q_INVOKABLE double getPeqPreampDb() const noexcept { return m_uiPeq.preampDb(); }
    Q_INVOKABLE double getListeningPreampDb() const noexcept { return m_uiListeningCorrection.preampDb(); }

    // Listening Correction Presets & Per-Band Control (Section 5.10)
    Q_INVOKABLE QStringList availableListeningPresets();
    Q_INVOKABLE void setListeningPreset(const QString& presetName);
    Q_INVOKABLE QVariantMap getListeningBand(bool isRightChannel, int bandIndex);
    Q_INVOKABLE void setListeningBand(bool isRightChannel, int bandIndex, double frequency, double gainDb, double q, int filterType, bool enabled);
    Q_INVOKABLE QVariantList getListeningCurve(bool isRightChannel, int numPoints = 128);

    Q_INVOKABLE void setDspNodeBypass(const QString& nodeName, bool bypass);
    Q_INVOKABLE void setGeqBypassed(bool bypass);
    Q_INVOKABLE bool isGeqGlobalBypass() const noexcept;
    Q_INVOKABLE void setGeqGlobalBypass(bool bypass);
    Q_INVOKABLE void setPeqBypassed(bool bypass);
    Q_INVOKABLE void setHpfNodeEnabled(bool enabled);
    Q_INVOKABLE void setLpfNodeEnabled(bool enabled);
    Q_INVOKABLE bool resyncDspNodeBypass(const QString& nodeName);
    Q_INVOKABLE bool isDspNodeBypassed(const QString& nodeName);
    Q_INVOKABLE bool isDspNodeAppliedBypassed(const QString& nodeName) const noexcept;
    Q_INVOKABLE QVariantMap getDeckState(const QString& deckId);

    // Spec v3.8.3 §11.1 / Appendix A: Global & Per-DSP Preset Operations (P3 & P5)
    [[nodiscard]] QString activeGlobalPresetId() const;
    [[nodiscard]] QString activeGlobalPresetName() const;
    [[nodiscard]] bool isGlobalPresetModified() const;
    [[nodiscard]] QVariantMap dspPresetIndicators() const;
    Q_INVOKABLE bool isDspPresetApplied(const QString& target) const;
    Q_INVOKABLE QVariantList listGlobalDspPresets() const;
    Q_INVOKABLE QVariantList listPerDspPresets(const QString& target) const;
    Q_INVOKABLE bool hasUserDspPresetNamed(const QString& kind, const QString& target, const QString& name) const;
    Q_INVOKABLE bool isBuiltInDspPreset(const QString& kind, const QString& target, const QString& idOrName) const;
    Q_INVOKABLE bool applyGlobalDspPreset(const QString& idOrName);
    Q_INVOKABLE bool applyPerDspPreset(const QString& target, const QString& idOrName);
    Q_INVOKABLE bool applyDspPresetFromFile(const QString& filePath);
    Q_INVOKABLE bool saveGlobalDspPreset(const QString& name, bool overwrite = false);
    Q_INVOKABLE bool savePerDspPreset(const QString& target, const QString& name, bool overwrite = false);
    Q_INVOKABLE bool renameUserDspPreset(const QString& kind, const QString& target,
                                         const QString& id, const QString& newName,
                                         bool overwrite = false);
    Q_INVOKABLE bool deleteUserDspPreset(const QString& kind, const QString& target,
                                         const QString& idOrName);
    Q_INVOKABLE QVariantMap lastDspPresetReport() const;

    // Phase 3: External Insert Host Operations (Section 6.0)
    Q_INVOKABLE QVariantList discoverExternalInsertPorts() const;
    Q_INVOKABLE bool configureExternalInsert(const QString& clientName,
                                            const QString& sendL, const QString& sendR,
                                            const QString& returnL, const QString& returnR,
                                            double sampleRate = 0.0, int bufferSize = 0,
                                            int reportedLatencyFrames = 0);
    Q_INVOKABLE bool configureExternalInsertWithMetadata(const QString& clientName,
                                                        const QString& sendL, const QString& sendR,
                                                        const QString& returnL, const QString& returnR,
                                                        double sampleRate, int bufferSize,
                                                        int reportedLatencyFrames, bool latencyReported,
                                                        const QString& pluginName, const QString& pluginUri,
                                                        const QString& hostPresetReference);
    Q_INVOKABLE void setExternalInsertPosition(int positionIndex);
    [[nodiscard]] int externalInsertPosition() const noexcept;
    Q_INVOKABLE void setExternalInsertDisconnectPolicy(int policy);
    [[nodiscard]] int externalInsertDisconnectPolicy() const noexcept;
    Q_INVOKABLE void setExternalInsertBypass(bool bypass);
    Q_INVOKABLE void triggerInsertLatencyMeasurement();
    Q_INVOKABLE void calibrateInsertLatency();
    Q_INVOKABLE void recordInsertImpulseReturn(qint64 returnFrame);
    Q_INVOKABLE void beginInsertLatencyCalibration();
    Q_INVOKABLE bool recordInsertLatencyCalibrationSample(qint64 measuredFrames, double peakSnrDb);
    Q_INVOKABLE bool confirmRecommendedInsertLatency(bool allowOverrideUnsafe = false);
    Q_INVOKABLE void cancelInsertLatencyCalibration();
    Q_INVOKABLE QVariantMap getLatencyAlignmentReport() const;
    Q_INVOKABLE QVariantMap getExternalInsertStatus() const;
    Q_INVOKABLE void disconnectExternalInsert();
    Q_INVOKABLE bool reconnectExternalInsert();
    Q_INVOKABLE void resetInsertXRuns();
    Q_INVOKABLE void simulateInsertDisconnect();
    Q_INVOKABLE void simulateInsertRecovery();
    Q_INVOKABLE void setInsertDelayCompensation(int frames);
    [[nodiscard]] bool isExternalInsertCalibrating() const noexcept { return m_calActive; }
    [[nodiscard]] bool isExternalInsertSimulationMode() const noexcept;

    // Phase 3: Common Preset Management (Specification Section 11.1)
    Q_INVOKABLE QStringList listPresets(const QString& category) const;
    Q_INVOKABLE bool savePreset(const QString& category, const QString& presetName, const QVariantMap& data);
    Q_INVOKABLE QVariantMap loadPreset(const QString& category, const QString& presetName);
    Q_INVOKABLE bool deletePreset(const QString& category, const QString& presetName);
    Q_INVOKABLE bool applyPresetProfile(const QString& category, const QString& presetName);
    Q_INVOKABLE QVariantMap captureCurrentStateAsPreset(const QString& category);

    // Phase 4: Export & Render Operations (Section 5.11 & 20.1)
    Q_INVOKABLE void setRenderNodeEnabled(const QString& nodeName, bool enabled);
    [[nodiscard]] Q_INVOKABLE bool isRenderNodeEnabled(const QString& nodeName) const;
    Q_INVOKABLE void setSyncMasterDspToRender(bool sync);
    [[nodiscard]] bool syncMasterDspToRenderEnabled() const noexcept;
    Q_INVOKABLE QVariantMap verifyRenderTapConsistency();
    Q_INVOKABLE QString startTrackRender(const QString& deckId, int bitDepth = 1, int rateMode = 0, double customRate = 48000.0, bool applyDither = true, bool enableDsp = true);
    Q_INVOKABLE QString startAlbumRender(const QString& folderPath, int bitDepth = 1, int rateMode = 0, double customRate = 48000.0, bool applyDither = true, bool enableDsp = true);
    Q_INVOKABLE void cancelRender();
    Q_INVOKABLE void cancelAllRenders();
    Q_INVOKABLE void resetRenderStatus();
    Q_INVOKABLE void setRenderMonitorVolume(double volume);
    Q_INVOKABLE void setListeningVolume(double volume);
    Q_INVOKABLE void setRenderMonitorMuted(bool muted);
    Q_INVOKABLE void setListeningMuted(bool muted);
    Q_INVOKABLE bool auditionRenderedFile(const QString& targetDeckId, const QString& filePath, bool switchToRenderMonitor = true);
    Q_INVOKABLE QVariantMap getRenderStatus() const;

    // Session State Persistence
    Q_INVOKABLE QString defaultSessionStatePath() const;
    Q_INVOKABLE bool saveSessionState(const QString& filePath = QString()) const;
    Q_INVOKABLE bool restoreSessionState(const QString& filePath = QString());

    // Direct access to nodes and engine components for testing
    [[nodiscard]] std::shared_ptr<engine::insert::ExternalInsertNode> insertNode() const noexcept { return m_insertNode; }
    [[nodiscard]] engine::preset::PresetManager& presetManager() noexcept { return m_presetManager; }
    [[nodiscard]] engine::render::RenderEngine& renderEngine() noexcept { return m_renderEngine; }
    [[nodiscard]] engine::deck::DeckManager* deckManager() noexcept { return m_deckManager.get(); }
    [[nodiscard]] engine::backend::AudioOutput* audioOutput() noexcept { return m_audioOutput.get(); }
    [[nodiscard]] engine::backend::AudioControlManager* audioControlManager() const noexcept { return m_audioControlManager.get(); }
    [[nodiscard]] std::shared_ptr<engine::insert::BufferedInsertTransport> insertTransport() const noexcept { return m_insertTransport; }

    /// Test helper: Render an audio block through the controller's audio output callback
    void renderOfflineBlock(float* outInterleaved, size_t frames);
    void renderOfflineRawBlock(void* outInterleaved, size_t frames);

signals:
    void activeDeckChanged(const QString& deckId);
    void graphSampleRateChanged(double rate);
    void switchingModeChanged(int mode);
    void outputRoutingModeChanged(int path);
    void globalDspBypassChanged(bool bypass);
    void masterGainChanged(double gain);
    void masterGainLChanged(double gain);
    void masterGainRChanged(double gain);
    void masterBalanceChanged(double balance);
    void masterMuteChanged(bool muted);
    void masterMuteLChanged(bool muted);
    void masterMuteRChanged(bool muted);
    void monitorDimEnabledChanged(bool dimmed);
    void diagnosticsChanged();
    void audioBackendChanged(const QString& backendName);
    void deckStateChanged(const QString& deckId);
    void dspTelemetryChanged();
    void listeningPresetChanged(const QString& name);
    void peqParametersChanged();
    void geqParametersChanged();
    void filterParametersChanged();
    void compressorParametersChanged();
    void limiterParametersChanged();
    void reverbParametersChanged();
    void dspTrimsChanged();
    void externalInsertStatusChanged();
    void dspQueueOverflowWarning(int commandType);
    void presetManagementChanged();
    void dspPresetStateChanged();
    void renderStateChanged();
    void renderMonitorVolumeChanged(double volume);
    void listeningVolumeChanged(double volume);
    void renderMonitorMutedChanged(bool muted);
    void listeningMutedChanged(bool muted);
    void presetImportWarning(const QString& target, int requestedLeft, int requestedRight,
                             int activeLimit, int overflowLeft, int overflowRight,
                             const QString& message);
    void renderTaskStarted(const QString& taskId, const QString& name, int totalTracks);
    void renderTaskFinished(const QString& taskId, int status, const QString& outputDir, const QString& errorMessage);
    void levelMatchStateChanged();
    void levelMatchCompleted(bool success, const QString& summary);
    void deckWaveformReady(const QString& deckId);
    void waveformCacheChanged();
    void deckWorkspaceImportFinished(const QString& deckId, bool ok,
                                     const QString& statusText,
                                     const QString& message);
    void folderSessionEnded(const QString& deckId, const QString& reason,
                            const QString& folderPath, int trackIndex,
                            int totalTracks);

private:
    void syncMasterDspToRenderEngine();
    [[nodiscard]] engine::preset::GlobalDspPresetData captureCurrentGlobalDspState() const;
    void applyPeqPresetData(const engine::preset::PeqPresetData& peq);
    void applyGeqPresetData(const engine::preset::GeqPresetData& geq);
    void applyHpfPresetData(const engine::preset::HpfPresetData& hpf, const bool* optEnabled = nullptr);
    void applyLpfPresetData(const engine::preset::LpfPresetData& lpf, const bool* optEnabled = nullptr);
    void applyCompressorPresetData(const engine::preset::CompressorPresetData& comp);
    void applyLimiterPresetData(const engine::preset::LimiterPresetData& lim);
    void applyReverbPresetData(const engine::preset::ReverbPresetData& rev);
    void applyGeqEffectiveBypassToPipeline();
    void notifyManualDspParamChanged();
    void notifyDspNodeBypassChanged();
    bool applyValidatedDspPresetDocument(const engine::preset::DspPresetDocument& doc,
                                         const engine::preset::PresetValidationReport& report);
    bool applyMasterDspJson(const QJsonObject& obj);
    bool applyPeqBandsFromJson(const QJsonObject& peqObj, const QString& presetLabel);
    void recordPresetImportReport(const QString& targetKey, const QString& targetDisplay,
                                  const QString& logOperation, const QString& presetLabel,
                                  const ParametricEQ::BandImportReport& report,
                                  const QStringList& unsupportedFilters, double preampDb);
    bool applyListeningCorrectionJson(const QJsonObject& obj, const QString& presetLabel);
    bool applyExternalInsertJson(const QJsonObject& obj);
    bool applyAudioDeviceJson(const QJsonObject& obj);
    [[nodiscard]] static int resolveDspTrimIndex(const QString& dspName) noexcept;
    [[nodiscard]] bool areAllDspTrimsZero() const noexcept;
    std::atomic<bool>* renderNodeFlagPtr(const QString& nodeName) noexcept;
    const std::atomic<bool>* renderNodeFlagPtr(const QString& nodeName) const noexcept;
    void followActiveDeckSampleRateIfDirectMode();
    void recordSeekEvent(
        char deckChar,
        int64_t beforeFrame,
        const engine::decoder::SeekResult& res,
        const QString& trigger,
        double durationMs,
        const std::string& trackPath);
    void checkAudioHealthAndGraph();
    void pollExternalInsertHealth();
    void syncExternalInsertBypassToPipeline(bool bypass);
    void pollHardwareCalibrationProbe();
    void finishHardwareCalibration(bool restoreBypass, bool cancelSession = false);
    void applyInitialSettingsIfLoaded();
    bool setupAudioBackend(const QString& backendName, bool fallbackToOfflineOnStartFailure, bool preserveTargetDevice);

    std::unique_ptr<engine::deck::DeckManager> m_deckManager;
    std::shared_ptr<engine::pipeline::AudioPipeline> m_pipeline;
    std::unique_ptr<engine::backend::AudioOutput> m_audioOutput;
    std::unique_ptr<engine::backend::AudioControlManager> m_audioControlManager;
    std::shared_ptr<engine::insert::BufferedInsertTransport> m_insertTransport;
    QString m_activeAudioBackend{"PipeWire"};
    bool m_offlineMode{false};

    // Control Thread UI Parameter Snapshots & Response Curve Calculators (P0-21)
    // Strictly owned by Control Thread and completely separated from running AudioChain DSP nodes.
    ParametricEQ m_uiPeq;
    GraphicEQ m_uiGeq;
    HighLowPassFilter m_uiFilter;
    Compressor m_uiComp;
    PeakLimiter m_uiLimiter;
    AlgorithmicReverb m_uiReverb;
    ParametricEQ m_uiListeningCorrection;
    QString m_listeningPresetName{"Monitor Flat"};

    // Node bypass state tracking for Control Thread / UI display
    std::atomic<bool> m_peqBypassed{true};
    std::atomic<bool> m_geqBypassed{true};
    std::atomic<bool> m_geqGlobalBypass{false}; // Spec §11.1 temporary GEQ bypass (excluded from presets)
    std::atomic<bool> m_filterBypassed{true};
    std::atomic<bool> m_compBypassed{true};
    std::atomic<bool> m_insertBypassed{true};
    std::atomic<bool> m_limiterBypassed{true};
    std::atomic<bool> m_reverbBypassed{true};
    std::atomic<bool> m_listeningCorrectionBypassed{false};

    // Spec v3.8.3 §11.1 / P5: Canonical Preset State Tracker
    engine::preset::PresetStateTracker m_presetStateTracker;
    engine::preset::PresetValidationReport m_lastDspPresetReport;
    mutable std::mutex m_dspPresetStateMutex;
    bool m_applyingDspPreset{false};

    // Phase 3: External Insert Node
    std::shared_ptr<engine::insert::ExternalInsertNode> m_insertNode;

    // Phase 3: Common Preset Manager
    engine::preset::PresetManager& m_presetManager;

    // Phase 4: Export & Render Engine
    engine::render::RenderEngine& m_renderEngine;

    std::atomic<int> m_switchingMode{0}; // 0 = Normal, 1 = Comparative
    std::atomic<int> m_outputRoutingMode{0}; // 0 = Listening, 1 = RenderMonitor
    std::atomic<bool> m_globalDspBypass{false};
    std::atomic<bool> m_globalBypassDesynced{false};
    std::atomic<double> m_masterGain{1.0};
    std::atomic<double> m_masterGainL{1.0};
    std::atomic<double> m_masterGainR{1.0};
    std::atomic<double> m_masterBalance{0.0};
    std::atomic<bool> m_masterMute{false};
    std::atomic<bool> m_masterMuteL{false};
    std::atomic<bool> m_masterMuteR{false};
    std::atomic<bool> m_directModeEnabled{false};
    std::atomic<double> m_fixedGraphSampleRate{48000.0};
    std::atomic<int> m_externalInsertPosition{4};
    std::array<std::atomic<uint8_t>, 7> m_masterNodeIndices{};
    std::atomic<bool> m_syncMasterDspToRender{true};
    std::atomic<bool> m_renderIncludePeq{true};
    std::atomic<bool> m_renderIncludeGeq{true};
    std::atomic<bool> m_renderIncludeFilter{true};
    std::atomic<bool> m_renderIncludeCompressor{true};
    std::atomic<bool> m_renderIncludeReverb{true};
    std::atomic<bool> m_renderIncludeLimiter{true};
    std::array<std::atomic<double>, 7> m_dspInputTrimDb{};
    std::array<std::atomic<double>, 7> m_dspOutputTrimDb{};

    // Private helpers for DSP node lookup and signal dispatch (P1-1, P1-2)
    void emitDspNodeSignal(const QString& nodeName);
    bool findDspNode(const QString& nodeName, uint8_t& outChain, uint8_t& outIndex) const noexcept;
    void updateSourceBitDepthFromActiveDeck();

    std::atomic<double> m_reverbMaxTailRetentionSeconds{5.0};
    std::atomic<double> m_reverbMaxProcessingTimeMs{0.0};
    mutable std::mutex m_presetReportMutex;
    QVariantMap m_lastPresetImportReport;

    std::atomic<bool> m_levelMatchInProgress{false};
    std::atomic<bool> m_levelMatchCancel{false};
    std::thread m_levelMatchThread;

    std::unique_ptr<engine::audio::WaveformExtractor> m_waveformExtractor;
    uint64_t m_lastSpectrumWritePos{0};
    int m_spectrumStaleCount{0};
    int m_lastPolledInsertState{-1};
    QString m_graphicsBackend{QStringLiteral("Unknown")};
    QString m_gpuRendererName{QStringLiteral("Unknown")};

    // Real-Time Safe Parameter Smoothers for master gain (eliminates zipper noise)
    engine::dsp::ParameterSmoother m_gainSmootherL{1.0, 15.0};
    engine::dsp::ParameterSmoother m_gainSmootherR{1.0, 15.0};

    // Pre-allocated Audio Thread scratch buffers (strictly ZERO dynamic allocation in RT audio callback)
    alignas(64) std::array<double, 4096> m_audioInL{};
    alignas(64) std::array<double, 4096> m_audioInR{};

    // Observability session & health tracking
    bool m_sessionStartRecorded{false};
    bool m_sessionEndRecorded{false};
    std::chrono::steady_clock::time_point m_sessionStartTime{};
    uint64_t m_advanceSeq{0};
    uint64_t m_lastReportedXruns{0};
    uint64_t m_pendingXrunDelta{0};
    std::chrono::steady_clock::time_point m_lastXrunLogTime{};
    uint32_t m_lastGraphSampleRate{0};
    uint32_t m_lastGraphBufferFrames{0};
    int m_lastDirectModeStatus{-1};
    int m_lastLoggedBitPerfectModeState{-1};
    int m_healthCheckTick{0};
    std::array<uint64_t, 4> m_lastDeckUnderruns{0, 0, 0, 0};
    size_t m_lastDroppedLogCount{0};

    // Real-time multi-sample hardware latency calibration state (Control Thread)
    QTimer* m_calProbeTimer{nullptr};
    uint32_t m_calTargetProbes{3};
    uint32_t m_calCompletedProbes{0};
    bool m_calActive{false};
    int m_calSettleTicks{0};
    bool m_calRestoreBypass{false};
    bool m_applyingInitialSettings{false};
};

} // namespace desk32::core
