#include <QTest>
#include <QObject>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include "core/log/LogService.hpp"
#include <thread>
#include <vector>

using namespace desk32::core::log;

class TestLogService : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void testFourteenFieldsAndParentChildLinkage();
    void testD1RetentionAndPurge();
    void testD3TimezoneUtcAndOffset();
    void testAsynchronousWorkerThreadBatch();
    void testRealtimeQueueOverflowSafety();
    void testStructuredJsonlExport();
    void testQueryFilterAndSearch();
    void cleanupTestCase();

private:
    QTemporaryDir m_tempDir;
};

void TestLogService::initTestCase()
{
    qDebug() << "Initializing TestLogService (Phase 1.5 Log Tab Infrastructure)...";
    QVERIFY(m_tempDir.isValid());
}

void TestLogService::testFourteenFieldsAndParentChildLinkage()
{
    QString dbPath = m_tempDir.path() + "/test_14fields.db";
    LogRepository repo("Test14FieldsConn");
    QVERIFY(repo.open(dbPath));

    // 1. Create a parent operation (e.g. Album Export)
    std::string albumOpId = "op-album-render-1001";
    LogEvent parentEv = LogEvent::create(
        LogLevel::Info,
        "Render",
        "Album render session started",
        "{\"album\":\"Hi-Fi Master 2026\",\"track_count\":2}",
        "AlbumRender",
        "Folder",
        "/music/albums/master2026",
        0,
        15.2,
        albumOpId,
        "" // No parent (root)
    );
    QVERIFY(repo.insertEvent(parentEv));

    // 2. Create child operations linked via parent_operation_id
    LogEvent child1 = LogEvent::create(
        LogLevel::Success,
        "Render",
        "Track 1 rendered successfully",
        "{\"format\":\"FLAC\",\"rate\":96000,\"bits\":24}",
        "RenderTrack",
        "File",
        "/music/albums/master2026/01_track.flac",
        0,
        1240.5,
        "op-track-render-01",
        albumOpId // Linked to parent!
    );
    QVERIFY(repo.insertEvent(child1));

    LogEvent child2 = LogEvent::create(
        LogLevel::Error,
        "Render",
        "Track 2 render failed due to disk space",
        "{\"error_details\":\"ENOSPC\"}",
        "RenderTrack",
        "File",
        "/music/albums/master2026/02_track.flac",
        28, // POSIX ENOSPC
        342.1,
        "op-track-render-02",
        albumOpId // Linked to parent!
    );
    QVERIFY(repo.insertEvent(child2));

    QCOMPARE(repo.countEvents(), 3);

    // 3. Query all related events using parent operation ID
    auto related = repo.queryRelatedEvents(albumOpId);
    QCOMPARE(related.size(), 3u);

    // Verify parent event (index 0)
    QCOMPARE(related[0].operationId, albumOpId);
    QVERIFY(related[0].parentOperationId.empty());
    QCOMPARE(related[0].category, std::string("Render"));
    QCOMPARE(related[0].operation, std::string("AlbumRender"));
    QCOMPARE(related[0].status, std::string("Info"));
    QCOMPARE(related[0].targetType, std::string("Folder"));
    QCOMPARE(related[0].target, std::string("/music/albums/master2026"));
    QCOMPARE(related[0].errorCode, 0);
    QCOMPARE(related[0].durationMs, 15.2);

    // Verify child 1 (index 1)
    QCOMPARE(related[1].operationId, std::string("op-track-render-01"));
    QCOMPARE(related[1].parentOperationId, albumOpId);
    QCOMPARE(related[1].status, std::string("Success"));
    QCOMPARE(related[1].durationMs, 1240.5);

    // Verify child 2 (index 2)
    QCOMPARE(related[2].operationId, std::string("op-track-render-02"));
    QCOMPARE(related[2].parentOperationId, albumOpId);
    QCOMPARE(related[2].status, std::string("Failed"));
    QCOMPARE(related[2].errorCode, 28);
    QCOMPARE(related[2].durationMs, 342.1);

    repo.close();
}

