#include "WaveformExtractor.hpp"
#include "engine/decoder/FFmpegAudioDecoder.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <numbers>
#include <sys/stat.h>
#include <vector>

namespace desk32::engine::audio {

namespace {

constexpr uint32_t CACHE_FLAG_HAS_LUFS = 1u << 0;

struct CacheFileHeader {
    char magic[4] = {'D', 'W', 'F', 'M'};
    uint32_t version = 2;
    uint32_t buckets = WaveformData::DEFAULT_BUCKETS;
    uint32_t flags = 0;
    int64_t totalFrames = 0;
    double sampleRate = 0.0;
    double integratedLufs = -70.0;
};

struct KWeightBiquad {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0;
    double a1 = 0.0, a2 = 0.0;
    double z1L = 0.0, z2L = 0.0;
    double z1R = 0.0, z2R = 0.0;

    inline void processSample(double inL, double inR, double& outL, double& outR) noexcept {
        outL = (b0 * inL) + z1L;
        z1L  = (b1 * inL) - (a1 * outL) + z2L;
        z2L  = (b2 * inL) - (a2 * outL);

        outR = (b0 * inR) + z1R;
        z1R  = (b1 * inR) - (a1 * outR) + z2R;
        z2R  = (b2 * inR) - (a2 * outR);
    }
};

void computeKWeightingStages(double sampleRate, KWeightBiquad& stage1, KWeightBiquad& stage2) noexcept {
    const double fs = std::max(8000.0, sampleRate);
    if (std::abs(fs - 48000.0) < 0.5) {
        stage1 = KWeightBiquad{
            1.53512485958697, -2.69169618940638, 1.19839281085285,
            -1.69065929318241, 0.73248077421585
        };
        stage2 = KWeightBiquad{
            1.0, -2.0, 1.0,
            -1.99004745483398, 0.99007225036621
        };
        return;
    }
    if (std::abs(fs - 44100.0) < 0.5) {
        stage1 = KWeightBiquad{
            1.5308412300503478, -2.6509799951547297, 1.169079079921587,
            -1.6636551132560204, 0.7125954280732254
        };
        stage2 = KWeightBiquad{
            1.0, -2.0, 1.0,
            -1.989169673629796, 0.9891990357870393
        };
        return;
    }

    constexpr double f0_1 = 1681.974450955533;
    constexpr double G_1 = 3.999843853973347;
    constexpr double Q_1 = 0.7071752369554196;
    const double K1 = std::tan(std::numbers::pi * f0_1 / fs);
    const double Vh = std::pow(10.0, G_1 / 20.0);
    const double Vb = std::pow(Vh, 0.4996667741545416);
    const double a0_1 = 1.0 + (K1 / Q_1) + (K1 * K1);
    stage1 = KWeightBiquad{
        (Vh + (Vb * K1 / Q_1) + (K1 * K1)) / a0_1,
        2.0 * ((K1 * K1) - Vh) / a0_1,
        (Vh - (Vb * K1 / Q_1) + (K1 * K1)) / a0_1,
        2.0 * ((K1 * K1) - 1.0) / a0_1,
        (1.0 - (K1 / Q_1) + (K1 * K1)) / a0_1
    };

    constexpr double f0_2 = 38.13547087602444;
    constexpr double Q_2 = 0.5003270373238773;
    const double K2 = std::tan(std::numbers::pi * f0_2 / fs);
    const double a0_2 = 1.0 + (K2 / Q_2) + (K2 * K2);
    stage2 = KWeightBiquad{
        1.0,
        -2.0,
        1.0,
        2.0 * ((K2 * K2) - 1.0) / a0_2,
        (1.0 - (K2 / Q_2) + (K2 * K2)) / a0_2
    };
}

class IntegratedLufsAccumulator {
public:
    explicit IntegratedLufsAccumulator(double sampleRate, double expectedDurationSec = 0.0) {
        const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        computeKWeightingStages(sr, m_stage1, m_stage2);
        m_subBlockFrames = std::max<size_t>(1, static_cast<size_t>(std::llround(sr * 0.1)));
        if (expectedDurationSec > 0.0) {
            m_subPowers.reserve(static_cast<size_t>(expectedDurationSec * 10.0) + 16);
        }
    }

