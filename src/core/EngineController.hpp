#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QVariantList>
#include <memory>
#include <array>
#include <atomic>
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
#include "engine/render/RenderEngine.hpp"
#include "engine/backend/PipeWireAudioOutput.hpp"
#include "core/log/LogService.hpp"

namespace desk32::core {

/// Unified Studio Console Controller bridging QML UI with Engine Subsystems
/// Coordinates DeckManager, AudioPipeline, PipeWireAudioOutput, LogService, ExternalInsert, and PresetManager.
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
    Q_PROPERTY(bool masterMute READ isMasterMute WRITE setMasterMute NOTIFY masterMuteChanged)
    Q_PROPERTY(double masterPeakL READ masterPeakL NOTIFY dspTelemetryChanged)
    Q_PROPERTY(double masterPeakR READ masterPeakR NOTIFY dspTelemetryChanged)
    Q_PROPERTY(double masterRmsL READ masterRmsL NOTIFY dspTelemetryChanged)
    Q_PROPERTY(double masterRmsR READ masterRmsR NOTIFY dspTelemetryChanged)
    Q_PROPERTY(bool masterClipped READ masterClipped NOTIFY dspTelemetryChanged)
    Q_PROPERTY(double masterHeadroomDb READ masterHeadroomDb NOTIFY dspTelemetryChanged)
    Q_PROPERTY(int xrunCount READ xrunCount NOTIFY diagnosticsChanged)
    Q_PROPERTY(bool isDirectMode READ isDirectMode NOTIFY diagnosticsChanged)
    Q_PROPERTY(int directModeStatus READ directModeStatus NOTIFY diagnosticsChanged)
    Q_PROPERTY(int backendStatus READ backendStatus NOTIFY diagnosticsChanged)
    Q_PROPERTY(QString backendStatusText READ backendStatusText NOTIFY diagnosticsChanged)

    // Phase 2: Dynamic DSP Telemetry & Status Properties
    Q_PROPERTY(double compressorGainReductionDb READ compressorGainReductionDb NOTIFY dspTelemetryChanged)
    Q_PROPERTY(double limiterGainReductionDb READ limiterGainReductionDb NOTIFY dspTelemetryChanged)
    Q_PROPERTY(int limiterLatencyFrames READ limiterLatencyFrames NOTIFY dspTelemetryChanged)
    Q_PROPERTY(QString listeningPresetName READ listeningPresetName NOTIFY listeningPresetChanged)
    Q_PROPERTY(bool listeningCorrectionActive READ listeningCorrectionActive NOTIFY outputRoutingModeChanged)

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

    // Phase 4: Export & Render Telemetry (Section 5.11 & 20.1)
    Q_PROPERTY(bool isRendering READ isRendering NOTIFY renderStateChanged)
    Q_PROPERTY(double renderProgress READ renderProgress NOTIFY renderStateChanged)
    Q_PROPERTY(QString renderCurrentTrackName READ renderCurrentTrackName NOTIFY renderStateChanged)
    Q_PROPERTY(int renderTrackIndex READ renderTrackIndex NOTIFY renderStateChanged)
    Q_PROPERTY(int renderTotalTracks READ renderTotalTracks NOTIFY renderStateChanged)
    Q_PROPERTY(QString renderLastOutputDir READ renderLastOutputDir NOTIFY renderStateChanged)

public:
    static EngineController& instance();

    explicit EngineController(QObject* parent = nullptr);
    ~EngineController() override;

    bool initialize(bool offlineMode = false);

    // Property getters
    [[nodiscard]] QString activeDeckId() const;
    [[nodiscard]] double graphSampleRate() const;
    [[nodiscard]] int switchingMode() const;
    [[nodiscard]] int outputRoutingMode() const;
    [[nodiscard]] bool globalDspBypass() const;
    [[nodiscard]] double masterGain() const;
    [[nodiscard]] double masterGainL() const;
    [[nodiscard]] double masterGainR() const;
    [[nodiscard]] bool isMasterMute() const;
    [[nodiscard]] double masterPeakL() const;
    [[nodiscard]] double masterPeakR() const;
    [[nodiscard]] double masterRmsL() const;
    [[nodiscard]] double masterRmsR() const;
    [[nodiscard]] bool masterClipped() const;
    [[nodiscard]] double masterHeadroomDb() const;
    [[nodiscard]] int xrunCount() const;
    [[nodiscard]] bool isDirectMode() const;
    [[nodiscard]] int directModeStatus() const;
    [[nodiscard]] int backendStatus() const;
    [[nodiscard]] QString backendStatusText() const;

    // Phase 2 Getters
    [[nodiscard]] double compressorGainReductionDb() const;
    [[nodiscard]] double limiterGainReductionDb() const;
    [[nodiscard]] int limiterLatencyFrames() const;
    [[nodiscard]] QString listeningPresetName() const;
    [[nodiscard]] bool listeningCorrectionActive() const;

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

