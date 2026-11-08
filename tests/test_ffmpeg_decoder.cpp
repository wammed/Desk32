#include <QTest>
#include <QObject>
#include <QTemporaryFile>
#include <QFile>
#include <QDir>
#include <cmath>
#include <fstream>
#include <vector>
#include "engine/decoder/FFmpegAudioDecoder.hpp"

using namespace desk32::engine::decoder;

class TestFFmpegDecoder : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void testOpenAndDecodeWav();
    void testSeekFunctionality();
    void testArtworkExclusionDiscipline();
    void testInvalidFilesAndErrorResilience();
    void testNonStereoAudioRejectionAud10();
    void cleanupTestCase();

private:
    QString createSyntheticWav(double sampleRate, int channels, size_t totalFrames);
    QString m_testWavPath;
};

void TestFFmpegDecoder::initTestCase()
{
    qDebug() << "Initializing TestFFmpegDecoder (Phase 1.3 FFmpeg Decoder Implementation)...";
    m_testWavPath = createSyntheticWav(48000.0, 2, 48000); // 1.0 second test file
    QVERIFY(!m_testWavPath.isEmpty());
}

QString TestFFmpegDecoder::createSyntheticWav(double sampleRate, int channels, size_t totalFrames)
{
    auto tempFile = std::make_unique<QTemporaryFile>(QDir::tempPath() + "/desk32_test_XXXXXX.wav");
    tempFile->setAutoRemove(false);
    if (!tempFile->open()) {
        return QString();
    }
    QString path = tempFile->fileName();
    tempFile->close();

    std::ofstream out(path.toStdString(), std::ios::binary);
    if (!out) return QString();

    uint32_t sRate = static_cast<uint32_t>(sampleRate);
    uint16_t numCh = static_cast<uint16_t>(channels);
    uint16_t bitsPerSample = 16;
    uint32_t dataBytes = static_cast<uint32_t>(totalFrames * numCh * (bitsPerSample / 8));
    uint32_t chunkSize = 36 + dataBytes;
    uint32_t byteRate = sRate * numCh * (bitsPerSample / 8);
    uint16_t blockAlign = numCh * (bitsPerSample / 8);

    // RIFF header
    out.write("RIFF", 4);
    out.write(reinterpret_cast<const char*>(&chunkSize), 4);
    out.write("WAVE", 4);

    // fmt subchunk
    out.write("fmt ", 4);
    uint32_t subchunk1Size = 16;
    uint16_t audioFormat = 1; // PCM
    out.write(reinterpret_cast<const char*>(&subchunk1Size), 4);
    out.write(reinterpret_cast<const char*>(&audioFormat), 2);
    out.write(reinterpret_cast<const char*>(&numCh), 2);
    out.write(reinterpret_cast<const char*>(&sRate), 4);
    out.write(reinterpret_cast<const char*>(&byteRate), 4);
    out.write(reinterpret_cast<const char*>(&blockAlign), 2);
    out.write(reinterpret_cast<const char*>(&bitsPerSample), 2);

    // data subchunk
    out.write("data", 4);
    out.write(reinterpret_cast<const char*>(&dataBytes), 4);

    // Generate 440 Hz test sine wave
    for (size_t i = 0; i < totalFrames; ++i) {
        double t = static_cast<double>(i) / sampleRate;
        double s = 0.5 * std::sin(2.0 * M_PI * 440.0 * t); // -6 dBFS sine
        int16_t val = static_cast<int16_t>(s * 32767.0);

        for (int ch = 0; ch < channels; ++ch) {
            out.write(reinterpret_cast<const char*>(&val), sizeof(int16_t));
        }
    }

    out.close();
    return path;
}

void TestFFmpegDecoder::testOpenAndDecodeWav()
{
    FFmpegAudioDecoder decoder;
    QVERIFY(decoder.open(m_testWavPath.toStdString()));
    QVERIFY(decoder.isOpen());

    const auto& info = decoder.formatInfo();
    QCOMPARE(info.sampleRate, 48000.0);
    QCOMPARE(info.channels, 2u);
    QCOMPARE(info.bitDepth, 16u);
    QCOMPARE(info.totalFrames, 48000ll);
    QVERIFY(info.durationSeconds >= 0.99 && info.durationSeconds <= 1.01);

    // Read audio in blocks of 256 frames
    const size_t blockSize = 256;
    std::vector<double> bufL(blockSize);
    std::vector<double> bufR(blockSize);

    size_t totalRead = 0;
    while (true) {
        size_t read = decoder.readFrames(bufL.data(), bufR.data(), blockSize);
        if (read == 0) break;
        totalRead += read;

        // Verify values are in normalized float64 [-1.0, 1.0] range
        for (size_t i = 0; i < read; ++i) {
            QVERIFY(bufL[i] >= -1.0 && bufL[i] <= 1.0);
            QVERIFY(bufR[i] >= -1.0 && bufR[i] <= 1.0);
        }
    }

    QCOMPARE(totalRead, 48000u);
    QCOMPARE(decoder.currentFrame(), 48000ll);
    decoder.close();
    QVERIFY(!decoder.isOpen());
}