    inline void feed(double inL, double inR) {
        double s1L = 0.0, s1R = 0.0, kwL = 0.0, kwR = 0.0;
        m_stage1.processSample(inL, inR, s1L, s1R);
        m_stage2.processSample(s1L, s1R, kwL, kwR);
        const double p = (kwL * kwL) + (kwR * kwR);
        m_subSq += p;
        m_totalSq += p;
        ++m_subCount;
        ++m_totalFrames;
        if (m_subCount >= m_subBlockFrames) {
            m_subPowers.push_back(m_subSq / static_cast<double>(m_subCount));
            m_subSq = 0.0;
            m_subCount = 0;
        }
    }

    bool finalize(double& outLufs) {
        if (m_totalFrames == 0) {
            outLufs = -70.0;
            return false;
        }
        if (m_subCount >= (m_subBlockFrames / 2)) {
            m_subPowers.push_back(m_subSq / static_cast<double>(m_subCount));
        }

        const double absGatePower = std::pow(10.0, (-70.0 + 0.691) / 10.0);
        if (m_subPowers.size() < 4) {
            const double p = m_totalSq / static_cast<double>(m_totalFrames);
            if (p <= absGatePower) {
                outLufs = -70.0;
                return false;
            }
            outLufs = -0.691 + (10.0 * std::log10(std::max(p, 1e-12)));
            return true;
        }

        std::vector<double> blockPowers;
        blockPowers.reserve(m_subPowers.size() - 3);
        double absSum = 0.0;
        size_t absCount = 0;
        for (size_t i = 0; i + 3 < m_subPowers.size(); ++i) {
            const double bp = 0.25 * (m_subPowers[i] + m_subPowers[i + 1] + m_subPowers[i + 2] + m_subPowers[i + 3]);
            blockPowers.push_back(bp);
            if (bp > absGatePower) {
                absSum += bp;
                ++absCount;
            }
        }
        if (absCount == 0) {
            outLufs = -70.0;
            return false;
        }

        const double absMeanPower = absSum / static_cast<double>(absCount);
        const double finalGatePower = std::max(absGatePower, absMeanPower * 0.1);
        double gatedSum = 0.0;
        size_t gatedCount = 0;
        for (double bp : blockPowers) {
            if (bp > finalGatePower) {
                gatedSum += bp;
                ++gatedCount;
            }
        }
        if (gatedCount == 0) {
            outLufs = -70.0;
            return false;
        }

        const double gatedPower = gatedSum / static_cast<double>(gatedCount);
        outLufs = -0.691 + (10.0 * std::log10(std::max(gatedPower, 1e-12)));
        return true;
    }

private:
    KWeightBiquad m_stage1;
    KWeightBiquad m_stage2;
    size_t m_subBlockFrames = 4800;
    std::vector<double> m_subPowers;
    double m_subSq = 0.0;
    size_t m_subCount = 0;
    double m_totalSq = 0.0;
    size_t m_totalFrames = 0;
};

} // namespace

WaveformExtractor::WaveformExtractor() {
    m_worker = std::thread(&WaveformExtractor::workerLoop, this);
}

WaveformExtractor::~WaveformExtractor() {
    m_stop.store(true, std::memory_order_release);
    m_cv.notify_all();
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

void WaveformExtractor::setReadyCallback(ReadyCallback cb) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_readyCallback = std::move(cb);
}

std::string WaveformExtractor::canonicalizePath(const std::string& path) {
    if (path.empty()) return {};
    if (path.rfind("synthetic://", 0) == 0) return path;
    std::error_code ec;
    const auto cp = std::filesystem::weakly_canonical(path, ec);
    return ec ? path : cp.string();
}

std::string WaveformExtractor::canonicalizeCached(const std::string& path) const {
    if (path.empty()) return {};
    if (path.rfind("synthetic://", 0) == 0) return path;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_rawToCanon.find(path);
        if (it != m_rawToCanon.end()) {
            return it->second;
        }
    }
    const std::string canon = canonicalizePath(path);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_rawToCanon.size() > 2048) {
            m_rawToCanon.clear();
        }
        m_rawToCanon.emplace(path, canon);
    }
    return canon;
}

std::string WaveformExtractor::cacheDirectoryPath() {
    const QByteArray xdg = qgetenv("XDG_CACHE_HOME");
    const QString base = xdg.isEmpty() ? (QDir::homePath() + QStringLiteral("/.cache"))
                                       : QString::fromLocal8Bit(xdg);
    return QDir::cleanPath(base + QStringLiteral("/desk32/waveforms")).toStdString();
}

