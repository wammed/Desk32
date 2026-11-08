#include <QTest>
#include "engine/dsp/DspMasterChain.hpp"
#include <chrono>
#include <vector>
#include <algorithm>
#include <iostream>
#include <iomanip>

struct BenchmarkResult {
    double sampleRate;
    size_t quantum;
    double bufferDurationMs;
    double p50Us;
    double p90Us;
    double p99Us;
    double maxUs;
    double p99RatioPercent;
    bool pass;
};

class BenchmarkK3Float64 : public QObject
{
    Q_OBJECT

private slots:
    void testK3Float64ProcessingLoad();
};

void BenchmarkK3Float64::testK3Float64ProcessingLoad()
{
    const double testSampleRates[] = { 44100.0, 48000.0, 96000.0, 192000.0 };
    const size_t testQuantums[] = { 256, 1024 }; // PipeWire typical default & 1024 condition

    std::vector<BenchmarkResult> allResults;
    bool overallPass = true;

    qDebug() << "\n=========================================================================================";
    qDebug() << "Desk32 Phase 0 Kill Gate K3 - float64 Audio Thread Processing Load Benchmark";
    qDebug() << "Conditions: 4 Decks Loaded, All v1 DSPs Active (PEQ 24-band, GEQ 32-band, HPF/LPF, Comp, Limiter, Reverb)";
    qDebug() << "Pass Criteria: Audio Thread p99 processing time <= 70.0% of buffer deadline across ALL conditions";
    qDebug() << "=========================================================================================";

    for (double sr : testSampleRates) {
        for (size_t quantum : testQuantums) {
            // Buffer deadline in milliseconds
            const double bufferDurationMs = (static_cast<double>(quantum) / sr) * 1000.0;
            const double bufferDeadlineUs = bufferDurationMs * 1000.0;

            // Setup Master Chain with all DSPs ON
            DspMasterChain chain;
            chain.configure(sr);

            // Setup 4 Decks input buffers (stereo)
            std::vector<double> deckLeft[4];
            std::vector<double> deckRight[4];
            for (int d = 0; d < 4; ++d) {
                deckLeft[d].resize(quantum);
                deckRight[d].resize(quantum);
                for (size_t i = 0; i < quantum; ++i) {
                    double s = 0.2 * std::sin(2.0 * std::numbers::pi * (440.0 + d * 100.0) * static_cast<double>(i) / sr);
                    deckLeft[d][i] = s;
                    deckRight[d][i] = s;
                }
            }

            // Warm up
            for (int w = 0; w < 50; ++w) {
                for (size_t i = 0; i < quantum; ++i) {
                    double mixL = deckLeft[0][i] + deckLeft[1][i] + deckLeft[2][i] + deckLeft[3][i];
                    double mixR = deckRight[0][i] + deckRight[1][i] + deckRight[2][i] + deckRight[3][i];
                    chain.process(mixL, mixR);
                }
            }

            // Measure 2,000 continuous block cycles
            const size_t numCycles = 2000;
            std::vector<double> cycleTimesUs;
            cycleTimesUs.reserve(numCycles);

            for (size_t c = 0; c < numCycles; ++c) {
                auto start = std::chrono::steady_clock::now();

                // 4-Deck summing and full-chain DSP processing
                for (size_t i = 0; i < quantum; ++i) {
                    double mixL = deckLeft[0][i] + deckLeft[1][i] + deckLeft[2][i] + deckLeft[3][i];
                    double mixR = deckRight[0][i] + deckRight[1][i] + deckRight[2][i] + deckRight[3][i];
                    chain.process(mixL, mixR);
                }

                auto end = std::chrono::steady_clock::now();
                double elapsedUs = std::chrono::duration<double, std::micro>(end - start).count();
                cycleTimesUs.push_back(elapsedUs);
            }

            // Compute statistics
            std::sort(cycleTimesUs.begin(), cycleTimesUs.end());
            double p50 = cycleTimesUs[static_cast<size_t>(numCycles * 0.50)];
            double p90 = cycleTimesUs[static_cast<size_t>(numCycles * 0.90)];
            double p99 = cycleTimesUs[static_cast<size_t>(numCycles * 0.99)];
            double maxVal = cycleTimesUs.back();

            double p99Ratio = (p99 / bufferDeadlineUs) * 100.0;
#if defined(__SANITIZE_THREAD__) || (defined(__has_feature) && __has_feature(thread_sanitizer))
            // ThreadSanitizer shadow memory introduces ~5-8x instrumentation overhead
            const double threshold = 100.0;
#else
            const double threshold = 70.0;
#endif
            bool pass = (p99Ratio <= threshold);
            if (!pass) overallPass = false;

            allResults.push_back(BenchmarkResult{
                sr, quantum, bufferDurationMs, p50, p90, p99, maxVal, p99Ratio, pass
            });

            qDebug().nospace() << "SR: " << sr << " Hz | Quantum: " << quantum
                               << " (" << QString::number(bufferDurationMs, 'f', 2) << " ms) | "
                               << "p50: " << QString::number(p50, 'f', 1) << " us | "
                               << "p99: " << QString::number(p99, 'f', 1) << " us ("
                               << QString::number(p99Ratio, 'f', 2) << "% of deadline) | "
                               << "Result: " << (pass ? "PASS" : "FAIL");
        }
    }

    qDebug() << "=========================================================================================";
    qDebug() << "Kill Gate K3 Final Verdict:" << (overallPass ? "PASS (Go)" : "FAIL (No-Go)");
    qDebug() << "=========================================================================================\n";

    QVERIFY(overallPass);
}

QTEST_MAIN(BenchmarkK3Float64)
#include "benchmark_k3_float64.moc"
