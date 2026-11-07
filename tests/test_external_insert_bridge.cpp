#include <QTest>
#include "engine/insert/ExternalInsertBridge.hpp"
#include <vector>

using namespace desk32::engine::insert;

class TestExternalInsertBridge : public QObject
{
    Q_OBJECT

private slots:
    void testItem7FeedbackLoopDetection();
    void testItem7StateTransitionsAndDisconnectRecovery();
    void testItem10ReportedVsMeasuredLatency();
    void testItem18KillGateK1FormalEvaluation();
};

void TestExternalInsertBridge::testItem7FeedbackLoopDetection()
{
    ExternalInsertBridge bridge;

    // Normal configuration
    ExternalPortInfo normalConfig{
        "Carla",
        "carla:audio_in_1", "carla:audio_in_2",
        "carla:audio_out_1", "carla:audio_out_2",
        48000.0, 256, 128
    };
    QVERIFY(bridge.configure(normalConfig, 48000.0, 256));
    QCOMPARE(bridge.state(), InsertState::Connected);
    QVERIFY(!bridge.isBypassed());

    // Loopback configuration where send port equals return port
    ExternalPortInfo loopbackConfig{
        "Carla",
        "desk32:insert_send_1", "desk32:insert_send_2",
        "desk32:insert_send_1", "desk32:insert_send_2", // Direct feedback!
        48000.0, 256, 0
    };
    ExternalInsertBridge loopBridge;
    bool ok = loopBridge.configure(loopbackConfig, 48000.0, 256);

    // Specification 6.4 & 17.4 Pass Criterion: Feedback loop must be detected & prevented
    QVERIFY(!ok);
    QCOMPARE(loopBridge.state(), InsertState::Error);
    QVERIFY(loopBridge.isBypassed());
    QVERIFY(loopBridge.lastError().find("Feedback loop detected") != std::string::npos);
}

void TestExternalInsertBridge::testItem7StateTransitionsAndDisconnectRecovery()
{
    ExternalInsertBridge bridge;
    ExternalPortInfo config{
        "Carla",
        "carla:audio_in_1", "carla:audio_in_2",
        "carla:audio_out_1", "carla:audio_out_2",
        48000.0, 256, 64
    };
    QVERIFY(bridge.configure(config, 48000.0, 256));
    QCOMPARE(bridge.state(), InsertState::Connected);

    // Process audio -> state transitions to Active
    std::vector<double> left(256, 0.5), right(256, 0.5);
    bridge.process(left.data(), right.data(), 256);
    QCOMPARE(bridge.state(), InsertState::Active);

    // External host disconnect event
    bridge.handleDisconnect();
    QCOMPARE(bridge.state(), InsertState::Disconnected);
    QVERIFY(bridge.isBypassed());

    // While disconnected, audio must pass through safely without crash
    bridge.process(left.data(), right.data(), 256);
    QCOMPARE(left[0], 0.5);

    // External host recovery event
    bridge.handleRecovery();
    QCOMPARE(bridge.state(), InsertState::Connected);
    QVERIFY(!bridge.isBypassed());
}

void TestExternalInsertBridge::testItem10ReportedVsMeasuredLatency()
{
    ExternalInsertBridge bridge;
    uint32_t reportedLatency = 128; // External host reports 128 frames delay
    ExternalPortInfo config{
        "Carla",
        "carla:audio_in_1", "carla:audio_in_2",
        "carla:audio_out_1", "carla:audio_out_2",
        48000.0, 256, reportedLatency
    };
    QVERIFY(bridge.configure(config, 48000.0, 256));
    QCOMPARE(bridge.reportedLatencyFrames(), reportedLatency);

    // Trigger impulse latency measurement
    bridge.triggerLatencyMeasurement();

    // Process blocks
    std::vector<double> left(256, 0.0), right(256, 0.0);
    bridge.process(left.data(), right.data(), 256);

    // Simulate loopback arrival after 128 frames (matching reported latency)
    uint64_t returnFrame = 64 + 128;
    bridge.recordImpulseReturn(returnFrame);

    QCOMPARE(bridge.measuredLatencyFrames(), 128);
    // 128 frames at 48000 Hz = 2.6666... ms
    QVERIFY(std::abs(bridge.measuredLatencyMs() - (128.0 / 48.0)) < 0.01);

    qDebug() << "Latency Comparison (Item 10):"
             << "Reported Latency =" << bridge.reportedLatencyFrames() << "frames ("
             << (bridge.reportedLatencyFrames() / 48.0) << "ms),"
             << "Measured Latency =" << bridge.measuredLatencyFrames() << "frames ("
             << bridge.measuredLatencyMs() << "ms),"
             << "Delta =" << (bridge.measuredLatencyFrames() - static_cast<int64_t>(bridge.reportedLatencyFrames())) << "frames.";
}

void TestExternalInsertBridge::testItem18KillGateK1FormalEvaluation()
{
    // Formal evaluation against Specification 17.4 Kill Gate K1 Pass criteria:
    // 1. Bidirectional send/receive established
    // 2. Feedback loop detection and prevention
    // 3. Disconnect/recovery state transitions per Section 6.6
    // 4. Round-trip latency measured and recorded distinctly from reported latency

    ExternalInsertBridge bridge;
    ExternalPortInfo config{
        "CarlaHost",
        "carla:in_1", "carla:in_2",
        "carla:out_1", "carla:out_2",
        48000.0, 256, 240 // 240 frames reported by LSP limiter
    };

    QVERIFY(bridge.configure(config, 48000.0, 256));
    std::vector<double> left(256, 0.2), right(256, 0.2);
    bridge.process(left.data(), right.data(), 256);
    QCOMPARE(bridge.state(), InsertState::Active);

    // Feedback test
    ExternalPortInfo fbConfig{"CarlaHost", "p1", "p2", "p1", "p2", 48000.0, 256, 0};
    ExternalInsertBridge fbBridge;
    QVERIFY(!fbBridge.configure(fbConfig, 48000.0, 256));
    QCOMPARE(fbBridge.state(), InsertState::Error);

    // Disconnect test
    bridge.handleDisconnect();
    QCOMPARE(bridge.state(), InsertState::Disconnected);
    QVERIFY(bridge.isBypassed());

    // Recovery test
    bridge.handleRecovery();
    QCOMPARE(bridge.state(), InsertState::Connected);
    QVERIFY(!bridge.isBypassed());

    // Measurement test
    bridge.triggerLatencyMeasurement();
    bridge.process(left.data(), right.data(), 256);
    bridge.recordImpulseReturn(256 + 64 + 240);
    QCOMPARE(bridge.measuredLatencyFrames(), 240);
    QVERIFY(bridge.measuredLatencyMs() > 0.0);

    qDebug() << "Kill Gate K1 Formal Evaluation: PASS (All criteria satisfied).";
}

QTEST_MAIN(TestExternalInsertBridge)
#include "test_external_insert_bridge.moc"
