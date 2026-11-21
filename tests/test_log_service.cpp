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
    void testExportLatestEventsLimit();
    void testSys05OperationRetentionAndCascadePurge();
    void testXdgStatePathJsonlFilterParityAndOrphanPurge();
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

void TestLogService::testExportLatestEventsLimit()
{
    QString dbPath = m_tempDir.path() + "/test_export_all_and_filtered.db";
    QString jsonlAllPath = m_tempDir.path() + "/export_all.jsonl";
    QString jsonlFilteredPath = m_tempDir.path() + "/export_filtered.jsonl";
    QString jsonlSessionSearchPath = m_tempDir.path() + "/export_session_search.jsonl";
    QString jsonlLatestLimitPath = m_tempDir.path() + "/export_latest_limit.jsonl";

    LogService service;
    QVERIFY(service.start(dbPath));

    for (int i = 1; i <= 250; ++i) {
        const bool isFailedRender = (i % 50 == 0);
        service.log(
            isFailedRender ? QStringLiteral("Render") : QStringLiteral("Deck"),
            isFailedRender ? QStringLiteral("RenderTrack") : QStringLiteral("LoadTrack"),
            isFailedRender ? QStringLiteral("Failed") : QStringLiteral("Success"),
            QStringLiteral("Event %1").arg(i),
            QStringLiteral("Deck"),
            QStringLiteral("Deck A"),
            QStringLiteral("{\"seq\":%1}").arg(i)
        );
    }
    service.flush();
    QCOMPARE(service.countEvents(), 250);

    // 1. Default exportToJsonl() should export ALL 250 events in chronological order
    QVERIFY(service.exportToJsonl(jsonlAllPath));
    {
        QFile file(jsonlAllPath);
        QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
        QList<QByteArray> lines = file.readAll().split('\n');
        lines.removeAll(QByteArray());
        QCOMPARE(lines.size(), 250);

        const QJsonObject firstObj = QJsonDocument::fromJson(lines.first()).object();
        const QJsonObject lastObj = QJsonDocument::fromJson(lines.last()).object();
        QCOMPARE(firstObj["message"].toString(), QString("Event 1"));
        QCOMPARE(lastObj["message"].toString(), QString("Event 250"));
    }

    // 2. Filtered export with limit=0 (as used by UI Export JSONL) should export ONLY matching events
    QVariantMap filter;
    filter["category"] = QStringLiteral("Render");
    filter["status"] = QStringLiteral("Failed");
    filter["limit"] = 0;
    QVERIFY(service.exportFilteredToJsonl(jsonlFilteredPath, filter));
    {
        QFile file(jsonlFilteredPath);
        QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
        QList<QByteArray> lines = file.readAll().split('\n');
        lines.removeAll(QByteArray());
        QCOMPARE(lines.size(), 5); // 50, 100, 150, 200, 250
        for (const QByteArray& line : lines) {
            const QJsonObject obj = QJsonDocument::fromJson(line).object();
            QCOMPARE(obj["category"].toString(), QString("Render"));
            QCOMPARE(obj["status"].toString(), QString("Failed"));
        }
    }

    // 3. Combined sessionId + searchText + playbackHistoryOnly filtering
    QVariantMap sessionSearchFilter;
    sessionSearchFilter["sessionId"] = service.sessionId();
    sessionSearchFilter["searchText"] = QStringLiteral("Event 20");
    sessionSearchFilter["playbackHistoryOnly"] = true;
    sessionSearchFilter["limit"] = 0;
    QVERIFY(service.exportFilteredToJsonl(jsonlSessionSearchPath, sessionSearchFilter));
    {
        QFile file(jsonlSessionSearchPath);
        QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
        QList<QByteArray> lines = file.readAll().split('\n');
        lines.removeAll(QByteArray());
        // Matches Deck/LoadTrack/Success with "Event 20*" (excludes Event 200 which is Render/Failed)
        // Event 20, Event 201..209 -> 10 events
        QCOMPARE(lines.size(), 10);
        for (const QByteArray& line : lines) {
            const QJsonObject obj = QJsonDocument::fromJson(line).object();
            QCOMPARE(obj["category"].toString(), QString("Deck"));
            QCOMPARE(obj["operation"].toString(), QString("LoadTrack"));
            QVERIFY(obj["message"].toString().startsWith(QStringLiteral("Event 20")));
        }
    }

    // 4. Positive limit exports the latest N events (not oldest) in chronological order
    QVariantMap limitFilter;
    limitFilter["limit"] = 120;
    QVERIFY(service.exportFilteredToJsonl(jsonlLatestLimitPath, limitFilter));
    {
        QFile file(jsonlLatestLimitPath);
        QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
        QList<QByteArray> lines = file.readAll().split('\n');
        lines.removeAll(QByteArray());
        QCOMPARE(lines.size(), 120);
        QCOMPARE(QJsonDocument::fromJson(lines.first()).object()["message"].toString(), QString("Event 131"));
        QCOMPARE(QJsonDocument::fromJson(lines.last()).object()["message"].toString(), QString("Event 250"));
    }

    service.stop();
}