void TestLogService::testD1RetentionAndPurge()
{
    QString dbPath = m_tempDir.path() + "/test_retention.db";
    LogRepository repo("TestRetentionConn");
    QVERIFY(repo.open(dbPath));

    // 1. Insert 6 events
    for (int i = 0; i < 6; ++i) {
        LogEvent ev = LogEvent::create(LogLevel::Info, "Deck", "Event " + std::to_string(i));
        QVERIFY(repo.insertEvent(ev));
    }
    QCOMPARE(repo.countEvents(), 6);

    // 2. Count-based purge (limit to max 4 events)
    int countPurged = repo.purgeOldEvents(180, 4);
    QCOMPARE(countPurged, 2);
    QCOMPARE(repo.countEvents(), 4);

    // 3. Date-based purge (older than 180 days)
    LogEvent ancientEv = LogEvent::create(LogLevel::Info, "System", "Ancient event from year 2020");
    ancientEv.timestampUtc = "2020-01-01T12:00:00.000Z";
    QVERIFY(repo.insertEvent(ancientEv));
    QCOMPARE(repo.countEvents(), 5);

    int datePurged = repo.purgeOldEvents(180, 100);
    QCOMPARE(datePurged, 1);
    QCOMPARE(repo.countEvents(), 4);

    repo.close();
}

void TestLogService::testD3TimezoneUtcAndOffset()
{
    LogEvent ev = LogEvent::create(LogLevel::Info, "AudioEngine", "Stream initialised");

    // UTC timestamp must match ISO 8601 UTC regex: YYYY-MM-DDTHH:MM:SS.sssZ
    QRegularExpression utcRegex(R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$)");
    QVERIFY(utcRegex.match(QString::fromStdString(ev.timestampUtc)).hasMatch());

    // Local offset must match: [+-]HH:MM
    QRegularExpression offRegex(R"(^[+-]\d{2}:\d{2}$)");
    QVERIFY(offRegex.match(QString::fromStdString(ev.localUtcOffset)).hasMatch());
}

