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
{}

EngineController::~EngineController() {
    if (m_audioOutput) {
        m_audioOutput->stop();
    }
}

bool EngineController::initialize() {
    // 1. Initialize 4-Deck Manager
    m_deckManager = std::make_unique<engine::deck::DeckManager>(48000.0);

    // 2. Initialize DSP Nodes
    m_peqNode = std::make_shared<engine::pipeline::ParametricEqNode>();
    m_geqNode = std::make_shared<engine::pipeline::GraphicEqNode>();
    m_filterNode = std::make_shared<engine::pipeline::HighLowPassNode>();
    m_compNode = std::make_shared<engine::pipeline::CompressorNode>();
    m_limiterNode = std::make_shared<engine::pipeline::PeakLimiterNode>();
    m_reverbNode = std::make_shared<engine::pipeline::ReverbNode>();

    // 3. Construct Master and Listening Chains
    auto masterChain = std::make_unique<engine::pipeline::AudioChain>("Master Chain", engine::pipeline::ChainPurpose::MasteringChain);
    masterChain->appendNode(m_peqNode);
    masterChain->appendNode(m_geqNode);
    masterChain->appendNode(m_filterNode);
    masterChain->appendNode(m_compNode);
    masterChain->appendNode(m_limiterNode);

    auto listeningChain = std::make_unique<engine::pipeline::AudioChain>("Listening Chain", engine::pipeline::ChainPurpose::ListeningCorrectionChain);
    listeningChain->appendNode(m_reverbNode);

    // 4. Initialize Audio Pipeline with Logical Render Tap
    m_pipeline = std::make_shared<engine::pipeline::AudioPipeline>(48000.0);
    m_pipeline->swapMasterChain(std::move(masterChain));
    m_pipeline->swapListeningChain(std::move(listeningChain));

    // 5. Initialize Native PipeWire AudioOutput
    m_audioOutput = std::make_unique<engine::backend::PipeWireAudioOutput>();
    m_audioOutput->initialize(48000, 256);

    // Wire real-time audio callback
    m_audioOutput->setAudioCallback([this](float* outInterleaved, size_t frames) {
        if (!m_deckManager || !m_pipeline) {
            std::fill_n(outInterleaved, frames * 2, 0.0f);
            return;
        }

        std::vector<double> inL(frames);
        std::vector<double> inR(frames);
        m_deckManager->renderActiveBlock(inL.data(), inR.data(), frames);

        // Apply master gain
        if (m_masterGain != 1.0) {
            for (size_t i = 0; i < frames; ++i) {
                inL[i] *= m_masterGain;
                inR[i] *= m_masterGain;
            }
        }

        // Set output mode at Logical Render Tap
        m_pipeline->setOutputMode(
            m_outputRoutingMode == 1 ? engine::routing::OutputPathMode::RenderMonitorPath
                                     : engine::routing::OutputPathMode::ListeningPath
        );

        // Process through AudioPipeline directly into Float32 interleaved PCM with TPDF dither
        m_pipeline->process(inL.data(), inR.data(), frames, outInterleaved, engine::format::SampleFormat::Float32, true);
    });

    m_audioOutput->start();

    // Periodic check for seamless track auto-advancement in Folder Sessions
    auto* trackTimer = new QTimer(this);
    connect(trackTimer, &QTimer::timeout, this, [this]() {
        if (!m_deckManager) return;
        for (char c : {'A', 'B', 'C', 'D'}) {
            auto* d = m_deckManager->deck(c);
            if (d && d->checkAndResetTrackChanged()) {
                emit deckStateChanged(QString(c));
            }
        }
    });
    trackTimer->start(50);

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
    return m_switchingMode;
}

int EngineController::outputRoutingMode() const {
    return m_outputRoutingMode;
}

bool EngineController::globalDspBypass() const {
    return m_globalDspBypass;
}

double EngineController::masterGain() const {
    return m_masterGain;
}

int EngineController::xrunCount() const {
    if (!m_audioOutput) return 0;
    return static_cast<int>(m_audioOutput->diagnostics().xrunCount);
}

bool EngineController::isDirectMode() const {
    if (!m_audioOutput) return false;
    return m_audioOutput->diagnostics().isDirectMode;
}

void EngineController::setSwitchingMode(int mode) {
    if (m_switchingMode != mode) {
        m_switchingMode = mode;
        emit switchingModeChanged(mode);
    }
}

void EngineController::setOutputRoutingMode(int path) {
    if (m_outputRoutingMode != path) {
        m_outputRoutingMode = path;
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
    if (m_globalDspBypass != bypass) {
        m_globalDspBypass = bypass;
        if (m_peqNode) m_peqNode->setBypassed(bypass);
        if (m_geqNode) m_geqNode->setBypassed(bypass);
        if (m_filterNode) m_filterNode->setBypassed(bypass);
        if (m_compNode) m_compNode->setBypassed(bypass);
        if (m_limiterNode) m_limiterNode->setBypassed(bypass);
        if (m_reverbNode) m_reverbNode->setBypassed(bypass);
        emit globalDspBypassChanged(bypass);
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
    if (m_masterGain != gain) {
        m_masterGain = std::clamp(gain, 0.0, 2.0);
        emit masterGainChanged(m_masterGain);
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
    if (m_geqNode) {
        m_geqNode->dsp().setBandGain(static_cast<size_t>(bandIndex), gainDb);
    }
}

void EngineController::setDspNodeBypass(const QString& nodeName, bool bypass) {
    if (nodeName == "PEQ" && m_peqNode) m_peqNode->setBypassed(bypass);
    else if (nodeName == "GEQ" && m_geqNode) m_geqNode->setBypassed(bypass);
    else if (nodeName == "Filter" && m_filterNode) m_filterNode->setBypassed(bypass);
    else if (nodeName == "Compressor" && m_compNode) m_compNode->setBypassed(bypass);
    else if (nodeName == "Limiter" && m_limiterNode) m_limiterNode->setBypassed(bypass);
    else if (nodeName == "Reverb" && m_reverbNode) m_reverbNode->setBypassed(bypass);
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

    const auto* meta = d->metadata();
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

} // namespace desk32::core
