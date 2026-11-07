#include <QTest>
#include <QFile>
#include <QDir>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

class TestNetworkOffline : public QObject
{
    Q_OBJECT

private slots:
    void testNoNetworkLibrariesLinked();
    void testProcessSocketInspection();
};

void TestNetworkOffline::testNoNetworkLibrariesLinked()
{
    // Desk32 must NEVER link against Qt Network or any networking client library
#if defined(QT_NETWORK_LIB)
    QFAIL("FATAL: Qt Network library is linked into Desk32 test suite!");
#endif
    QVERIFY(true);
}

void TestNetworkOffline::testProcessSocketInspection()
{
    // Inspect process file descriptors under /proc/self/fd/ to verify no internet sockets (AF_INET / AF_INET6) are open
    QDir fdDir("/proc/self/fd");
    QStringList fds = fdDir.entryList(QDir::Files | QDir::System | QDir::NoDotAndDotDot);

    int inetSocketCount = 0;
    for (const QString& fdStr : fds) {
        int fd = fdStr.toInt();
        struct sockaddr_storage addr;
        socklen_t len = sizeof(addr);
        if (getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &len) == 0) {
            if (addr.ss_family == AF_INET || addr.ss_family == AF_INET6) {
                inetSocketCount++;
            }
        }
    }

    // Must be ZERO internet sockets
    QCOMPARE(inetSocketCount, 0);
}

QTEST_MAIN(TestNetworkOffline)
#include "test_network_offline.moc"