std::string WaveformExtractor::computeCacheFilePath(const std::string& canonPath) {
    qint64 fileSize = 0;
    qint64 mtimeMs = 0;
    struct stat st{};
    if (::stat(canonPath.c_str(), &st) == 0) {
        fileSize = static_cast<qint64>(st.st_size);
        mtimeMs = static_cast<qint64>(st.st_mtim.tv_sec) * 1000LL +
                  static_cast<qint64>(st.st_mtim.tv_nsec) / 1000000LL;
    }
    const QString key = QStringLiteral("%1|%2|%3|v1")
                            .arg(QString::fromStdString(canonPath))
                            .arg(fileSize)
                            .arg(mtimeMs);
    const QByteArray digest = QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex();
    return cacheDirectoryPath() + "/" + digest.toStdString() + ".wfm";
}

bool WaveformExtractor::loadFromDiskCache(const std::string& cacheFile,
                                          const std::string& canonPath,
                                          WaveformData& outData) {
    QFile f(QString::fromStdString(cacheFile));
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) {
        return false;
    }
    CacheFileHeader hdr{};
    if (f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr)) != static_cast<qint64>(sizeof(hdr))) {
        return false;
    }
    if (std::memcmp(hdr.magic, "DWFM", 4) != 0 || hdr.version != 2 ||
        hdr.buckets == 0 || hdr.buckets > 8192) {
        return false;
    }
    const qint64 expectedPayload = static_cast<qint64>(hdr.buckets) * 6 * static_cast<qint64>(sizeof(float));
    if (f.bytesAvailable() < expectedPayload) {
        return false;
    }

    WaveformData data;
    data.filePath = canonPath;
    data.sampleRate = hdr.sampleRate;
    data.totalFrames = hdr.totalFrames;
    data.bucketCount = hdr.buckets;
    data.integratedLufs = hdr.integratedLufs;
    data.hasLufs = (hdr.flags & CACHE_FLAG_HAS_LUFS) != 0;
    data.minL.resize(hdr.buckets);
    data.maxL.resize(hdr.buckets);
    data.minR.resize(hdr.buckets);
    data.maxR.resize(hdr.buckets);
    data.rmsL.resize(hdr.buckets);
    data.rmsR.resize(hdr.buckets);

    const qint64 arrBytes = static_cast<qint64>(hdr.buckets) * static_cast<qint64>(sizeof(float));
    if (f.read(reinterpret_cast<char*>(data.minL.data()), arrBytes) != arrBytes ||
        f.read(reinterpret_cast<char*>(data.maxL.data()), arrBytes) != arrBytes ||
        f.read(reinterpret_cast<char*>(data.minR.data()), arrBytes) != arrBytes ||
        f.read(reinterpret_cast<char*>(data.maxR.data()), arrBytes) != arrBytes ||
        f.read(reinterpret_cast<char*>(data.rmsL.data()), arrBytes) != arrBytes ||
        f.read(reinterpret_cast<char*>(data.rmsR.data()), arrBytes) != arrBytes) {
        return false;
    }
    data.valid = true;
    outData = std::move(data);
    return true;
}

bool WaveformExtractor::saveToDiskCache(const std::string& cacheFile, const WaveformData& data) {
    if (!data.valid || data.bucketCount == 0 ||
        data.rmsL.size() != data.bucketCount || data.rmsR.size() != data.bucketCount) {
        return false;
    }
    const QFileInfo fi(QString::fromStdString(cacheFile));
    QDir().mkpath(fi.absolutePath());

    QSaveFile sf(QString::fromStdString(cacheFile));
    if (!sf.open(QIODevice::WriteOnly)) {
        return false;
    }
    CacheFileHeader hdr{};
    hdr.version = 2;
    hdr.buckets = data.bucketCount;
    hdr.flags = data.hasLufs ? CACHE_FLAG_HAS_LUFS : 0u;
    hdr.totalFrames = data.totalFrames;
    hdr.sampleRate = data.sampleRate;
    hdr.integratedLufs = data.integratedLufs;
    sf.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    const qint64 arrBytes = static_cast<qint64>(data.bucketCount) * static_cast<qint64>(sizeof(float));
    sf.write(reinterpret_cast<const char*>(data.minL.data()), arrBytes);
    sf.write(reinterpret_cast<const char*>(data.maxL.data()), arrBytes);
    sf.write(reinterpret_cast<const char*>(data.minR.data()), arrBytes);
    sf.write(reinterpret_cast<const char*>(data.maxR.data()), arrBytes);
    sf.write(reinterpret_cast<const char*>(data.rmsL.data()), arrBytes);
    sf.write(reinterpret_cast<const char*>(data.rmsR.data()), arrBytes);
    return sf.commit();
}