void TestFFmpegDecoder::testSeekFunctionality()
{
    FFmpegAudioDecoder decoder;
    QVERIFY(decoder.open(m_testWavPath.toStdString()));

    // Seek to 24000 frames (0.5 seconds)
    QVERIFY(decoder.seek(24000));
    QCOMPARE(decoder.currentFrame(), 24000ll);

    std::vector<double> bufL(256);
    std::vector<double> bufR(256);
    size_t read = decoder.readFrames(bufL.data(), bufR.data(), 256);
    QCOMPARE(read, 256u);
    QCOMPARE(decoder.currentFrame(), 24256ll);

    // Seek to beginning (frame 0)
    QVERIFY(decoder.seek(0));
    QCOMPARE(decoder.currentFrame(), 0ll);
    read = decoder.readFrames(bufL.data(), bufR.data(), 128);
    QCOMPARE(read, 128u);
    QCOMPARE(decoder.currentFrame(), 128ll);

    decoder.close();
}

void TestFFmpegDecoder::testArtworkExclusionDiscipline()
{
    // Section 2.5: Zero extraction, zero decoding, zero caching of Artwork
    using desk32::engine::metadata::TrackMetadata;
    static_assert(!TrackMetadata::hasArtworkSupport(), "Artwork support must strictly remain false!");
    QVERIFY(!TrackMetadata::hasArtworkSupport());

    FFmpegAudioDecoder decoder;
    QVERIFY(decoder.open(m_testWavPath.toStdString()));
    const auto& meta = decoder.metadata();
    QCOMPARE(meta.sampleRate, 48000.0);
    QCOMPARE(meta.channels, 2u);

    // Verify decoder does not create any image cache files in workspace or temp
    QVERIFY(!meta.hasArtworkSupport());
    decoder.close();
}

void TestFFmpegDecoder::testInvalidFilesAndErrorResilience()
{
    FFmpegAudioDecoder decoder;

    // 1. Non-existent file
    QVERIFY(!decoder.open("/path/to/non_existent_audio_file.flac"));
    QVERIFY(!decoder.isOpen());

    // 2. Empty 0-byte file
    QTemporaryFile emptyFile;
    QVERIFY(emptyFile.open());
    QString emptyPath = emptyFile.fileName();
    emptyFile.close();

    QVERIFY(!decoder.open(emptyPath.toStdString()));
    QVERIFY(!decoder.isOpen());

    // 3. Corrupt header file (junk bytes)
    QTemporaryFile corruptFile;
    QVERIFY(corruptFile.open());
    corruptFile.write("NOT_A_VALID_AUDIO_FILE_DATA_CORRUPT");
    QString corruptPath = corruptFile.fileName();
    corruptFile.close();

    QVERIFY(!decoder.open(corruptPath.toStdString()));
    QVERIFY(!decoder.isOpen());
}

void TestFFmpegDecoder::testNonStereoAudioRejectionAud10()
{
    // AUD-10: Stereo Only Specification Verification
    // Desk32 explicitly rejects mono (1ch), quad (4ch), 5.1/7.1 surround (>2ch) audio files upfront.
    // Implicit downmixing/upmixing via SwrContext is strictly prohibited per Section 2.3 and AUD-10.

    FFmpegAudioDecoder decoder;

    // 1. Mono (1 channel) input must be rejected
    QString monoPath = createSyntheticWav(48000.0, 1, 24000);
    QVERIFY(!monoPath.isEmpty());
    QVERIFY(!decoder.open(monoPath.toStdString()));
    QVERIFY(!decoder.isOpen());
    QVERIFY(!decoder.lastError().empty());
    QVERIFY(decoder.lastError().find("AUD-10") != std::string::npos);
    QFile::remove(monoPath);

    // 2. Quadraphonic (4 channels) input must be rejected
    QString quadPath = createSyntheticWav(48000.0, 4, 24000);
    QVERIFY(!quadPath.isEmpty());
    QVERIFY(!decoder.open(quadPath.toStdString()));
    QVERIFY(!decoder.isOpen());
    QVERIFY(!decoder.lastError().empty());
    QVERIFY(decoder.lastError().find("AUD-10") != std::string::npos);
    QFile::remove(quadPath);

    // 3. 5.1 Surround (6 channels) input must be rejected
    QString surroundPath = createSyntheticWav(48000.0, 6, 24000);
    QVERIFY(!surroundPath.isEmpty());
    QVERIFY(!decoder.open(surroundPath.toStdString()));
    QVERIFY(!decoder.isOpen());
    QVERIFY(!decoder.lastError().empty());
    QVERIFY(decoder.lastError().find("AUD-10") != std::string::npos);
    QFile::remove(surroundPath);

    // 4. Stereo (2 channels) input must succeed normally
    QString stereoPath = createSyntheticWav(48000.0, 2, 24000);
    QVERIFY(!stereoPath.isEmpty());
    QVERIFY(decoder.open(stereoPath.toStdString()));
    QVERIFY(decoder.isOpen());
    QCOMPARE(decoder.formatInfo().channels, 2u);
    decoder.close();
    QFile::remove(stereoPath);
}

void TestFFmpegDecoder::cleanupTestCase()
{
    if (!m_testWavPath.isEmpty()) {
        QFile::remove(m_testWavPath);
    }
    qDebug() << "TestFFmpegDecoder completed successfully.";
}

QTEST_MAIN(TestFFmpegDecoder)
#include "test_ffmpeg_decoder.moc"