void TestLogService::testAsynchronousWorkerThreadBatch()
{
    QString dbPath = m_tempDir.path() + "/test_async_service.db";
    LogService service;
    QVERIFY(service.start(dbPath));

    const int threadCount = 4;
    const int logsPerThread = 25;
    std::vector<std::thread> threads;

    // Concurrent multi-producer logging
    for (int t = 0; t < threadCount; ++t) {
        threads.emplace_back([&service, t, logsPerThread]() {
            for (int i = 0; i < logsPerThread; ++i) {
                LogEvent ev = LogEvent::create(
                    LogLevel::Info,
                    "ThreadWorker",
                    "Message from thread " + std::to_string(t) + " idx " + std::to_string(i),
                    "{\"worker_id\":" + std::to_string(t) + "}",
                    "AsyncOp",
                    "Thread",
                    "T" + std::to_string(t)
                );
                service.postLog(ev);
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    // Flush to disk
    service.flush();

    // Verify that all 100 logs were persisted into SQLite WAL
    QCOMPARE(service.countEvents(), 100);

    service.stop();
}

void TestLogService::testRealtimeQueueOverflowSafety()
{
    BoundedLogQueue<LogEvent, 16> smallQueue;

    // Fill queue to capacity (16 items)
    for (int i = 0; i < 16; ++i) {
        LogEvent ev = LogEvent::create(LogLevel::Info, "AudioRT", "Buffer frame rendered");
        bool ok = smallQueue.try_push(ev);
        QVERIFY(ok);
    }

    QCOMPARE(smallQueue.size(), 16u);
    QCOMPARE(smallQueue.overflowCount(), 0u);

    // Next realtime push MUST FAIL non-blockingly and record overflow
    LogEvent overflowEv = LogEvent::create(LogLevel::Warning, "AudioRT", "XRUN detected");
    bool okAfterFull = smallQueue.try_push(overflowEv);
    QVERIFY(!okAfterFull); // RT audio thread was not blocked!
    QCOMPARE(smallQueue.overflowCount(), 1u);

    // Drain queue
    auto drained = smallQueue.drain();
    QCOMPARE(drained.size(), 16u);
    QCOMPARE(smallQueue.size(), 0u);

    // After draining, pushing resumes normally
    QVERIFY(smallQueue.try_push(overflowEv));
}

void TestLogService::testStructuredJsonlExport()
{
    QString dbPath = m_tempDir.path() + "/test_jsonl_export.db";
    QString jsonlPath = m_tempDir.path() + "/export_output.jsonl";

    LogRepository repo("TestJsonlExportConn");
    QVERIFY(repo.open(dbPath));

    repo.insertEvent(LogEvent::create(
        LogLevel::Success,
        "DSP",
        "GEQ 32-band initialized with flat response",
        "{\"bands\":32,\"gain\":0.0}",
        "InitGeq",
        "DSP",
        "GEQ-32",
        0,
        1.25
    ));

    repo.insertEvent(LogEvent::create(
        LogLevel::Error,
        "AudioBackend",
        "Device buffer underrun occurred",
        "{\"xrun_count\":1,\"hw_buffer_frames\":256}",
        "XrunEvent",
        "Device",
        "hw:UR22",
        -32,
        0.05
    ));

    QVERIFY(repo.exportToJsonl(jsonlPath));

    // Verify JSONL structure
    QFile file(jsonlPath);
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    QByteArray l1 = file.readLine();
    QByteArray l2 = file.readLine();
    file.close();

    QVERIFY(!l1.isEmpty());
    QVERIFY(!l2.isEmpty());

    // Line 1 verification
    QJsonDocument doc1 = QJsonDocument::fromJson(l1);
    QVERIFY(!doc1.isNull());
    QJsonObject obj1 = doc1.object();
    QCOMPARE(obj1["category"].toString(), QString("DSP"));
    QCOMPARE(obj1["status"].toString(), QString("Success"));
    QCOMPARE(obj1["level"].toString(), QString("Success")); // Backward compat check
    QCOMPARE(obj1["operation"].toString(), QString("InitGeq"));
    QCOMPARE(obj1["target_type"].toString(), QString("DSP"));
    QCOMPARE(obj1["target"].toString(), QString("GEQ-32"));
    QCOMPARE(obj1["duration_ms"].toDouble(), 1.25);

    // Line 2 verification
    QJsonDocument doc2 = QJsonDocument::fromJson(l2);
    QVERIFY(!doc2.isNull());
    QJsonObject obj2 = doc2.object();
    QCOMPARE(obj2["category"].toString(), QString("AudioBackend"));
    QCOMPARE(obj2["status"].toString(), QString("Failed"));
    QCOMPARE(obj2["level"].toString(), QString("Error")); // Backward compat check
    QCOMPARE(obj2["error_code"].toInt(), -32);

    repo.close();
}

void TestLogService::testQueryFilterAndSearch()
{
    QString dbPath = m_tempDir.path() + "/test_query.db";
    LogRepository repo("TestQueryConn");
    QVERIFY(repo.open(dbPath));

    repo.insertEvent(LogEvent::create(LogLevel::Success, "Deck", "Deck A track loaded", "{}", "LoadTrack", "Deck", "Deck A"));
    repo.insertEvent(LogEvent::create(LogLevel::Success, "Deck", "Deck B track loaded", "{}", "LoadTrack", "Deck", "Deck B"));
    repo.insertEvent(LogEvent::create(LogLevel::Error, "Decoder", "Failed to parse FLAC header", "{}", "OpenDecoder", "File", "/bad/file.flac"));
    repo.insertEvent(LogEvent::create(LogLevel::Warning, "ExternalInsert", "Buffer mismatch detected", "{}", "Connect", "Plugin", "LSP"));

    // 1. Filter by category
    LogFilter fCat;
    fCat.category = "Deck";
    auto deckLogs = repo.queryEvents(fCat);
    QCOMPARE(deckLogs.size(), 2u);

    // 2. Filter by status
    LogFilter fStatus;
    fStatus.status = "Failed";
    auto failLogs = repo.queryEvents(fStatus);
    QCOMPARE(failLogs.size(), 1u);
    QCOMPARE(failLogs[0].category, std::string("Decoder"));

    // 3. Search text
    LogFilter fSearch;
    fSearch.searchText = "mismatch";
    auto searchLogs = repo.queryEvents(fSearch);
    QCOMPARE(searchLogs.size(), 1u);
    QCOMPARE(searchLogs[0].category, std::string("ExternalInsert"));

    repo.close();
}

void TestLogService::cleanupTestCase()
{
    qDebug() << "TestLogService cleanup completed successfully.";
}

QTEST_MAIN(TestLogService)
#include "test_log_service.moc"

