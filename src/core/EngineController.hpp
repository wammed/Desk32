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
#include "engine/backend/PipeWireAudioOutput.hpp"
#include "core/log/LogService.hpp"

namespace desk32::core {

/// Unified Studio Console Controller bridging QML UI with Engine Subsystems
/// Coordinates DeckManager, AudioPipeline, PipeWireAudioOutput, and LogService.
class EngineController : public QObject {
    Q_OBJECT

    Q_PROPERTY(QString activeDeckId READ activeDeckId NOTIFY activeDeckChanged)
    Q_PROPERTY(double graphSampleRate READ graphSampleRate NOTIFY graphSampleRateChanged)
    Q_PROPERTY(int switchingMode READ switchingMode WRITE setSwitchingMode NOTIFY switchingModeChanged)
    Q_PROPERTY(int outputRoutingMode READ outputRoutingMode WRITE setOutputRoutingMode NOTIFY outputRoutingModeChanged)
    Q_PROPERTY(bool globalDspBypass READ globalDspBypass WRITE setGlobalDspBypass NOTIFY globalDspBypassChanged)
    Q_PROPERTY(double masterGain READ masterGain WRITE setMasterGain NOTIFY masterGainChanged)
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

public:
    static EngineController& instance();

    explicit EngineController(QObject* parent = nullptr);
    ~EngineController() override;

    bool initialize();

    // Property getters
    [[nodiscard]] QString activeDeckId() const;
    [[nodiscard]] double graphSampleRate() const;
    [[nodiscard]] int switchingMode() const;
    [[nodiscard]] int outputRoutingMode() const;
    [[nodiscard]] bool globalDspBypass() const;
    [[nodiscard]] double masterGain() const;
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

    // Property setters
    void setSwitchingMode(int mode);
    void setOutputRoutingMode(int path);
    void setGlobalDspBypass(bool bypass);
    void setMasterGain(double gain);

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
    Q_INVOKABLE bool isDspNodeBypassed(const QString& nodeName);
    Q_INVOKABLE QVariantMap getDeckState(const QString& deckId);

signals:
    void activeDeckChanged(const QString& deckId);
    void graphSampleRateChanged(double rate);
    void switchingModeChanged(int mode);
    void outputRoutingModeChanged(int path);
    void globalDspBypassChanged(bool bypass);
    void masterGainChanged(double gain);
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

private:
    std::unique_ptr<engine::deck::DeckManager> m_deckManager;
    std::shared_ptr<engine::pipeline::AudioPipeline> m_pipeline;
    std::unique_ptr<engine::backend::PipeWireAudioOutput> m_audioOutput;

    // DSP Nodes
    std::shared_ptr<engine::pipeline::ParametricEqNode> m_peqNode;
    std::shared_ptr<engine::pipeline::GraphicEqNode> m_geqNode;
    std::shared_ptr<engine::pipeline::HighLowPassNode> m_filterNode;
    std::shared_ptr<engine::pipeline::CompressorNode> m_compNode;
    std::shared_ptr<engine::pipeline::PeakLimiterNode> m_limiterNode;
    std::shared_ptr<engine::pipeline::ReverbNode> m_reverbNode;

    // Listening Correction PEQ Node (downstream of Logical Render Tap, Section 5.10 & 5.11)
    std::shared_ptr<engine::pipeline::ParametricEqNode> m_listeningCorrectionNode;
    QString m_listeningPresetName{"Monitor Flat"};

    std::atomic<int> m_switchingMode{0}; // 0 = Normal, 1 = Comparative
    std::atomic<int> m_outputRoutingMode{0}; // 0 = Listening, 1 = RenderMonitor
    std::atomic<bool> m_globalDspBypass{false};
    std::atomic<double> m_masterGain{1.0};

    // Pre-allocated Audio Thread scratch buffers (strictly ZERO dynamic allocation in RT audio callback)
    alignas(64) std::array<double, 4096> m_audioInL{};
    alignas(64) std::array<double, 4096> m_audioInR{};
};

} // namespace desk32::core