bool WaveformExtractor::extractViaFfmpeg(const std::string& canonPath, WaveformData& outData) const {
    const uint32_t buckets = WaveformData::DEFAULT_BUCKETS;
    if (canonPath.rfind("synthetic://", 0) == 0) {
        WaveformData data;
        data.filePath = canonPath;
        data.sampleRate = 48000.0;
        data.totalFrames = 48000;
        data.bucketCount = buckets;
        data.minL.assign(buckets, -0.5f);
        data.maxL.assign(buckets, 0.5f);
        data.minR.assign(buckets, -0.5f);
        data.maxR.assign(buckets, 0.5f);
        const float sineRms = static_cast<float>(0.5 / std::sqrt(2.0));
        data.rmsL.assign(buckets, sineRms);
        data.rmsR.assign(buckets, sineRms);
        data.integratedLufs = -0.691 + (10.0 * std::log10(0.25));
        data.hasLufs = true;
        data.valid = true;
        outData = std::move(data);
        return true;
    }

    decoder::FFmpegAudioDecoder dec;
    if (!dec.open(canonPath)) {
        return false;
    }
    const auto fmt = dec.formatInfo();
    constexpr size_t CHUNK_FRAMES = 4096;
    std::vector<double> bufL(CHUNK_FRAMES, 0.0);
    std::vector<double> bufR(CHUNK_FRAMES, 0.0);

    WaveformData data;
    data.filePath = canonPath;
    data.sampleRate = fmt.sampleRate;
    data.totalFrames = fmt.totalFrames;
    data.bucketCount = buckets;
    data.minL.assign(buckets, 1.0f);
    data.maxL.assign(buckets, -1.0f);
    data.minR.assign(buckets, 1.0f);
    data.maxR.assign(buckets, -1.0f);
    data.rmsL.assign(buckets, 0.0f);
    data.rmsR.assign(buckets, 0.0f);

    IntegratedLufsAccumulator lufsAcc(fmt.sampleRate, fmt.durationSeconds);

    if (fmt.totalFrames > 0) {
        std::vector<bool> touched(buckets, false);
        std::vector<double> sumSqL(buckets, 0.0);
        std::vector<double> sumSqR(buckets, 0.0);
        std::vector<uint64_t> counts(buckets, 0);
        int64_t framePos = 0;
        while (!m_stop.load(std::memory_order_acquire)) {
            const size_t n = dec.readFrames(bufL.data(), bufR.data(), CHUNK_FRAMES);
            if (n == 0) break;
            for (size_t i = 0; i < n; ++i) {
                const double rawL = bufL[i];
                const double rawR = bufR[i];
                lufsAcc.feed(rawL, rawR);

                const int64_t f = framePos + static_cast<int64_t>(i);
                const size_t b = static_cast<size_t>(
                    std::clamp<int64_t>((f * static_cast<int64_t>(buckets)) / fmt.totalFrames,
                                        0, static_cast<int64_t>(buckets) - 1));
                const float lv = static_cast<float>(std::clamp(rawL, -1.0, 1.0));
                const float rv = static_cast<float>(std::clamp(rawR, -1.0, 1.0));
                if (!touched[b]) {
                    data.minL[b] = data.maxL[b] = lv;
                    data.minR[b] = data.maxR[b] = rv;
                    touched[b] = true;
                } else {
                    data.minL[b] = std::min(data.minL[b], lv);
                    data.maxL[b] = std::max(data.maxL[b], lv);
                    data.minR[b] = std::min(data.minR[b], rv);
                    data.maxR[b] = std::max(data.maxR[b], rv);
                }
                sumSqL[b] += static_cast<double>(lv) * static_cast<double>(lv);
                sumSqR[b] += static_cast<double>(rv) * static_cast<double>(rv);
                ++counts[b];
            }
            framePos += static_cast<int64_t>(n);
        }
        if (m_stop.load(std::memory_order_acquire)) return false;
        for (size_t b = 0; b < buckets; ++b) {
            if (!touched[b] || counts[b] == 0) {
                data.minL[b] = data.maxL[b] = 0.0f;
                data.minR[b] = data.maxR[b] = 0.0f;
                data.rmsL[b] = data.rmsR[b] = 0.0f;
            } else {
                const double invN = 1.0 / static_cast<double>(counts[b]);
                data.rmsL[b] = static_cast<float>(std::clamp(std::sqrt(sumSqL[b] * invN), 0.0, 1.0));
                data.rmsR[b] = static_cast<float>(std::clamp(std::sqrt(sumSqR[b] * invN), 0.0, 1.0));
            }
        }
        if (framePos > data.totalFrames) {
            data.totalFrames = framePos;
        }
    } else {
        // Unknown totalFrames fallback: accumulate coarse peaks & RMS and resample to 1024 buckets.
        struct BlockPeak { float minL, maxL, minR, maxR; double sumSqL, sumSqR; uint64_t count; };
        std::vector<BlockPeak> coarse;
        int64_t totalRead = 0;
        while (!m_stop.load(std::memory_order_acquire)) {
            const size_t n = dec.readFrames(bufL.data(), bufR.data(), CHUNK_FRAMES);
            if (n == 0) break;
            float mnL = 1.0f, mxL = -1.0f, mnR = 1.0f, mxR = -1.0f;
            double sqL = 0.0, sqR = 0.0;
            for (size_t i = 0; i < n; ++i) {
                const double rawL = bufL[i];
                const double rawR = bufR[i];
                lufsAcc.feed(rawL, rawR);
                const float lv = static_cast<float>(std::clamp(rawL, -1.0, 1.0));
                const float rv = static_cast<float>(std::clamp(rawR, -1.0, 1.0));
                mnL = std::min(mnL, lv); mxL = std::max(mxL, lv);
                mnR = std::min(mnR, rv); mxR = std::max(mxR, rv);
                sqL += static_cast<double>(lv) * static_cast<double>(lv);
                sqR += static_cast<double>(rv) * static_cast<double>(rv);
            }
            coarse.push_back({mnL, mxL, mnR, mxR, sqL, sqR, static_cast<uint64_t>(n)});
            totalRead += static_cast<int64_t>(n);
        }
        if (m_stop.load(std::memory_order_acquire)) return false;
        data.totalFrames = totalRead;
        if (coarse.empty()) {
            std::fill(data.minL.begin(), data.minL.end(), 0.0f);
            std::fill(data.maxL.begin(), data.maxL.end(), 0.0f);
            std::fill(data.minR.begin(), data.minR.end(), 0.0f);
            std::fill(data.maxR.begin(), data.maxR.end(), 0.0f);
        } else {
            for (size_t b = 0; b < buckets; ++b) {
                const size_t s0 = (b * coarse.size()) / buckets;
                const size_t s1 = std::max(s0 + 1, ((b + 1) * coarse.size()) / buckets);
                float mnL = 1.0f, mxL = -1.0f, mnR = 1.0f, mxR = -1.0f;
                double sqL = 0.0, sqR = 0.0;
                uint64_t cnt = 0;
                for (size_t s = s0; s < s1 && s < coarse.size(); ++s) {
                    mnL = std::min(mnL, coarse[s].minL); mxL = std::max(mxL, coarse[s].maxL);
                    mnR = std::min(mnR, coarse[s].minR); mxR = std::max(mxR, coarse[s].maxR);
                    sqL += coarse[s].sumSqL;
                    sqR += coarse[s].sumSqR;
                    cnt += coarse[s].count;
                }
                data.minL[b] = mnL; data.maxL[b] = mxL;
                data.minR[b] = mnR; data.maxR[b] = mxR;
                if (cnt > 0) {
                    data.rmsL[b] = static_cast<float>(std::clamp(std::sqrt(sqL / static_cast<double>(cnt)), 0.0, 1.0));
                    data.rmsR[b] = static_cast<float>(std::clamp(std::sqrt(sqR / static_cast<double>(cnt)), 0.0, 1.0));
                }
            }
        }
    }

    dec.close();
    data.hasLufs = lufsAcc.finalize(data.integratedLufs);
    data.valid = (data.totalFrames > 0);
    outData = std::move(data);
    return outData.valid;
}