void TestLogService::testSys05OperationRetentionAndCascadePurge()
{
    QString dbPath = m_tempDir.path() + "/test_sys05.db";
    LogRepository repo("TestSys05Conn");
    QVERIFY(repo.open(dbPath));

    // =========================================================================
    // 1. 180-Day Boundary Test
    // =========================================================================
    {
        QDateTime now = QDateTime::currentDateTimeUtc();
        
        // Operation A: 181 days old (Expired)
        std::string opExpired = "op-expired-181d";
        LogEvent evA1 = LogEvent::create(LogLevel::Info, "System", "Expired op start", "{}", "OpStart", "System", "", 0, 0.0, opExpired, "");
        evA1.timestampUtc = now.addDays(-181).toString(Qt::ISODateWithMs).toStdString();
        QVERIFY(repo.insertEvent(evA1));

        LogEvent evA2 = LogEvent::create(LogLevel::Success, "System", "Expired op complete", "{}", "OpComplete", "System", "", 0, 0.0, opExpired, "");
        evA2.timestampUtc = now.addDays(-181).toString(Qt::ISODateWithMs).toStdString();
        QVERIFY(repo.insertEvent(evA2));

        // Operation B: 179 days old (Retained)
        std::string opRetained = "op-retained-179d";
        LogEvent evB1 = LogEvent::create(LogLevel::Info, "System", "Retained op start", "{}", "OpStart", "System", "", 0, 0.0, opRetained, "");
        evB1.timestampUtc = now.addDays(-179).toString(Qt::ISODateWithMs).toStdString();
        QVERIFY(repo.insertEvent(evB1));

        QCOMPARE(repo.countOperations(), 2);
        QCOMPARE(repo.countEvents(), 3);

        // Purge with maxRetentionDays = 180, generous maxOperations
        int purgedRows = repo.purgeOldOperations(180, 1000);
        QCOMPARE(purgedRows, 2); // Both events of opExpired deleted atomically
        QCOMPARE(repo.countOperations(), 1);
        QCOMPARE(repo.countEvents(), 1);

        auto remaining = repo.queryEvents();
        QCOMPARE(remaining.size(), 1u);
        QCOMPARE(remaining[0].operationId, opRetained);
    }

    // =========================================================================
    // 2. 300,000 Operation Boundary Test (Tested with 3 Operation Cap)
    //    Verifies logical operation counting, not individual event counting.
    // =========================================================================
    {
        // Add 4 more root operations, each having 3 events (12 events total)
        for (int op = 1; op <= 4; ++op) {
            std::string opId = "multi-ev-op-" + std::to_string(op);
            for (int ev = 1; ev <= 3; ++ev) {
                LogEvent logEv = LogEvent::create(
                    LogLevel::Info, "Deck", "Event " + std::to_string(ev) + " for " + opId,
                    "{}", "LoadTrack", "Deck", "Deck A", 0, 10.0, opId, ""
                );
                // Stagger timestamps so op 1 is oldest
                logEv.timestampUtc = QDateTime::currentDateTimeUtc().addSecs(op * 10 + ev).toString(Qt::ISODateWithMs).toStdString();
                QVERIFY(repo.insertEvent(logEv));
            }
        }

        // Total operations in DB: 1 (opRetained) + 4 = 5 operations
        // Total events in DB: 1 + 12 = 13 events
        QCOMPARE(repo.countOperations(), 5);
        QCOMPARE(repo.countEvents(), 13);

        // Cap at 3 operations. Should purge 2 oldest operations in full (opRetained [1 ev] + multi-ev-op-1 [3 ev] = 4 events)
        int purged = repo.purgeOldOperations(180, 3);
        QCOMPARE(purged, 4);
        QCOMPARE(repo.countOperations(), 3);
        QCOMPARE(repo.countEvents(), 9);

        // Verify no partial operations exist: remaining ops multi-ev-op-2, 3, 4 each have exactly 3 events
        for (int op = 2; op <= 4; ++op) {
            LogFilter f;
            f.operationId = "multi-ev-op-" + std::to_string(op);
            auto events = repo.queryEvents(f);
            QCOMPARE(events.size(), 3u);
        }
    }

    // =========================================================================
    // 3. Album Render Parent-Child Cascade & Multi-Tier Hierarchy Purge
    //    Album Render (root) + 2 Track Renders (children) + 1 Dither (grandchild)
    //    must be counted as 1 logical operation and purged completely without orphans.
    // =========================================================================
    {
        std::string albumOpId = "op-album-master-hierarchical";
        std::string track1OpId = albumOpId + "-t1";
        std::string track2OpId = albumOpId + "-t2";
        std::string ditherOpId = track1OpId + "-dither";

        // Ancient timestamp so this album render is the oldest
        QString ancientTs = QDateTime::currentDateTimeUtc().addDays(-200).toString(Qt::ISODateWithMs);

        // Root album events (2 events)
        LogEvent albStart = LogEvent::create(LogLevel::Info, "Render", "Album started", "{}", "AlbumRender", "Folder", "/out", 0, 0.0, albumOpId, "");
        albStart.timestampUtc = ancientTs.toStdString();
        QVERIFY(repo.insertEvent(albStart));

        LogEvent albDone = LogEvent::create(LogLevel::Success, "Render", "Album committed", "{}", "AlbumRender", "Folder", "/out", 0, 0.0, albumOpId, "");
        albDone.timestampUtc = ancientTs.toStdString();
        QVERIFY(repo.insertEvent(albDone));

        // Child Track 1 (2 events)
        LogEvent t1Start = LogEvent::create(LogLevel::Info, "Render", "Track 1 start", "{}", "RenderTrack", "File", "01.wav", 0, 0.0, track1OpId, albumOpId);
        t1Start.timestampUtc = ancientTs.toStdString();
        QVERIFY(repo.insertEvent(t1Start));

        LogEvent t1Done = LogEvent::create(LogLevel::Success, "Render", "Track 1 done", "{}", "RenderTrack", "File", "01.wav", 0, 0.0, track1OpId, albumOpId);
        t1Done.timestampUtc = ancientTs.toStdString();
        QVERIFY(repo.insertEvent(t1Done));

        // Child Track 2 (2 events)
        LogEvent t2Start = LogEvent::create(LogLevel::Info, "Render", "Track 2 start", "{}", "RenderTrack", "File", "02.wav", 0, 0.0, track2OpId, albumOpId);
        t2Start.timestampUtc = ancientTs.toStdString();
        QVERIFY(repo.insertEvent(t2Start));

        LogEvent t2Done = LogEvent::create(LogLevel::Success, "Render", "Track 2 done", "{}", "RenderTrack", "File", "02.wav", 0, 0.0, track2OpId, albumOpId);
        t2Done.timestampUtc = ancientTs.toStdString();
        QVERIFY(repo.insertEvent(t2Done));

        // Grandchild Dither under Track 1 (1 event)
        LogEvent ditherEv = LogEvent::create(LogLevel::Info, "DSP", "TPDF dither applied", "{}", "Dither", "AudioBuffer", "", 0, 0.0, ditherOpId, track1OpId);
        ditherEv.timestampUtc = ancientTs.toStdString();
        QVERIFY(repo.insertEvent(ditherEv));

        // Prior state: 3 operations (9 events)
        // Added album tree: 1 logical root operation with 7 events across 4 distinct operation IDs
        // New totals: 4 operations, 16 events
        QCOMPARE(repo.countOperations(), 4);
        QCOMPARE(repo.countEvents(), 16);

        // Purge expired (>180 days). The album tree is 200 days old.
        // It must delete the root op, both child ops, and the grandchild op in one atomic transaction.
        int purgedRows = repo.purgeOldOperations(180, 1000);
        QCOMPARE(purgedRows, 7);
        QCOMPARE(repo.countOperations(), 3);
        QCOMPARE(repo.countEvents(), 9);

        // Verify zero orphan events remain anywhere in the album hierarchy
        LogFilter fAlb;
        fAlb.operationId = albumOpId;
        QCOMPARE(repo.queryEvents(fAlb).size(), 0u);

        LogFilter fT1;
        fT1.operationId = track1OpId;
        QCOMPARE(repo.queryEvents(fT1).size(), 0u);

        LogFilter fT2;
        fT2.operationId = track2OpId;
        QCOMPARE(repo.queryEvents(fT2).size(), 0u);

        LogFilter fDith;
        fDith.operationId = ditherOpId;
        QCOMPARE(repo.queryEvents(fDith).size(), 0u);

        // Also check by parentOperationId
        LogFilter fParent;
        fParent.parentOperationId = albumOpId;
        QCOMPARE(repo.queryEvents(fParent).size(), 0u);
    }

    repo.close();
}

