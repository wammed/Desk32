#include <QTest>
#include <QObject>
#include "engine/backend/PipeWireAudioOutput.hpp"
#include "engine/pipeline/AudioPipeline.hpp"

using namespace desk32::engine::backend;
using namespace desk32::engine::pipeline;

class TestPipeWireOutput : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void testInitializationAndShutdown();
    void testAudioPipelineIntegration();
    void testDiagnosticsAndXrunTracking();
    void cleanupTestCase();
};

void TestPipeWireOutput::initTestCase()
{
    qDebug() << "Initializing TestPipeWireOutput (Phase 1.2 PipeWire Output Implementation)...";
}

void TestPipeWireOutput::testInitializationAndShutdown()
{
    PipeWireAudioOutput output;

    // 1. Initialize
    bool initOk = output.initialize(48000, 256);
    QVERIFY(initOk);

    // 2. Diagnostics initial state
    auto diag = output.diagnostics();
    QCOMPARE(diag.sampleRate, 48000u);
    QCOMPARE(diag.channels, 2u);
    QCOMPARE(diag.bufferFrames, 256u);
    QCOMPARE(diag.xrunCount, 0ull);
    QCOMPARE(diag.totalFramesProcessed, 0ull);

    // 3. Connect/Start attempt (graceful in both desktop and headless CI)
    bool started = output.start();
    if (started) {
        qDebug() << "PipeWire native output stream connected to active PipeWire server!";
        QVERIFY(output.state() == StreamState::Connecting || output.state() == StreamState::Streaming);
        output.pause();
        output.stop();
    } else {
        qDebug() << "PipeWire server not running in sandbox/CI (gracefully handled).";
    }

    output.shutdown();
    QCOMPARE(output.state(), StreamState::Unconnected);
}

void TestPipeWireOutput::testAudioPipelineIntegration()
{
    // Test end-to-end audio delivery from AudioPipeline into PipeWireAudioOutput
    AudioPipeline pipeline(48000.0);
    PipeWireAudioOutput output;
    QVERIFY(output.initialize(48000, 256));

    // Enable offline simulation mode for deterministic callback verification
    output.setOfflineSimulationMode(true);
    QVERIFY(output.start());

    const size_t frames = 256;
    std::vector<double> inL(frames, 0.4);
    std::vector<double> inR(frames, -0.4);

    // Wire up callback
    output.setAudioCallback([&](float* outPcm, size_t nFrames) {
        std::vector<double> l = inL;
        std::vector<double> r = inR;
        pipeline.process(l.data(), r.data(), nFrames, outPcm, SampleFormat::Float32, false);
    });

    std::vector<float> pcmBuffer(frames * 2, 0.0f);
    output.renderOfflineBlock(pcmBuffer.data(), frames);

    // Verify PCM buffer contains correct audio from pipeline
    for (size_t i = 0; i < frames; ++i) {
        QCOMPARE(pcmBuffer[i * 2], 0.4f);
        QCOMPARE(pcmBuffer[i * 2 + 1], -0.4f);
    }

    output.stop();
    output.shutdown();
}

void TestPipeWireOutput::testDiagnosticsAndXrunTracking()
{
    // Verify Section 17.4 durability monitoring targets:
    // Tracking frame counts, sample rates, buffer sizes, and zero XRUNs in steady state
    PipeWireAudioOutput output;
    output.initialize(96000, 512);
    output.setOfflineSimulationMode(true);
    output.start();

    output.setAudioCallback([](float* outPcm, size_t nFrames) {
        std::fill_n(outPcm, nFrames * 2, 0.1f);
    });

    std::vector<float> buf(512 * 2, 0.0f);
    // Render 100 blocks
    for (int i = 0; i < 100; ++i) {
        output.renderOfflineBlock(buf.data(), 512);
    }

    auto diag = output.diagnostics();
    QCOMPARE(diag.sampleRate, 96000u);
    QCOMPARE(diag.bufferFrames, 512u);
    QCOMPARE(diag.totalFramesProcessed, 512u * 100);
    QCOMPARE(diag.xrunCount, 0ull); // 0 XRUNs in normal processing

    output.stop();
    output.shutdown();
}

void TestPipeWireOutput::cleanupTestCase()
{
    qDebug() << "TestPipeWireOutput completed successfully.";
}

QTEST_MAIN(TestPipeWireOutput)
#include "test_pipewire_output.moc"