void WaveformExtractor::requestWaveform(const std::string& filePath) {
    if (filePath.empty()) return;
    const std::string canon = canonicalizeCached(filePath);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_memoryCache.find(canon) != m_memoryCache.end()) {
            return;
        }
        if (m_queuedOrActive.find(canon) != m_queuedOrActive.end()) {
            return;
        }
        m_queuedOrActive.insert(canon);
        m_queue.push_back(canon);
    }
    m_cv.notify_one();
}

bool WaveformExtractor::isExtracting(const std::string& filePath) const {
    if (filePath.empty()) return false;
    const std::string canon = canonicalizeCached(filePath);
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queuedOrActive.find(canon) != m_queuedOrActive.end();
}

bool WaveformExtractor::hasWaveform(const std::string& filePath) const {
    if (filePath.empty()) return false;
    const std::string canon = canonicalizeCached(filePath);
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_memoryCache.find(canon);
    return it != m_memoryCache.end() && it->second.valid;
}

bool WaveformExtractor::getWaveform(const std::string& filePath, WaveformData& outData) const {
    if (filePath.empty()) return false;
    const std::string canon = canonicalizeCached(filePath);
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_memoryCache.find(canon);
    if (it == m_memoryCache.end() || !it->second.valid) {
        return false;
    }
    outData = it->second;
    return true;
}

