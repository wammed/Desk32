#include <QTest>
#include "engine/dsp/Resampler.hpp"
#include "engine/deck/DeckManager.hpp"
#include <vector>
#include <numeric>
#include <algorithm>

using namespace desk32::engine::dsp;
using namespace desk32::engine::deck;

class TestSampleRateSwitching : public QObject
{
    Q_OBJECT

private slots:
    void testResamplerQualityAndBypass();
    void testItem15AndK2DeckSwitchingMuteTime();
    void testItem15DirectModeRenegotiation();
};

void TestSampleRateSwitching::testResamplerQualityAndBypass()
{
    // 1. Bypass mode: 48 kHz to 48 kHz
    Resampler bypassResampler;
    QVERIFY(bypassResampler.configure(48000.0, 48000.0, 2));
    QVERIFY(bypassResampler.isBypass());
    QCOMPARE(bypassResampler.latencyInInputSamples(), 0.0);

    std::vector<double> inBuf(512, 0.707);
    std::vector<double> outBuf(512, 0.0);
    size_t inDone = 0, outDone = 0;
    QVERIFY(bypassResampler.process(inBuf.data(), 256, inDone, outBuf.data(), 256, outDone));
    QCOMPARE(inDone, 256);
    QCOMPARE(outDone, 256);
    QCOMPARE(outBuf[0], 0.707);

    // 2. High-grade Resampling: 44.1 kHz to 192 kHz
    Resampler upResampler;
    QVERIFY(upResampler.configure(44100.0, 192000.0, 2, ResamplerQuality::VeryHigh));
    QVERIFY(!upResampler.isBypass());

    // Feed 2000 input frames
    std::vector<double> in44k(4000, 0.5);
    std::vector<double> out192k(16000, 0.0);
    size_t upInDone = 0, upOutDone = 0;
    QVERIFY(upResampler.process(in44k.data(), 2000, upInDone, out192k.data(), 8000, upOutDone));
    QVERIFY(upInDone > 0);
    QVERIFY(upOutDone > 0);
    QVERIFY(upResampler.latencyInInputSamples() > 0.0);
}

void TestSampleRateSwitching::testItem15AndK2DeckSwitchingMuteTime()
{
    // Setup 4 Decks with Fixed Graph Rate 48.0 kHz
    DeckManager manager(48000.0);

    // Deck A: 44.1 kHz track (10 seconds)
    TrackInfo trackA{"/path/to/track_44k.flac", 44100.0, 2, 441000};
    QVERIFY(manager.deck('A')->loadTrack(trackA, manager.graphRate()));

    // Deck B: 192.0 kHz track (10 seconds)
    TrackInfo trackB{"/path/to/track_192k.flac", 192000.0, 2, 1920000};
    QVERIFY(manager.deck('B')->loadTrack(trackB, manager.graphRate()));

    // Start playing Deck A
    manager.deck('A')->play();

    // Perform repeated switching between Deck A (44.1k) and Deck B (192k) 100 times
    const int iterations = 100;
    std::vector<double> muteTimes;
    muteTimes.reserve(iterations);

    for (int i = 0; i < iterations; ++i) {
        char target = (i % 2 == 0) ? 'B' : 'A';
        SwitchResult res = manager.switchDeck(target, SwitchingMode::Comparative);

        QVERIFY(res.success);
        QCOMPARE(res.targetDeck, target);
        muteTimes.push_back(res.muteTimeMs);

        // Verify active deck produces audio
        std::vector<double> audioBlock(512);
        size_t rendered = manager.renderBlock(audioBlock.data(), 256);
        QVERIFY(rendered > 0);
    }

    // Sort to calculate statistics
    std::sort(muteTimes.begin(), muteTimes.end());
    double p50 = muteTimes[muteTimes.size() / 2];
    double p90 = muteTimes[static_cast<size_t>(static_cast<double>(muteTimes.size()) * 0.90)];
    double p99 = muteTimes[static_cast<size_t>(static_cast<double>(muteTimes.size()) * 0.99)];
    double maxTime = muteTimes.back();

    qDebug() << "Kill Gate K2 Deck Switching Mute Times (100 iterations between 44.1k & 192k):"
             << "p50 =" << p50 << "ms,"
             << "p90 =" << p90 << "ms,"
             << "p99 =" << p99 << "ms,"
             << "max =" << maxTime << "ms";

    // Specification 17.4 Pass Criterion for K2:
    // "通常再生・比較切り替え経路（グラフレート固定＋リサンプル、2.6）では、切り替え時の無音時間のp99が500ms以下であること"
    QVERIFY2(p99 <= 500.0, "K2 Pass criterion failed: p99 mute time exceeded 500 ms!");
}

void TestSampleRateSwitching::testItem15DirectModeRenegotiation()
{
    DeckManager manager(44100.0);

    TrackInfo trackA{"/path/to/track_44k.flac", 44100.0, 2, 441000};
    manager.deck('A')->loadTrack(trackA, 44100.0);
    manager.deck('A')->play();

    TrackInfo trackB{"/path/to/track_192k.flac", 192000.0, 2, 1920000};
    manager.deck('B')->loadTrack(trackB, 44100.0);

    // Direct mode switch to 192 kHz track
    SwitchResult res = manager.directModeSwitch('B');
    QVERIFY(res.success);
    QCOMPARE(res.targetDeck, 'B');
    QCOMPARE(manager.graphRate(), 192000.0);

    qDebug() << "Direct Mode renegotiation mute time (44.1k -> 192k):" << res.muteTimeMs << "ms";
    QVERIFY(res.muteTimeMs > 0.0);
}

QTEST_MAIN(TestSampleRateSwitching)
#include "test_sample_rate_switching.moc"