    // Phase 4 Getters
    [[nodiscard]] bool isRendering() const;
    [[nodiscard]] double renderProgress() const;
    [[nodiscard]] QString renderCurrentTrackName() const;
    [[nodiscard]] int renderTrackIndex() const;
    [[nodiscard]] int renderTotalTracks() const;
    [[nodiscard]] QString renderLastOutputDir() const;
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
    Q_INVOKABLE void setMasterMute(bool mute);

    // QML-invokable Deck Operations
    Q_INVOKABLE void play(const QString& deckId);
    Q_INVOKABLE void pause(const QString& deckId);
    Q_INVOKABLE void stop(const QString& deckId);
    Q_INVOKABLE void seek(const QString& deckId, qint64 targetFrame);
    Q_INVOKABLE void switchDeck(const QString& targetDeckId, int mode);
    Q_INVOKABLE void quickSwitchAB();
    Q_INVOKABLE void setDeckLocked(const QString& deckId, bool locked);
    Q_INVOKABLE bool loadTrack(const QString& deckId, const QString& filePath);

    // QML-invokable Folder Session Operations
    Q_INVOKABLE bool startFolderSession(const QString& deckId, const QString& folderPath, const QStringList& fileNames);
    Q_INVOKABLE bool advanceFolderNext(const QString& deckId);
    Q_INVOKABLE bool retreatFolderPrevious(const QString& deckId);

    // QML-invokable DSP & EQ Console Operations
    Q_INVOKABLE void setGeqBandGain(int bandIndex, double gainDb);
    Q_INVOKABLE double getGeqBandGain(int bandIndex);
    Q_INVOKABLE void resetAllGeqBands();
    Q_INVOKABLE QVariantList getGeqCurve(int numPoints = 128);

    // 24-Band PEQ Operations (Left 12 + Right 12)
    Q_INVOKABLE QVariantMap getPeqBand(bool isRightChannel, int bandIndex);
    Q_INVOKABLE void setPeqBand(bool isRightChannel, int bandIndex, double frequency, double gainDb, double q, int filterType, bool enabled);
    Q_INVOKABLE void setPeqBandEnabled(bool isRightChannel, int bandIndex, bool enabled);
    Q_INVOKABLE QVariantList getPeqCurve(bool isRightChannel, int numPoints = 128);
    Q_INVOKABLE void applyPeqPreset(const QString& presetName);

    // Dynamic Processors & Filters
    Q_INVOKABLE void setFilterParams(double hpfFreq, bool hpfOn, int hpfSlope, double lpfFreq, bool lpfOn, int lpfSlope);
    Q_INVOKABLE QVariantMap getFilterParams();
    Q_INVOKABLE QVariantList getFilterCurve(int numPoints = 128);

    Q_INVOKABLE void setCompressorParams(double thresholdDb, double ratio, double attackMs, double releaseMs, double makeupDb, double kneeDb);
    Q_INVOKABLE QVariantMap getCompressorParams();

    Q_INVOKABLE void setLimiterParams(double ceilingDb, double releaseMs, double lookaheadMs);
    Q_INVOKABLE QVariantMap getLimiterParams();

    Q_INVOKABLE void setReverbParams(double roomSize, double damping, double wetLevel, double predelayMs, bool tailPreserving);
    Q_INVOKABLE QVariantMap getReverbParams();

    // Listening Correction Presets (Section 5.10)
    Q_INVOKABLE QStringList availableListeningPresets();
    Q_INVOKABLE void setListeningPreset(const QString& presetName);

    Q_INVOKABLE void setDspNodeBypass(const QString& nodeName, bool bypass);
    Q_INVOKABLE bool resyncDspNodeBypass(const QString& nodeName);
    Q_INVOKABLE bool isDspNodeBypassed(const QString& nodeName);
    Q_INVOKABLE bool isDspNodeAppliedBypassed(const QString& nodeName) const noexcept;
    Q_INVOKABLE QVariantMap getDeckState(const QString& deckId);

    // Phase 3: External Insert Host Operations (Section 6.0)
    Q_INVOKABLE bool configureExternalInsert(const QString& clientName,
                                            const QString& sendL, const QString& sendR,
                                            const QString& returnL, const QString& returnR,
                                            double sampleRate = 0.0, int bufferSize = 0,
                                            int reportedLatencyFrames = 0);
    Q_INVOKABLE void setExternalInsertBypass(bool bypass);
    Q_INVOKABLE void triggerInsertLatencyMeasurement();
    Q_INVOKABLE void calibrateInsertLatency();
    Q_INVOKABLE void recordInsertImpulseReturn(qint64 returnFrame);
    Q_INVOKABLE QVariantMap getExternalInsertStatus() const;
    Q_INVOKABLE void disconnectExternalInsert();
    Q_INVOKABLE void resetInsertXRuns();
    Q_INVOKABLE void simulateInsertDisconnect();
    Q_INVOKABLE void simulateInsertRecovery();
    Q_INVOKABLE void setInsertDelayCompensation(int frames);

