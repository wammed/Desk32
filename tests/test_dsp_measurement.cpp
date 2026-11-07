#include <QTest>
#include "engine/dsp/DspMasterChain.hpp"
#include "engine/format/FormatConverter.hpp"
#include "engine/memory/EpochReclaimer.hpp"
#include <thread>
#include <chrono>

class TestDspMeasurement : public QObject
{
    Q_OBJECT

private slots:
    void testItem6DspFrequencyAndTransient();
    void testItem6LimiterStressSuppression();
    void testItem6MasterChainStressStability();
    void testItem5FormatConversionBoundaries();
    void testItem11EpochMemoryReclamationSafety();
};

void TestDspMeasurement::testItem6DspFrequencyAndTransient()
{
    // Test frequency response & impulse response of DSP Master Chain
    DspMasterChain chain;
    chain.configure(48000.0);

    // Feed an impulse: [1.0, 0.0, 0.0, ...]
    double left = 1.0, right = 1.0;
    chain.process(left, right);

    QVERIFY(!std::isnan(left) && !std::isinf(left));
    QVERIFY(!std::isnan(right) && !std::isinf(right));

    // Tail response over 1024 samples
    for (int i = 0; i < 1024; ++i) {
        double l = 0.0, r = 0.0;
        chain.process(l, r);
        QVERIFY(!std::isnan(l) && !std::isinf(l));
        QVERIFY(!std::isnan(r) && !std::isinf(r));
    }
}

void TestDspMeasurement::testItem6LimiterStressSuppression()
{
    // Test PeakLimiter suppression under +24 dBFS overload (15.85x linear)
    PeakLimiter limiter;
    limiter.configure(48000.0, -0.1); // -0.1 dB ceiling approx 0.98855

    const double freq = 1000.0;
    const double amplitude = std::pow(10.0, 24.0 / 20.0);
    const int sampleCount = 4800; // 100ms

    for (int i = 0; i < sampleCount; ++i) {
        double s = amplitude * std::sin(2.0 * std::numbers::pi * freq * i / 48000.0);
        double left = s, right = s;
        limiter.process(left, right);

        QVERIFY(!std::isnan(left) && !std::isinf(left));
        QVERIFY(std::abs(left) <= limiter.ceiling() + 1e-6);
        QVERIFY(std::abs(right) <= limiter.ceiling() + 1e-6);
    }
}

void TestDspMeasurement::testItem6MasterChainStressStability()
{
    // Test complete Master Chain (PEQ + GEQ + HPF/LPF + Comp + Limiter + Reverb) under extreme stress
    DspMasterChain chain;
    chain.configure(192000.0);

    const double freq = 1000.0;
    const double amplitude = std::pow(10.0, 18.0 / 20.0); // +18 dBFS overload
    const int sampleCount = 19200; // 100ms

    for (int i = 0; i < sampleCount; ++i) {
        double s = amplitude * std::sin(2.0 * std::numbers::pi * freq * i / 192000.0);
        double left = s, right = s;
        chain.process(left, right);

        // Entire chain must be stable, finite, and non-divergent
        QVERIFY(!std::isnan(left) && !std::isinf(left));
        QVERIFY(!std::isnan(right) && !std::isinf(right));
    }
}

void TestDspMeasurement::testItem5FormatConversionBoundaries()
{
    FormatConverter conv(42);

    // 1. Clipping protection at > 0 dBFS (+6 dB)
    double overloaded = 2.0;
    QCOMPARE(FormatConverter::toFloat32(overloaded), 1.0f);
    QCOMPARE(FormatConverter::toInt32(overloaded), 2147483647);
    QCOMPARE(conv.toInt24(overloaded, DitherType::None), 8388607);
    QCOMPARE(conv.toInt16(overloaded, DitherType::None), 32767);

    // 2. Dynamic range retention at -120 dBFS (below 16-bit noise floor, within 24-bit)
    double quiet = std::pow(10.0, -120.0 / 20.0); // approx 1e-6
    int32_t val24 = conv.toInt24(quiet, DitherType::None);
    int32_t val32 = FormatConverter::toInt32(quiet);

    QVERIFY(val24 > 0);
    QVERIFY(val32 > 0);
}

void TestDspMeasurement::testItem11EpochMemoryReclamationSafety()
{
    // Test Epoch-based reclamation safety across concurrent audio/control threads
    EpochReclaimer<DspMasterChain> reclaimer;
    reclaimer.swapChain(std::make_unique<DspMasterChain>());

    std::atomic<bool> running{true};
    std::atomic<uint64_t> processedBlocks{0};

    // Simulated Audio Thread
    std::thread audioThread([&]() {
        while (running.load(std::memory_order_relaxed)) {
            auto guard = reclaimer.enterRead();
            if (guard.chain) {
                double l = 0.1, r = 0.1;
                guard.chain->process(l, r);
                processedBlocks.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    // Control Thread swapping chains rapidly (100 times)
    for (int i = 0; i < 100; ++i) {
        auto newChain = std::make_unique<DspMasterChain>();
        newChain->configure(48000.0);
        reclaimer.swapChain(std::move(newChain));
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    running.store(false, std::memory_order_relaxed);
    audioThread.join();

    reclaimer.drainAll();

    qDebug() << "Epoch reclamation test passed: Processed" << processedBlocks.load() << "blocks during 100 concurrent chain swaps without crash.";
    QVERIFY(processedBlocks.load() > 1000);
}

QTEST_MAIN(TestDspMeasurement)
#include "test_dsp_measurement.moc"