TrackLufsInfo WaveformExtractor::queryTrackLufs(const std::string& filePath) const {
    TrackLufsInfo info;
    if (filePath.empty()) return info;
    const std::string canon = canonicalizeCached(filePath);
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_memoryCache.find(canon);
    if (it != m_memoryCache.end() && it->second.valid) {
        info.ready = true;
        info.hasLufs = it->second.hasLufs;
        info.lufs = it->second.integratedLufs;
        info.measuring = false;
        return info;
    }
    info.measuring = (m_queuedOrActive.find(canon) != m_queuedOrActive.end());
    return info;
}

uint64_t WaveformExtractor::diskCacheSizeBytes() const {
    uint64_t total = 0;
    QDir dir(QString::fromStdString(cacheDirectoryPath()));
    if (!dir.exists()) return 0;
    const auto entries = dir.entryInfoList(QStringList{QStringLiteral("*.wfm")}, QDir::Files | QDir::NoDotAndDotDot);
    for (const QFileInfo& fi : entries) {
        total += static_cast<uint64_t>(fi.size());
    }
    return total;
}

size_t WaveformExtractor::diskCacheFileCount() const {
    QDir dir(QString::fromStdString(cacheDirectoryPath()));
    if (!dir.exists()) return 0;
    return static_cast<size_t>(dir.entryInfoList(QStringList{QStringLiteral("*.wfm")}, QDir::Files | QDir::NoDotAndDotDot).size());
}

bool WaveformExtractor::clearDiskCache() {
    QDir dir(QString::fromStdString(cacheDirectoryPath()));
    if (!dir.exists()) return true;
    const auto entries = dir.entryInfoList(QStringList{QStringLiteral("*.wfm")}, QDir::Files | QDir::NoDotAndDotDot);
    bool allOk = true;
    for (const QFileInfo& fi : entries) {
        if (!QFile::remove(fi.absoluteFilePath())) {
            allOk = false;
        }
    }
    return allOk;
}

void WaveformExtractor::workerLoop() {
    while (true) {
        std::string jobPath;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this]() {
                return m_stop.load(std::memory_order_acquire) || !m_queue.empty();
            });
            if (m_stop.load(std::memory_order_acquire)) {
                return;
            }
            jobPath = std::move(m_queue.front());
            m_queue.pop_front();
        }

        const bool isSynthetic = (jobPath.rfind("synthetic://", 0) == 0);
        const std::string cacheFile = isSynthetic ? std::string() : computeCacheFilePath(jobPath);
        WaveformData result;
        bool ok = !isSynthetic && loadFromDiskCache(cacheFile, jobPath, result);
        if (!ok && !m_stop.load(std::memory_order_acquire)) {
            ok = extractViaFfmpeg(jobPath, result);
            if (ok && !isSynthetic) {
                saveToDiskCache(cacheFile, result);
            }
        }

        ReadyCallback cb;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (ok) {
                m_memoryCache[jobPath] = result;
            }
            m_queuedOrActive.erase(jobPath);
            cb = m_readyCallback;
        }
        if (cb && !m_stop.load(std::memory_order_acquire)) {
            cb(jobPath, ok);
        }
    }
}

} // namespace desk32::engine::audio