    // Phase 3: Common Preset Management (Specification Section 11.1)
    QStringList listPresets(const QString& category) const;
    Q_INVOKABLE bool savePreset(const QString& category, const QString& presetName, const QVariantMap& data);
    Q_INVOKABLE QVariantMap loadPreset(const QString& category, const QString& presetName);
    Q_INVOKABLE bool deletePreset(const QString& category, const QString& presetName);
    Q_INVOKABLE bool applyPresetProfile(const QString& category, const QString& presetName);
    Q_INVOKABLE QVariantMap captureCurrentStateAsPreset(const QString& category);

    // Phase 4: Export & Render Operations (Section 5.11 & 20.1)
    Q_INVOKABLE QString startTrackRender(const QString& deckId, int bitDepth = 1, int rateMode = 0, double customRate = 48000.0, bool applyDither = true, bool enableDsp = true);
    Q_INVOKABLE QString startAlbumRender(const QString& folderPath, int bitDepth = 1, int rateMode = 0, double customRate = 48000.0, bool applyDither = true, bool enableDsp = true);
    Q_INVOKABLE void cancelRender();
    Q_INVOKABLE bool auditionRenderedFile(const QString& targetDeckId, const QString& filePath);
    Q_INVOKABLE QVariantMap getRenderStatus() const;

    // Direct access to nodes and engine components for testing
    [[nodiscard]] std::shared_ptr<engine::insert::ExternalInsertNode> insertNode() const noexcept { return m_insertNode; }
    [[nodiscard]] engine::preset::PresetManager& presetManager() noexcept { return m_presetManager; }
    [[nodiscard]] engine::render::RenderEngine& renderEngine() noexcept { return m_renderEngine; }
    [[nodiscard]] engine::deck::DeckManager* deckManager() noexcept { return m_deckManager.get(); }
    [[nodiscard]] engine::backend::PipeWireAudioOutput* audioOutput() noexcept { return m_audioOutput.get(); }

    /// Test helper: Render an audio block through the controller's audio output callback
    void renderOfflineBlock(float* outInterleaved, size_t frames);

signals:
    void activeDeckChanged(const QString& deckId);
    void graphSampleRateChanged(double rate);
    void switchingModeChanged(int mode);
    void outputRoutingModeChanged(int path);
    void globalDspBypassChanged(bool bypass);
    void masterGainChanged(double gain);
    void masterGainLChanged(double gain);
    void masterGainRChanged(double gain);
    void masterMuteChanged(bool muted);
    void diagnosticsChanged();
    void deckStateChanged(const QString& deckId);
    void dspTelemetryChanged();
    void listeningPresetChanged(const QString& name);
    void peqParametersChanged();
    void geqParametersChanged();
    void filterParametersChanged();
    void compressorParametersChanged();
    void limiterParametersChanged();
    void reverbParametersChanged();
    void externalInsertStatusChanged();
    void dspQueueOverflowWarning(int commandType);
    void presetManagementChanged();
    void renderStateChanged();
    void renderTaskFinished(const QString& taskId, int status, const QString& outputDir, const QString& errorMessage);

private:
    void syncMasterDspToRenderEngine();

    std::unique_ptr<engine::deck::DeckManager> m_deckManager;
    std::shared_ptr<engine::pipeline::AudioPipeline> m_pipeline;
    std::unique_ptr<engine::backend::PipeWireAudioOutput> m_audioOutput;

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
    std::atomic<bool> m_filterBypassed{true};
    std::atomic<bool> m_compBypassed{true};
    std::atomic<bool> m_insertBypassed{true};
    std::atomic<bool> m_limiterBypassed{true};
    std::atomic<bool> m_reverbBypassed{true};
    std::atomic<bool> m_listeningCorrectionBypassed{false};

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
    std::atomic<bool> m_masterMute{false};

    // Private helpers for DSP node lookup and signal dispatch (P1-1, P1-2)
    void emitDspNodeSignal(const QString& nodeName);
    bool findDspNode(const QString& nodeName, uint8_t& outChain, uint8_t& outIndex) const noexcept;

    // Real-Time Safe Parameter Smoothers for master gain (eliminates zipper noise)
    engine::dsp::ParameterSmoother m_gainSmootherL{1.0, 15.0};
    engine::dsp::ParameterSmoother m_gainSmootherR{1.0, 15.0};

    // Pre-allocated Audio Thread scratch buffers (strictly ZERO dynamic allocation in RT audio callback)
    alignas(64) std::array<double, 4096> m_audioInL{};
    alignas(64) std::array<double, 4096> m_audioInR{};
};

} // namespace desk32::core
