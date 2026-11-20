#include <QTest>
#include <QObject>
#include "engine/backend/AudioControlManager.hpp"
#include "engine/backend/PipeWireAudioOutput.hpp"
#include "engine/backend/PipeWireControlAdapter.hpp"
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
    void testAudioControlManagerCapabilitiesAndPcmFormats();
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

void TestPipeWireOutput::testAudioControlManagerCapabilitiesAndPcmFormats()
{
    // Verify §8.1 AudioControlManager / PipeWireControlAdapter separation,
    // §8.2 4-state capabilities, sink selection, and §3.4 S16LE/S24_32LE/F32LE formats.
    PipeWireAudioOutput output;
    AudioControlManager mgr(std::make_unique<PipeWireControlAdapter>(&output));

    const auto preInitCaps = mgr.queryCapabilities(false, false);
    QCOMPARE(preInitCaps.directMode, CapabilityAvailability::Unavailable);
    QCOMPARE(preInitCaps.exclusiveAccess, CapabilityAvailability::Unsupported);
    QCOMPARE(preInitCaps.hardwareBitPerfect, CapabilityAvailability::Unknown);

    output.setOfflineSimulationMode(true);
    AudioOutputDeviceInfo dac1;
    dac1.nodeId = 101;
    dac1.name = "alsa_output.usb-dac";
    dac1.description = "Reference USB DAC";
    dac1.isDefault = true;
    dac1.sampleRate = 96000;
    dac1.nativeFormat = OutputPcmFormat::S24_32LE;
    AudioOutputDeviceInfo dac2;
    dac2.nodeId = 102;
    dac2.name = "alsa_output.builtin";
    dac2.description = "Onboard Stereo";
    dac2.sampleRate = 48000;
    output.setSimulatedOutputDevices({dac1, dac2});

    QVERIFY(output.initialize(48000, 256));
    QVERIFY(output.start());

    const auto devices = mgr.enumerateOutputDevices();
    QCOMPARE(devices.size(), size_t(2));
    QVERIFY(mgr.selectOutputDevice("alsa_output.usb-dac"));
    QCOMPARE(mgr.currentTargetDevice(), std::string("alsa_output.usb-dac"));
    std::string errMsg;
    QVERIFY(!mgr.selectOutputDevice("nonexistent-sink-id", &errMsg));
    QVERIFY(!errMsg.empty());

    QVERIFY(mgr.applyLatencySetting(512, 48000.0));
    QCOMPARE(output.diagnostics().negotiatedQuantum, uint32_t(512));

    // Verify S16LE & S24_32LE raw PCM output negotiation and rendering
    AudioPipeline pipeline(48000.0);
    std::vector<double> inL(128, 0.5), inR(128, -0.5);
    output.setRawPcmAudioCallback([&](void* outBuf, size_t nFrames, OutputPcmFormat fmt) {
        std::vector<double> l = inL;
        std::vector<double> r = inR;
        auto sf = (fmt == OutputPcmFormat::S16LE) ? SampleFormat::Int16
                  : (fmt == OutputPcmFormat::S24_32LE) ? SampleFormat::Int24_32
                                                       : SampleFormat::Float32;
        pipeline.process(l.data(), r.data(), nFrames, outBuf, sf, false);
    });

    QVERIFY(mgr.setPreferredPcmFormat(OutputPcmFormat::S16LE));
    std::vector<int16_t> s16Buf(128 * 2, 0);
    OutputPcmFormat usedFmt = OutputPcmFormat::F32LE;
    output.renderOfflineRawBlock(s16Buf.data(), 128, &usedFmt);
    QCOMPARE(usedFmt, OutputPcmFormat::S16LE);
    QCOMPARE(s16Buf[0], int16_t(16384));
    QCOMPARE(s16Buf[1], int16_t(-16384));

    QVERIFY(mgr.setPreferredPcmFormat(OutputPcmFormat::S24_32LE));
    std::vector<int32_t> s24Buf(128 * 2, 0);
    output.renderOfflineRawBlock(s24Buf.data(), 128, &usedFmt);
    QCOMPARE(usedFmt, OutputPcmFormat::S24_32LE);
    QVERIFY(std::abs(s24Buf[0] - 4194304) <= 1);
    QCOMPARE(output.diagnostics().formatName, std::string("S24_32LE"));

    output.stop();
    output.shutdown();
}

void TestPipeWireOutput::cleanupTestCase()
{
    qDebug() << "TestPipeWireOutput completed successfully.";
}

QTEST_MAIN(TestPipeWireOutput)
#include "test_pipewire_output.moc"