void TestLogService::testXdgStatePathJsonlFilterParityAndOrphanPurge()
{
    // 1. Default database path uses XDG_STATE_HOME/desk32/operation_logs.db (Item 10)
    const QByteArray prevXdg = qgetenv("XDG_STATE_HOME");
    const QString customStateDir = m_tempDir.path() + "/xdg_state_test";
    qputenv("XDG_STATE_HOME", customStateDir.toUtf8());
    const QString expectedDb = customStateDir + "/desk32/operation_logs.db";
    QCOMPARE(LogService::defaultDatabasePath(), expectedDb);

    LogService::instance().stop();
    QVERIFY(LogService::instance().start(""));
    QVERIFY(QFile::exists(expectedDb));
    LogService::instance().stop();

    if (prevXdg.isEmpty()) {
        qunsetenv("XDG_STATE_HOME");
    } else {
        qputenv("XDG_STATE_HOME", prevXdg);
    }

    // 2. JSONL RFC-8259 escaping + full LogFilter parity + orphan child purge (Item 11)
    LogRepository repo("TestJsonlFilterOrphanConn");
    const QString repoDb = m_tempDir.path() + "/test_jsonl_filter_orphan.db";
    QVERIFY(repo.open(repoDb));

    QDateTime now = QDateTime::currentDateTimeUtc();
    std::vector<LogEvent> events;

    LogEvent specialEv;
    specialEv.operationId = "op-special-1";
    specialEv.parentOperationId = "parent-1";
    specialEv.timestampUtc = now.addSecs(-5).toString(Qt::ISODateWithMs).toStdString();
    specialEv.localUtcOffset = "+09:00";
    specialEv.category = "Render";
    specialEv.operation = "TrackWrite";
    specialEv.status = "Failed";
    specialEv.targetType = "AudioFile";
    specialEv.target = "dir/song \"quoted\"\\slash.wav";
    specialEv.message = "Line1\nLine2\t\"quoted\"\\backslash";
    specialEv.errorCode = 4001;
    specialEv.detailsJson = "{\"detail\":\"ok\"}";
    specialEv.durationMs = 12.5;
    events.push_back(specialEv);

    LogEvent orphanOld;
    orphanOld.operationId = "op-orphan-child";
    orphanOld.parentOperationId = "nonexistent-root-999";
    orphanOld.timestampUtc = now.addDays(-60).toString(Qt::ISODateWithMs).toStdString();
    orphanOld.localUtcOffset = "+00:00";
    orphanOld.category = "Workspace";
    orphanOld.operation = "ChildStep";
    orphanOld.status = "Success";
    orphanOld.targetType = "WorkspaceFile";
    orphanOld.target = "old.wav";
    orphanOld.message = "Old orphan child";
    orphanOld.errorCode = 0;
    orphanOld.detailsJson = "{}";
    events.push_back(orphanOld);

    QVERIFY(repo.insertBatch(events));

    // Verify all 8 filter fields work identically in exportToJsonl()
    LogFilter f;
    f.category = "Render";
    f.status = "Failed";
    f.operationId = "op-special-1";
    f.parentOperationId = "parent-1";
    f.target = "quoted";
    f.searchText = "Line2";
    const QString jsonlPath = m_tempDir.path() + "/filtered_special.jsonl";
    QVERIFY(repo.exportToJsonl(jsonlPath, f));

    QFile jf(jsonlPath);
    QVERIFY(jf.open(QIODevice::ReadOnly));
    const QByteArray rawLine = jf.readAll().trimmed();
    QVERIFY(!rawLine.contains('\n')); // Properly escaped single-line JSONL
    QJsonParseError perr{};
    const QJsonDocument parsed = QJsonDocument::fromJson(rawLine, &perr);
    QCOMPARE(perr.error, QJsonParseError::NoError);
    QVERIFY(parsed.isObject());
    const QJsonObject ro = parsed.object();
    QCOMPARE(ro["operation_id"].toString(), QString("op-special-1"));
    QCOMPARE(ro["parent_operation_id"].toString(), QString("parent-1"));
    QCOMPARE(ro["error_code"].toInt(), 4001);
    QCOMPARE(ro["message"].toString(), QString("Line1\nLine2\t\"quoted\"\\backslash"));

    // Purge >30 days removes the orphan child event while keeping the recent special event
    const int purged = repo.purgeOldOperations(30, 100000);
    QVERIFY(purged >= 1);
    QCOMPARE(repo.countEvents(), 1);
    LogFilter remF;
    remF.operationId = "op-special-1";
    QCOMPARE(repo.queryEvents(remF).size(), 1u);

    repo.close();
}

void TestLogService::cleanupTestCase()
{
    qDebug() << "TestLogService cleanup completed successfully.";
}

QTEST_MAIN(TestLogService)
#include "test_log_service.moc"

