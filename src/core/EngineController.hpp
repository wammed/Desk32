#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QVariantList>
#include <memory>
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
    Q_INVOKABLE void setDspNodeBypass(const QString& nodeName, bool bypass);
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

    int m_switchingMode = 0; // 0 = Normal, 1 = Comparative
    int m_outputRoutingMode = 0; // 0 = Listening, 1 = RenderMonitor
    bool m_globalDspBypass = false;
    double m_masterGain = 1.0;
};

} // namespace desk32::core

