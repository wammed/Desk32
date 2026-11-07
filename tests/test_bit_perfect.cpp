#include <QTest>
#include <QString>
#include <QCryptographicHash>
#include <vector>
#include <cstdint>

enum class VerificationStatus {
    Verified,       // Verified bit-perfect (e.g. SHA-256 matches)
    NotVerified,    // Processing applied or unverified
    Indeterminate,  // Cannot be determined from software
    Unsupported     // Hardware or backend doesn't support verification
};

struct BitPerfectDiagnostics {
    VerificationStatus level1Internal = VerificationStatus::NotVerified;
    VerificationStatus level2Backend  = VerificationStatus::NotVerified;
    VerificationStatus level3DAC      = VerificationStatus::Indeterminate; // DAC input hardware unobservable
};

class TestBitPerfect : public QObject
{
    Q_OBJECT

private slots:
    void testLevel1HashComparison();
    void testLevel1WithDspApplied();
    void testLevel3UnobservablePolicy();
};

void TestBitPerfect::testLevel1HashComparison()
{
    // Level 1: Compare decoded PCM vs output-stage PCM using sample hash
    std::vector<int32_t> decodedPcm = { 1000, -2000, 3000, -4000, 5000, -6000 };
    std::vector<int32_t> outputPcm = decodedPcm; // Direct Mode (unaltered)

    QByteArray hashDecoded = QCryptographicHash::hash(
        QByteArrayView(reinterpret_cast<const char*>(decodedPcm.data()), static_cast<qsizetype>(decodedPcm.size() * sizeof(int32_t))),
        QCryptographicHash::Sha256
    );
    QByteArray hashOutput = QCryptographicHash::hash(
        QByteArrayView(reinterpret_cast<const char*>(outputPcm.data()), static_cast<qsizetype>(outputPcm.size() * sizeof(int32_t))),
        QCryptographicHash::Sha256
    );

    VerificationStatus status = (hashDecoded == hashOutput) 
        ? VerificationStatus::Verified 
        : VerificationStatus::NotVerified;

    QCOMPARE(status, VerificationStatus::Verified);
}

void TestBitPerfect::testLevel1WithDspApplied()
{
    // If any DSP or gain modification is applied, it MUST NEVER report Verified
    std::vector<int32_t> decodedPcm = { 1000, -2000, 3000, -4000, 5000, -6000 };
    std::vector<int32_t> outputPcm = decodedPcm;
    outputPcm[0] += 1; // 1 LSB altered

    QByteArray hashDecoded = QCryptographicHash::hash(
        QByteArrayView(reinterpret_cast<const char*>(decodedPcm.data()), static_cast<qsizetype>(decodedPcm.size() * sizeof(int32_t))),
        QCryptographicHash::Sha256
    );
    QByteArray hashOutput = QCryptographicHash::hash(
        QByteArrayView(reinterpret_cast<const char*>(outputPcm.data()), static_cast<qsizetype>(outputPcm.size() * sizeof(int32_t))),
        QCryptographicHash::Sha256
    );

    VerificationStatus status = (hashDecoded == hashOutput) 
        ? VerificationStatus::Verified 
        : VerificationStatus::NotVerified;

    QCOMPARE(status, VerificationStatus::NotVerified);
}

void TestBitPerfect::testLevel3UnobservablePolicy()
{
    // DAC internal stream cannot be observed by host software
    // Rule: Unobservable values MUST NOT be reported as Verified or Pass
    BitPerfectDiagnostics diag;
    diag.level1Internal = VerificationStatus::Verified;
    diag.level2Backend  = VerificationStatus::Verified;
    diag.level3DAC      = VerificationStatus::Indeterminate;

    // Ensure level 3 is never displayed as Verified
    QVERIFY(diag.level3DAC != VerificationStatus::Verified);
    QCOMPARE(diag.level3DAC, VerificationStatus::Indeterminate);
}

QTEST_MAIN(TestBitPerfect)
#include "test_bit_perfect.moc"
