#include <QTest>
#include <QObject>
#include <latch>
#include <chrono>
#include <cstdlib>
#include <atomic>
#include <execinfo.h>
#include <unistd.h>
#include <dlfcn.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include "engine/pipeline/AudioPipeline.hpp"
#include "engine/pipeline/BuiltInDspNodes.hpp"
#include "engine/insert/ExternalInsertNode.hpp"
#include "core/log/BoundedLogQueue.hpp"
#include <numeric>
#include <algorithm>

struct RtAllocationTracker {
    std::atomic<size_t> opNewCount{0};
    std::atomic<size_t> opNewArrayCount{0};
    std::atomic<size_t> opNewNothrowCount{0};
    std::atomic<size_t> opNewAlignedCount{0};
    std::atomic<size_t> opDeleteCount{0};
    std::atomic<size_t> opDeleteArrayCount{0};
    std::atomic<size_t> cMallocCount{0};
    std::atomic<size_t> cCallocCount{0};
    std::atomic<size_t> cReallocCount{0};
    std::atomic<size_t> cFreeCount{0};
    std::atomic<size_t> cMemalignCount{0};
    std::atomic<size_t> rtBlockingCount{0};

    void reset() noexcept {
        opNewCount.store(0);
        opNewArrayCount.store(0);
        opNewNothrowCount.store(0);
        opNewAlignedCount.store(0);
        opDeleteCount.store(0);
        opDeleteArrayCount.store(0);
        cMallocCount.store(0);
        cCallocCount.store(0);
        cReallocCount.store(0);
        cFreeCount.store(0);
        cMemalignCount.store(0);
        rtBlockingCount.store(0);
    }

    [[nodiscard]] size_t totalAllocations() const noexcept {
        return opNewCount.load() + opNewArrayCount.load() + opNewNothrowCount.load() +
               opNewAlignedCount.load() + cMallocCount.load() + cCallocCount.load() +
               cReallocCount.load() + cMemalignCount.load();
    }

    [[nodiscard]] size_t totalDeallocations() const noexcept {
        return opDeleteCount.load() + opDeleteArrayCount.load() + cFreeCount.load();
    }
};

static RtAllocationTracker s_rtStats;
static thread_local bool t_isAudioThread{false};

extern "C" {
void* __libc_malloc(size_t);
void* __libc_calloc(size_t, size_t);
void* __libc_realloc(void*, size_t);
void __libc_free(void*);
void* __libc_memalign(size_t, size_t);
}

#if !defined(__SANITIZE_THREAD__) && !defined(__SANITIZE_ADDRESS__)
extern "C" {
void* malloc(size_t size) {
    if (t_isAudioThread) {
        s_rtStats.cMallocCount.fetch_add(1, std::memory_order_relaxed);
    }
    return __libc_malloc(size);
}

void* calloc(size_t nmemb, size_t size) {
    if (t_isAudioThread) {
        s_rtStats.cCallocCount.fetch_add(1, std::memory_order_relaxed);
    }
    return __libc_calloc(nmemb, size);
}

void* realloc(void* ptr, size_t size) {
    if (t_isAudioThread) {
        s_rtStats.cReallocCount.fetch_add(1, std::memory_order_relaxed);
    }
    return __libc_realloc(ptr, size);
}

void free(void* ptr) {
    if (t_isAudioThread) {
        s_rtStats.cFreeCount.fetch_add(1, std::memory_order_relaxed);
    }
    __libc_free(ptr);
}

int posix_memalign(void** memptr, size_t alignment, size_t size) {
    if (t_isAudioThread) {
        s_rtStats.cMemalignCount.fetch_add(1, std::memory_order_relaxed);
    }
    void* ptr = __libc_memalign(alignment, size);
    if (!ptr) return 12; // ENOMEM
    *memptr = ptr;
    return 0;
}

void* aligned_alloc(size_t alignment, size_t size) {
    if (t_isAudioThread) {
        s_rtStats.cMemalignCount.fetch_add(1, std::memory_order_relaxed);
    }
    return __libc_memalign(alignment, size);
}
} // extern "C"
#endif

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

void* operator new(size_t size) {
    if (t_isAudioThread) {
        s_rtStats.opNewCount.fetch_add(1, std::memory_order_relaxed);
    }
    return malloc(size);
}

void* operator new(size_t size, const std::nothrow_t&) noexcept {
    if (t_isAudioThread) {
        s_rtStats.opNewNothrowCount.fetch_add(1, std::memory_order_relaxed);
    }
    return malloc(size);
}

void* operator new[](size_t size) {
    if (t_isAudioThread) {
        s_rtStats.opNewArrayCount.fetch_add(1, std::memory_order_relaxed);
    }
    return malloc(size);
}

void* operator new[](size_t size, const std::nothrow_t&) noexcept {
    if (t_isAudioThread) {
        s_rtStats.opNewNothrowCount.fetch_add(1, std::memory_order_relaxed);
    }
    return malloc(size);
}

void* operator new(size_t size, std::align_val_t al) {
    if (t_isAudioThread) {
        s_rtStats.opNewAlignedCount.fetch_add(1, std::memory_order_relaxed);
    }
    void* ptr = nullptr;
    if (posix_memalign(&ptr, static_cast<size_t>(al), size) != 0) {
        throw std::bad_alloc();
    }
    return ptr;
}

void* operator new[](size_t size, std::align_val_t al) {
    if (t_isAudioThread) {
        s_rtStats.opNewAlignedCount.fetch_add(1, std::memory_order_relaxed);
    }
    void* ptr = nullptr;
    if (posix_memalign(&ptr, static_cast<size_t>(al), size) != 0) {
        throw std::bad_alloc();
    }
    return ptr;
}

void operator delete(void* ptr) noexcept {
    if (t_isAudioThread) {
        s_rtStats.opDeleteCount.fetch_add(1, std::memory_order_relaxed);
    }
    free(ptr);
}

void operator delete[](void* ptr) noexcept {
    if (t_isAudioThread) {
        s_rtStats.opDeleteArrayCount.fetch_add(1, std::memory_order_relaxed);
    }
    free(ptr);
}

void operator delete(void* ptr, size_t) noexcept {
    if (t_isAudioThread) {
        s_rtStats.opDeleteCount.fetch_add(1, std::memory_order_relaxed);
    }
    free(ptr);
}

void operator delete[](void* ptr, size_t) noexcept {
    if (t_isAudioThread) {
        s_rtStats.opDeleteArrayCount.fetch_add(1, std::memory_order_relaxed);
    }
    free(ptr);
}

void operator delete(void* ptr, std::align_val_t) noexcept {
    if (t_isAudioThread) {
        s_rtStats.opDeleteCount.fetch_add(1, std::memory_order_relaxed);
    }
    free(ptr);
}

void operator delete[](void* ptr, std::align_val_t) noexcept {
    if (t_isAudioThread) {
        s_rtStats.opDeleteArrayCount.fetch_add(1, std::memory_order_relaxed);
    }
    free(ptr);
}

void operator delete(void* ptr, size_t, std::align_val_t) noexcept {
    if (t_isAudioThread) {
        s_rtStats.opDeleteCount.fetch_add(1, std::memory_order_relaxed);
    }
    free(ptr);
}

void operator delete[](void* ptr, size_t, std::align_val_t) noexcept {
    if (t_isAudioThread) {
        s_rtStats.opDeleteArrayCount.fetch_add(1, std::memory_order_relaxed);
    }
    free(ptr);
}

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

extern "C" {

int pthread_mutex_lock(pthread_mutex_t* mutex) {
    static auto orig = reinterpret_cast<int(*)(pthread_mutex_t*)>(dlsym(RTLD_NEXT, "pthread_mutex_lock"));
    if (t_isAudioThread) {
        s_rtStats.rtBlockingCount.fetch_add(1, std::memory_order_relaxed);
    }
    return orig(mutex);
}

int pthread_mutex_trylock(pthread_mutex_t* mutex) {
    static auto orig = reinterpret_cast<int(*)(pthread_mutex_t*)>(dlsym(RTLD_NEXT, "pthread_mutex_trylock"));
    if (t_isAudioThread) {
        s_rtStats.rtBlockingCount.fetch_add(1, std::memory_order_relaxed);
    }
    return orig(mutex);
}

int pthread_mutex_timedlock(pthread_mutex_t* mutex, const struct timespec* abstime) {
    static auto orig = reinterpret_cast<int(*)(pthread_mutex_t*, const struct timespec*)>(dlsym(RTLD_NEXT, "pthread_mutex_timedlock"));
    if (t_isAudioThread) {
        s_rtStats.rtBlockingCount.fetch_add(1, std::memory_order_relaxed);
    }
    return orig(mutex, abstime);
}

int pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex) {
    static auto orig = reinterpret_cast<int(*)(pthread_cond_t*, pthread_mutex_t*)>(dlsym(RTLD_NEXT, "pthread_cond_wait"));
    if (t_isAudioThread) {
        s_rtStats.rtBlockingCount.fetch_add(1, std::memory_order_relaxed);
    }
    return orig(cond, mutex);
}

int pthread_cond_timedwait(pthread_cond_t* cond, pthread_mutex_t* mutex, const struct timespec* abstime) {
    static auto orig = reinterpret_cast<int(*)(pthread_cond_t*, pthread_mutex_t*, const struct timespec*)>(dlsym(RTLD_NEXT, "pthread_cond_timedwait"));
    if (t_isAudioThread) {
        s_rtStats.rtBlockingCount.fetch_add(1, std::memory_order_relaxed);
    }
    return orig(cond, mutex, abstime);
}

int sem_wait(sem_t* sem) {
    static auto orig = reinterpret_cast<int(*)(sem_t*)>(dlsym(RTLD_NEXT, "sem_wait"));
    if (t_isAudioThread) {
        s_rtStats.rtBlockingCount.fetch_add(1, std::memory_order_relaxed);
    }
    return orig(sem);
}

int sem_trywait(sem_t* sem) {
    static auto orig = reinterpret_cast<int(*)(sem_t*)>(dlsym(RTLD_NEXT, "sem_trywait"));
    if (t_isAudioThread) {
        s_rtStats.rtBlockingCount.fetch_add(1, std::memory_order_relaxed);
    }
    return orig(sem);
}

int nanosleep(const struct timespec* req, struct timespec* rem) {
    static auto orig = reinterpret_cast<int(*)(const struct timespec*, struct timespec*)>(dlsym(RTLD_NEXT, "nanosleep"));
    if (t_isAudioThread) {
        s_rtStats.rtBlockingCount.fetch_add(1, std::memory_order_relaxed);
    }
    return orig(req, rem);
}

int usleep(useconds_t usec) {
    static auto orig = reinterpret_cast<int(*)(useconds_t)>(dlsym(RTLD_NEXT, "usleep"));
    if (t_isAudioThread) {
        s_rtStats.rtBlockingCount.fetch_add(1, std::memory_order_relaxed);
    }
    return orig(usec);
}

unsigned int sleep(unsigned int seconds) {
    static auto orig = reinterpret_cast<unsigned int(*)(unsigned int)>(dlsym(RTLD_NEXT, "sleep"));
    if (t_isAudioThread) {
        s_rtStats.rtBlockingCount.fetch_add(1, std::memory_order_relaxed);
    }
    return orig(seconds);
}

int sched_yield() {
    static auto orig = reinterpret_cast<int(*)()>(dlsym(RTLD_NEXT, "sched_yield"));
    if (t_isAudioThread) {
        s_rtStats.rtBlockingCount.fetch_add(1, std::memory_order_relaxed);
    }
    return orig();
}

} // extern "C"

using namespace desk32::engine::pipeline;

class TestAudioPipeline : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void testNodeAffiliationStructuralVerification();
    void testAudioPipelineFormatConversionAndClamping();
    void testOutputPathRoutingAndZeroLeakage();
    void testAudioGraphRebuildClonesNodesWithoutSharing();
    void testEpochReclamationHotSwap();
    void testEpochReclaimerConcurrentStress();
    void testEpochReclaimerMultiReaderConcurrentStress();
    void testSampleRateChangeConcurrentWithTelemetry();
    void testShutdownWaitsForActiveReaders();
    void testReaderSlotExhaustionRejectsUnshieldedPointer();
    void testDspInstanceContinuousExchangeAndRetirement();
    void testReverbTailPreservationCrossfade();
    void testLimiterLookaheadChangeSampleContinuity();
    void testTelemetrySnapshotThreadSafety();
    void testEpochReclaimerShutdownConcurrentSafety();
    void testEpochReclaimerWriterShutdownSerialization();
    void testEpochReclaimerDeterministicShutdownRaces();
    void testEpochReclaimerTimeoutSafetyAndThreadCleanup();
    void testRealTimeSafetyInLivePipeline();
    void testRealtimeSafetyNegativeDetection();
    void testAudioPipelineWorstCaseRealtimeConditions();
    void testAudioChainMaxNodesAndBoundedCommandDrain();
    void testAudioThreadRealtimeLogQueueAndLatencyDistribution();
    void cleanupTestCase();
};

void TestAudioPipeline::initTestCase()
{
    qDebug() << "Initializing TestAudioPipeline (Phase 1.1 Core Engine Integration)...";
}

void TestAudioPipeline::testNodeAffiliationStructuralVerification()
{
    // Section 9.2: "Nodes belonging to Output Path declare affiliation, structurally verifying
    // that Listening Correction cannot be connected to Render branches."

    auto masterChain = std::make_shared<AudioChain>("Mastering", ChainPurpose::MasteringChain);
    auto listeningChain = std::make_shared<AudioChain>("Listening", ChainPurpose::ListeningCorrectionChain);
    auto renderMonChain = std::make_shared<AudioChain>("RenderMonitor", ChainPurpose::RenderMonitorChain);

    auto peqMaster = std::make_shared<ParametricEqNode>("Master PEQ", NodeAffiliation::RenderCommon);
    auto peqListen = std::make_shared<ParametricEqNode>("Headphone EQ", NodeAffiliation::ListeningCorrection);
    auto geq = std::make_shared<GraphicEqNode>("Console GEQ");

    // 1. RenderCommon can be appended to MasteringChain
    QVERIFY(masterChain->appendNode(peqMaster));
    QVERIFY(masterChain->appendNode(geq));
    QCOMPARE(masterChain->nodeCount(), 2);

    // 2. ListeningCorrection node MUST BE REJECTED from MasteringChain
    QVERIFY(!masterChain->appendNode(peqListen));
    QCOMPARE(masterChain->nodeCount(), 2); // Count unchanged

    // 3. ListeningCorrection node MUST BE REJECTED from RenderMonitorChain
    QVERIFY(!renderMonChain->appendNode(peqListen));

    // 4. ListeningCorrection node can be appended to ListeningCorrectionChain
    QVERIFY(listeningChain->appendNode(peqListen));
    QCOMPARE(listeningChain->nodeCount(), 1);
}

void TestAudioPipeline::testAudioPipelineFormatConversionAndClamping()
{
    AudioPipeline pipeline(48000.0);
    const size_t frames = 64;

    std::vector<double> inL(frames, 2.0); // +6 dBFS overload (> 1.0)
    std::vector<double> inR(frames, -2.0);

    // Test output to Float32
    std::vector<float> outF32(frames * 2, 0.0f);
    pipeline.process(inL.data(), inR.data(), frames, outF32.data(), SampleFormat::Float32, false);

    for (size_t i = 0; i < frames; ++i) {
        QCOMPARE(outF32[i * 2], 1.0f);     // Clamped to +1.0
        QCOMPARE(outF32[i * 2 + 1], -1.0f); // Clamped to -1.0
    }

    // Test output to Int16 with TPDF dither
    std::vector<double> quietL(frames, 0.5);
    std::vector<double> quietR(frames, -0.5);
    std::vector<int16_t> outI16(frames * 2, 0);
    pipeline.process(quietL.data(), quietR.data(), frames, outI16.data(), SampleFormat::Int16, true);

    for (size_t i = 0; i < frames; ++i) {
        // 0.5 * 32767 ≈ 16383, within dither variance (±2)
        QVERIFY(std::abs(outI16[i * 2] - 16383) <= 2);
        QVERIFY(std::abs(outI16[i * 2 + 1] - (-16384)) <= 2);
    }
}

void TestAudioPipeline::testOutputPathRoutingAndZeroLeakage()
{
    // Section 5.10 & 5.11: Listening Correction must never leak into Render Monitor Path (0.000000)
    AudioPipeline pipeline(48000.0);
    const size_t frames = 128;

    // Create a Listening Correction chain with massive boost (+12 dB on peak filter)
    auto listenChain = std::make_unique<AudioChain>("Test Listening", ChainPurpose::ListeningCorrectionChain);
    auto headphoneEq = std::make_shared<ParametricEqNode>("Headphone EQ", NodeAffiliation::ListeningCorrection);
    listenChain->appendNode(headphoneEq);

    pipeline.swapListeningChain(std::move(listenChain));

    ParametricEQ::BandConfig cfg;
    cfg.enabled = true;
    cfg.type = BiquadFilterType::Peak;
    cfg.frequency = 1000.0;
    cfg.gainDb = 12.0;
    cfg.Q = 2.0;
    pipeline.paramMailbox().setListeningBand(0, cfg);

    // 1. In RenderMonitorPath, Listening Correction MUST NOT affect the output at all
    pipeline.setOutputMode(OutputPathMode::RenderMonitorPath);
    pipeline.setRenderMonitorVolume(1.0);

    std::vector<double> inL(frames, 0.2);
    std::vector<double> inR(frames, 0.2);
    std::vector<double> copyL = inL;
    std::vector<double> copyR = inR;

    std::vector<float> outRenderMon(frames * 2, 0.0f);
    pipeline.process(copyL.data(), copyR.data(), frames, outRenderMon.data(), SampleFormat::Float32, false);

    // Under Render Monitor, output must be exactly identical to input (0.2) with 0.000000 leakage of the +12dB boost
    for (size_t i = 0; i < frames; ++i) {
        QCOMPARE(outRenderMon[i * 2], 0.2f);
        QCOMPARE(outRenderMon[i * 2 + 1], 0.2f);
    }

    // 2. In ListeningPath, Listening Correction MUST be applied (boosted above 0.2)
    pipeline.setOutputMode(OutputPathMode::ListeningPath);
    pipeline.setListeningVolume(1.0);

    copyL = inL;
    copyR = inR;
    std::vector<float> outListening(frames * 2, 0.0f);
    pipeline.process(copyL.data(), copyR.data(), frames, outListening.data(), SampleFormat::Float32, false);

    // Peak filter transient may take a few samples, but after filter step it must deviate from 0.2
    bool altered = false;
    for (size_t i = 0; i < frames; ++i) {
        if (std::abs(outListening[i * 2] - 0.2f) > 0.001f) {
            altered = true;
            break;
        }
    }
    QVERIFY(altered);
}

void TestAudioPipeline::testAudioGraphRebuildClonesNodesWithoutSharing()
{
    // P0-08: Verify AudioChain::clone() creates isolated deep copies of nodes
    auto original = std::make_unique<AudioChain>("OriginalMaster", ChainPurpose::MasteringChain);
    auto peqNode = std::make_shared<ParametricEqNode>("MasterPEQ");
    auto geqNode = std::make_shared<GraphicEqNode>("MasterGEQ");

    original->appendNode(peqNode);
    original->appendNode(geqNode);

    auto cloned = original->clone();

    QCOMPARE(cloned->nodeCount(), 2u);
    QVERIFY(cloned->node(0) != nullptr);
    QVERIFY(cloned->node(1) != nullptr);

    // Instances must be completely distinct pointers to avoid mutating nodes active in Audio Thread
    QVERIFY(cloned->node(0) != peqNode.get());
    QVERIFY(cloned->node(1) != geqNode.get());
    QCOMPARE(cloned->node(0)->name(), peqNode->name());
    QCOMPARE(cloned->node(1)->name(), geqNode->name());

    // AudioPipeline graph rate reconfiguration test
    AudioPipeline pipeline(48000.0);
    pipeline.swapMasterChain(std::move(original));
    pipeline.setGraphSampleRate(96000.0);
    QCOMPARE(pipeline.graphSampleRate(), 96000.0);

    // Verify audio rendering works seamlessly through cloned graph
    std::vector<double> inL(128, 0.5);
    std::vector<double> inR(128, 0.5);
    std::vector<float> out(256, 0.0f);
    pipeline.process(inL.data(), inR.data(), 128, out.data(), SampleFormat::Float32, false);
    QVERIFY(out[0] != 0.0f);
}

void TestAudioPipeline::testEpochReclamationHotSwap()
{
    // Section 9.3: Hot-swap audio chains without audio glitch or memory leaks
    AudioPipeline pipeline(48000.0);
    const size_t frames = 256;

    std::vector<double> inL(frames, 0.1);
    std::vector<double> inR(frames, 0.1);
    std::vector<float> outBuf(frames * 2, 0.0f);

    // Repeatedly hot-swap master chain during active processing loop
    for (int cycle = 0; cycle < 50; ++cycle) {
        auto newChain = std::make_unique<AudioChain>("Dynamic Chain " + std::to_string(cycle), ChainPurpose::MasteringChain);
        newChain->appendNode(std::make_shared<GraphicEqNode>());
        newChain->appendNode(std::make_shared<CompressorNode>());

        pipeline.swapMasterChain(std::move(newChain));

        // Process audio blocks while chain is active
        for (int b = 0; b < 10; ++b) {
            std::vector<double> l = inL;
            std::vector<double> r = inR;
            pipeline.process(l.data(), r.data(), frames, outBuf.data(), SampleFormat::Float32, false);
        }

        // Periodically reclaim retired memory
        pipeline.reclaimRetiredMemory();
    }

    // Drain remaining retired chains
    pipeline.reclaimRetiredMemory();
    QVERIFY(true);
}

void TestAudioPipeline::testEpochReclaimerConcurrentStress()
{
    // Section 9.3: Multi-threaded stress test of EpochReclaimer
    // Verifies concurrent reader thread and writer swap thread with zero use-after-free
    EpochReclaimer<std::string> reclaimer;
    reclaimer.swapChain(std::make_unique<std::string>("Initial Value"));

    std::atomic<bool> stopReader{false};
    std::atomic<uint64_t> readCycles{0};

    // Reader thread (simulating Audio Thread)
    std::thread readerThread([&]() {
        while (!stopReader.load(std::memory_order_relaxed)) {
            auto guard = reclaimer.enterRead();
            if (guard.chain) {
                // Access content
                volatile size_t sz = guard.chain->size();
                (void)sz;
            }
            readCycles.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    // Writer thread (Control Thread) performs rapid swaps
    const int swaps = 2000;
    for (int i = 0; i < swaps; ++i) {
        reclaimer.swapChain(std::make_unique<std::string>("Chain Version " + std::to_string(i)));
        if (i % 50 == 0) {
            reclaimer.reclaimRetired();
        }
    }

    // Ensure reader thread has actively scheduled and read at least 100 times with bounded timeout (B-1, B-2)
    const auto timeoutDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool readTargetReached = false;
    while (std::chrono::steady_clock::now() < timeoutDeadline) {
        if (readCycles.load(std::memory_order_relaxed) >= 100) {
            readTargetReached = true;
            break;
        }
        std::this_thread::yield();
    }

    // Guaranteed thread termination and reclamation even on timeout (B-3)
    stopReader.store(true, std::memory_order_relaxed);
    if (readerThread.joinable()) {
        readerThread.join();
    }

    // After reader exits, reclaim all
    reclaimer.reclaimRetired();
    QCOMPARE(reclaimer.retiredCount(), 0u);
    QVERIFY2(readTargetReached, "Timed out waiting for reader cycles to reach 100 in testEpochReclaimerConcurrentStress");
    QVERIFY(readCycles.load() >= 100);
}

void TestAudioPipeline::testEpochReclaimerMultiReaderConcurrentStress()
{
    // P0-13: Multi-reader concurrent access with simultaneous writer and reclaimer
    EpochReclaimer<std::string> reclaimer;
    reclaimer.swapChain(std::make_unique<std::string>("Initial Value"));

    const size_t numReaders = 8;
    std::atomic<bool> stop{false};
    std::vector<std::thread> readers;
    std::array<std::atomic<uint64_t>, 8> readerCounts{};
    for (auto& c : readerCounts) c.store(0);

    for (size_t t = 0; t < numReaders; ++t) {
        readers.emplace_back([&reclaimer, &stop, &readerCounts, t]() {
            while (!stop.load(std::memory_order_relaxed)) {
                auto guard = reclaimer.enterRead();
                if (guard) {
                    volatile size_t sz = guard->size();
                    (void)sz;
                }
                readerCounts[t].fetch_add(1, std::memory_order_relaxed);
                std::this_thread::yield();
            }
        });
    }

    // Reclaimer thread calling reclaimRetired concurrently
    std::thread reclaimerThread([&reclaimer, &stop]() {
        while (!stop.load(std::memory_order_relaxed)) {
            reclaimer.reclaimRetired();
            std::this_thread::yield();
        }
    });

    // Writer thread rapidly swapping chains
    const int swaps = 1000;
    for (int i = 0; i < swaps; ++i) {
        reclaimer.swapChain(std::make_unique<std::string>("Chain Swap " + std::to_string(i)));
    }

    // Ensure all readers have actively scheduled and read at least 20 times with bounded timeout (B-1, B-2)
    const auto timeoutDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool allReadersReachedTarget = true;
    for (size_t t = 0; t < numReaders; ++t) {
        while (readerCounts[t].load(std::memory_order_relaxed) < 20) {
            if (std::chrono::steady_clock::now() >= timeoutDeadline) {
                allReadersReachedTarget = false;
                break;
            }
            std::this_thread::yield();
        }
        if (!allReadersReachedTarget) {
            break;
        }
    }

    // Guaranteed termination and reclamation of all readers and reclaimer thread (B-3)
    stop.store(true, std::memory_order_relaxed);
    for (auto& r : readers) {
        if (r.joinable()) {
            r.join();
        }
    }
    if (reclaimerThread.joinable()) {
        reclaimerThread.join();
    }

    // After all readers and writers are stopped, all retired objects must be cleanly reclaimed
    reclaimer.reclaimRetired();
    QCOMPARE(reclaimer.retiredCount(), 0u);
    QVERIFY2(allReadersReachedTarget, "Timed out waiting for readerCounts to reach 20 in testEpochReclaimerMultiReaderConcurrentStress");
    for (size_t t = 0; t < numReaders; ++t) {
        QVERIFY(readerCounts[t].load() >= 20);
    }
}

void TestAudioPipeline::testSampleRateChangeConcurrentWithTelemetry()
{
    // P0-13: Concurrent sample rate changes while audio thread renders and telemetry threads poll
    AudioPipeline pipeline(48000.0);
    auto masterChain = std::make_unique<AudioChain>("Master", ChainPurpose::MasteringChain);
    masterChain->appendNode(std::make_shared<CompressorNode>());
    masterChain->appendNode(std::make_shared<PeakLimiterNode>());
    pipeline.swapMasterChain(std::move(masterChain));

    std::atomic<bool> stop{false};
    const size_t frames = 128;
    std::vector<double> inL(frames, 0.2);
    std::vector<double> inR(frames, 0.2);
    std::vector<float> outBuf(frames * 2, 0.0f);

    // Audio rendering thread
    std::thread audioThread([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            std::vector<double> l = inL;
            std::vector<double> r = inR;
            pipeline.process(l.data(), r.data(), frames, outBuf.data(), SampleFormat::Float32, false);
            std::this_thread::yield();
        }
    });

    // Telemetry polling thread (simulating UI thread)
    std::thread telemetryThread([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            double cGr = pipeline.compressorGainReductionDb();
            double lGr = pipeline.limiterGainReductionDb();
            int lat = pipeline.limiterLatencyFrames();
            (void)cGr; (void)lGr; (void)lat;
            std::this_thread::yield();
        }
    });

    // Control thread rapidly reconfiguring graph sample rate
    const double rates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 };
    for (int i = 0; i < 200; ++i) {
        pipeline.setGraphSampleRate(rates[i % 5]);
        if (i % 20 == 0) {
            pipeline.reclaimRetiredMemory();
        }
    }

    stop.store(true, std::memory_order_relaxed);
    audioThread.join();
    telemetryThread.join();

    pipeline.reclaimRetiredMemory();
    QVERIFY(true);
}

void TestAudioPipeline::testShutdownWaitsForActiveReaders()
{
    // P0-13, P0-20: drainAll() must wait until an active reader completes reading before destroying
    // Strictly ZERO sleep_for() reliance: Deterministic synchronization via std::latch
    auto reclaimer = std::make_unique<EpochReclaimer<std::string>>();
    reclaimer->swapChain(std::make_unique<std::string>("ProtectedString"));

    std::latch readerInsideLatch(1);
    std::latch drainStartedLatch(1);
    std::atomic<bool> releaseReader{false};
    std::atomic<bool> readerDone{false};
    std::atomic<bool> drained{false};

    std::thread readerThread([&]() {
        auto guard = reclaimer->enterRead();
        QVERIFY(guard.chain != nullptr);
        QCOMPARE(*guard.chain, std::string("ProtectedString"));

        // Signal that reader has acquired guard and is inside the protected epoch
        readerInsideLatch.count_down();

        // Wait until drainThread has entered drainAll() and is actively waiting for this reader (bounded timeout B-1, B-2)
        drainStartedLatch.wait();
        const auto readerTimeout = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!releaseReader.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= readerTimeout) {
                break;
            }
            std::this_thread::yield();
        }

        // Reader completes reading; guard will be destroyed upon function return
        readerDone.store(true, std::memory_order_release);
    });

    // 1. Wait until readerThread has entered the epoch read guard
    readerInsideLatch.wait();

    std::thread drainThread([&]() {
        // Signal that drainThread is starting drainAll()
        drainStartedLatch.count_down();
        reclaimer->drainAll();
        drained.store(true, std::memory_order_release);
    });

    // 2. Wait until drainThread has called count_down() and entered drainAll()
    drainStartedLatch.wait();

    // 3. Verify that drainThread is strictly BLOCKED waiting for the active reader
    for (int i = 0; i < 50; ++i) {
        QVERIFY(!drained.load(std::memory_order_acquire));
        std::this_thread::yield();
    }

    // 4. Now release the readerThread to exit its read guard
    releaseReader.store(true, std::memory_order_release);

    drainThread.join();
    readerThread.join();

    QVERIFY(readerDone.load());
    QVERIFY(drained.load());
}

void TestAudioPipeline::testReaderSlotExhaustionRejectsUnshieldedPointer()
{
    // P0-20: If all 64 reader slots are saturated, enterRead() strictly returns an invalid ReadGuard
    // with nullptr. IT MUST NEVER RETURN AN UNSHIELDED POINTER.
    EpochReclaimer<std::string> reclaimer;
    reclaimer.swapChain(std::make_unique<std::string>("TargetData"));

    std::vector<EpochReclaimer<std::string>::ReadGuard> guards;
    guards.reserve(EpochReclaimer<std::string>::MAX_READERS);

    // 1. Acquire all MAX_READERS (64) slots
    for (size_t i = 0; i < EpochReclaimer<std::string>::MAX_READERS; ++i) {
        auto guard = reclaimer.enterRead();
        QVERIFY(guard.isValid());
        QVERIFY(guard.get() != nullptr);
        QCOMPARE(*guard, std::string("TargetData"));
        guards.push_back(std::move(guard));
    }
    QCOMPARE(reclaimer.slotExhaustionCount(), 0u);

    // 2. 65th attempt MUST fail cleanly without exposing an unshielded pointer
    auto failedGuard = reclaimer.enterRead();
    QVERIFY(!failedGuard.isValid());
    QVERIFY(!failedGuard);
    QVERIFY(failedGuard.get() == nullptr);
    QVERIFY(failedGuard.chain == nullptr);
    QCOMPARE(reclaimer.slotExhaustionCount(), 1u);

    // 3. Release one slot: subsequent enterRead() must succeed immediately
    guards.pop_back();
    auto recoveryGuard = reclaimer.enterRead();
    QVERIFY(recoveryGuard.isValid());
    QVERIFY(recoveryGuard.get() != nullptr);
    QCOMPARE(*recoveryGuard, std::string("TargetData"));
}

void TestAudioPipeline::testDspInstanceContinuousExchangeAndRetirement()
{
    // P0-21: Continuous DSP instance generation on Control Thread and lock-free block-boundary
    // swap on Audio Thread with safe retirement outside Audio Thread.
    AudioPipeline pipeline(48000.0);

    auto master = std::make_unique<AudioChain>("Mastering", ChainPurpose::MasteringChain);
    master->appendNode(std::make_shared<CompressorNode>("Compressor"));
    master->appendNode(std::make_shared<PeakLimiterNode>("Peak Limiter"));
    master->appendNode(std::make_shared<ReverbNode>("Reverb"));
    pipeline.swapMasterChain(std::move(master));

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> audioBlocksRendered{0};

    // Audio Thread
    std::thread audioThread([&]() {
        const size_t frames = 256;
        std::vector<double> inL(frames, 0.4);
        std::vector<double> inR(frames, -0.4);
        std::vector<float> out(frames * 2, 0.0f);

        while (!stop.load(std::memory_order_relaxed)) {
            pipeline.process(inL.data(), inR.data(), frames, out.data(), SampleFormat::Float32, false);
            audioBlocksRendered.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    // Control Thread: Rapidly preparing and publishing new DSP instances
    for (int i = 0; i < 200; ++i) {
        pipeline.updateCompressor(-12.0 - (i % 10), 2.0 + (i % 4), 10.0, 100.0, 0.0, 4.0, true);
        pipeline.updatePeakLimiter(-0.1 - (i % 3) * 0.5, 50.0, 1.0 + (i % 3) * 0.5, true);
        pipeline.updateReverb(0.5 + (i % 5) * 0.08, 0.3, 0.2, 5.0, true, true);

        if (i % 10 == 0) {
            pipeline.reclaimRetiredMemory();
        }
        std::this_thread::yield();
    }

    stop.store(true, std::memory_order_relaxed);
    audioThread.join();

    pipeline.reclaimRetiredMemory();
    QVERIFY(audioBlocksRendered.load() > 0);
}

void TestAudioPipeline::testReverbTailPreservationCrossfade()
{
    // P0-22: Reverb state transition with 512-frame tail preservation crossfade
    AudioPipeline pipeline(48000.0);

    auto master = std::make_unique<AudioChain>("Mastering", ChainPurpose::MasteringChain);
    auto reverbNode = std::make_shared<ReverbNode>("Reverb");
    master->appendNode(reverbNode);
    pipeline.swapMasterChain(std::move(master));

    pipeline.updateReverb(0.85, 0.2, 0.5, 0.0, true, true);

    const size_t frames = 256;
    std::vector<double> inL(frames, 0.0);
    std::vector<double> inR(frames, 0.0);
    std::vector<float> out(frames * 2, 0.0f);

    // Excite reverb with 2048 frames of audio to fully populate comb filter delay lines
    for (int b = 0; b < 8; ++b) {
        for (size_t i = 0; i < frames; ++i) {
            inL[i] = 0.5 * std::sin((static_cast<double>(b) * static_cast<double>(frames) + static_cast<double>(i)) * 0.1);
            inR[i] = inL[i];
        }
        pipeline.process(inL.data(), inR.data(), frames, out.data(), SampleFormat::Float32, false);
    }

    // Now clear input to let tail ring out
    std::fill(inL.begin(), inL.end(), 0.0);
    std::fill(inR.begin(), inR.end(), 0.0);
    pipeline.process(inL.data(), inR.data(), frames, out.data(), SampleFormat::Float32, false);

    double tailEnergyBefore = 0.0;
    for (float s : out) tailEnergyBefore += s * s;
    QVERIFY(tailEnergyBefore > 1e-4);

    // Update reverb settings while tail is ringing (triggers 512-frame crossfade)
    pipeline.updateReverb(0.4, 0.6, 0.3, 10.0, true, true);

    // Process through the crossfade (first 256 frames)
    pipeline.process(inL.data(), inR.data(), frames, out.data(), SampleFormat::Float32, false);
    double tailEnergyCrossfade1 = 0.0;
    for (float s : out) tailEnergyCrossfade1 += s * s;

    // Tail energy must remain preserved and non-zero (not truncated to 0)
    QVERIFY(tailEnergyCrossfade1 > 1e-4);

    // Process next block (completing the initial transition)
    pipeline.process(inL.data(), inR.data(), frames, out.data(), SampleFormat::Float32, false);
    double tailEnergyCrossfade2 = 0.0;
    for (float s : out) tailEnergyCrossfade2 += s * s;
    QVERIFY(tailEnergyCrossfade2 > 1e-4);

    // Process blocks beyond 512 frames: ringout tail must continue naturally (P0-22)
    for (int b = 0; b < 4; ++b) {
        pipeline.process(inL.data(), inR.data(), frames, out.data(), SampleFormat::Float32, false);
        double tailBlockEnergy = 0.0;
        for (float s : out) tailBlockEnergy += s * s;
        QVERIFY(tailBlockEnergy > 1e-5);
    }

    pipeline.reclaimRetiredMemory();
}

void TestAudioPipeline::testLimiterLookaheadChangeSampleContinuity()
{
    // P0-23: Limiter lookahead reconfiguration with buffer inheritance and latency synchronization
    AudioPipeline pipeline(48000.0);

    auto master = std::make_unique<AudioChain>("Mastering", ChainPurpose::MasteringChain);
    auto limiterNode = std::make_shared<PeakLimiterNode>("Peak Limiter");
    master->appendNode(limiterNode);
    pipeline.swapMasterChain(std::move(master));

    pipeline.updatePeakLimiter(-0.5, 50.0, 1.0, true); // 1.0ms = 48 samples at 48kHz

    const size_t frames = 256;
    std::vector<double> inL(frames, 0.5);
    std::vector<double> inR(frames, 0.5);
    std::vector<float> out(frames * 2, 0.0f);

    pipeline.process(inL.data(), inR.data(), frames, out.data(), SampleFormat::Float32, false);
    QCOMPARE(pipeline.limiterLatencyFrames(), 48);

    // Dynamically increase lookahead to 2.0ms (96 samples)
    pipeline.updatePeakLimiter(-0.5, 50.0, 2.0, true);
    pipeline.process(inL.data(), inR.data(), frames, out.data(), SampleFormat::Float32, false);

    // Latency frames must reflect the updated lookahead
    QCOMPARE(pipeline.limiterLatencyFrames(), 96);

    // Output must not contain NaN, inf, or values exceeding ceiling
    for (float s : out) {
        QVERIFY(!std::isnan(s));
        QVERIFY(!std::isinf(s));
        QVERIFY(std::abs(s) <= 1.0f);
    }

    pipeline.reclaimRetiredMemory();
}

void TestAudioPipeline::testTelemetrySnapshotThreadSafety()
{
    // P0-26: Concurrent Audio Thread telemetry publishing and UI Thread polling
    AudioPipeline pipeline(48000.0);

    auto master = std::make_unique<AudioChain>("Mastering", ChainPurpose::MasteringChain);
    master->appendNode(std::make_shared<CompressorNode>("Compressor"));
    master->appendNode(std::make_shared<PeakLimiterNode>("Peak Limiter"));
    master->appendNode(std::make_shared<ReverbNode>("Reverb"));
    pipeline.swapMasterChain(std::move(master));

    pipeline.updateCompressor(-18.0, 4.0, 10.0, 100.0, 0.0, 4.0, true);
    pipeline.updatePeakLimiter(-1.0, 50.0, 1.0, true);

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> totalTelemetryReads{0};

    // Audio Thread
    std::thread audioThread([&]() {
        const size_t frames = 128;
        std::vector<double> inL(frames, 0.8); // High signal to trigger gain reduction
        std::vector<double> inR(frames, 0.8);
        std::vector<float> out(frames * 2, 0.0f);

        while (!stop.load(std::memory_order_relaxed)) {
            pipeline.process(inL.data(), inR.data(), frames, out.data(), SampleFormat::Float32, false);
            std::this_thread::yield();
        }
    });

    // 4 UI / Background Telemetry Threads
    std::vector<std::thread> uiThreads;
    for (int t = 0; t < 4; ++t) {
        uiThreads.emplace_back([&]() {
            while (!stop.load(std::memory_order_relaxed)) {
                auto tele = pipeline.currentTelemetry();
                QVERIFY(tele.graphSampleRate > 0.0);
                double compGR = pipeline.compressorGainReductionDb();
                double limGR = pipeline.limiterGainReductionDb();
                int latency = pipeline.limiterLatencyFrames();
                Q_UNUSED(compGR);
                Q_UNUSED(limGR);
                Q_UNUSED(latency);
                totalTelemetryReads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stop.store(true, std::memory_order_relaxed);

    audioThread.join();
    for (auto& t : uiThreads) {
        t.join();
    }

    QVERIFY(totalTelemetryReads.load() > 100);
}

void TestAudioPipeline::testEpochReclaimerShutdownConcurrentSafety()
{
    // P0-28: drainAll() shutdown protocol with concurrent speculative readers
    auto reclaimer = std::make_unique<EpochReclaimer<std::string>>();
    reclaimer->swapChain(std::make_unique<std::string>("InitialActiveData"));

    std::atomic<bool> stopReaders{false};
    std::atomic<uint64_t> successfulReads{0};
    std::atomic<uint64_t> rejectedReads{0};

    // 4 concurrent reader threads attempting enterRead() before, during, and after drainAll()
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&]() {
            while (!stopReaders.load(std::memory_order_relaxed)) {
                auto guard = reclaimer->enterRead();
                if (guard.isValid() && guard.chain != nullptr) {
                    successfulReads.fetch_add(1, std::memory_order_relaxed);
                    // Verify data sanity
                    volatile size_t len = guard.chain->length();
                    (void)len;
                } else {
                    rejectedReads.fetch_add(1, std::memory_order_relaxed);
                    QVERIFY(guard.chain == nullptr);
                }
                std::this_thread::yield();
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Shutdown while readers are vigorously querying
    reclaimer->drainAll();
    QVERIFY(reclaimer->isClosing());

    // Subsequent enterRead() MUST strictly reject
    auto postGuard = reclaimer->enterRead();
    QVERIFY(!postGuard.isValid());
    QVERIFY(postGuard.chain == nullptr);

    // swapChain on closed reclaimer is a safe no-op
    reclaimer->swapChain(std::make_unique<std::string>("ShouldBeIgnored"));

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    stopReaders.store(true, std::memory_order_relaxed);

    for (auto& t : readers) {
        t.join();
    }

    QVERIFY(successfulReads.load() > 0);
    QVERIFY(rejectedReads.load() > 0);
}

void TestAudioPipeline::testEpochReclaimerWriterShutdownSerialization()
{
    // P0-20: Atomic serialization of swapChain() with drainAll()
    auto reclaimer = std::make_unique<EpochReclaimer<std::string>>();
    QVERIFY(reclaimer->swapChain(std::make_unique<std::string>("InitialChain")));

    std::atomic<bool> stopWorkers{false};
    std::atomic<uint64_t> successfulSwaps{0};
    std::atomic<uint64_t> rejectedSwaps{0};
    std::atomic<uint64_t> successfulReads{0};

    // 2 Concurrent Reader threads
    std::vector<std::thread> readers;
    for (int i = 0; i < 2; ++i) {
        readers.emplace_back([&]() {
            while (!stopWorkers.load(std::memory_order_relaxed)) {
                auto guard = reclaimer->enterRead();
                if (guard.isValid() && guard.chain != nullptr) {
                    successfulReads.fetch_add(1, std::memory_order_relaxed);
                    volatile size_t len = guard.chain->length();
                    (void)len;
                }
                std::this_thread::yield();
            }
        });
    }

    // 2 Concurrent Writer threads attempting continuous swapChain
    std::vector<std::thread> writers;
    for (int i = 0; i < 2; ++i) {
        writers.emplace_back([&, i]() {
            uint64_t count = 0;
            while (!stopWorkers.load(std::memory_order_relaxed)) {
                auto chain = std::make_unique<std::string>("Chain_" + std::to_string(i) + "_" + std::to_string(count++));
                if (reclaimer->swapChain(std::move(chain))) {
                    successfulSwaps.fetch_add(1, std::memory_order_relaxed);
                } else {
                    rejectedSwaps.fetch_add(1, std::memory_order_relaxed);
                }
                std::this_thread::yield();
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Shutdown while writers and readers are actively working
    reclaimer->drainAll();
    QVERIFY(reclaimer->isClosing());

    // Subsequent swapChain MUST be rejected
    auto lateChain = std::make_unique<std::string>("LateChain");
    bool swapResult = reclaimer->swapChain(std::move(lateChain));
    QVERIFY(!swapResult);

    // All retired objects MUST be cleaned up
    QCOMPARE(reclaimer->retiredCount(), 0u);

    // Subsequent enterRead() MUST return nullptr
    auto postGuard = reclaimer->enterRead();
    QVERIFY(!postGuard.isValid());
    QVERIFY(postGuard.chain == nullptr);

    stopWorkers.store(true, std::memory_order_relaxed);
    for (auto& w : writers) w.join();
    for (auto& r : readers) r.join();

    QVERIFY(successfulSwaps.load() > 0);
    QVERIFY(rejectedSwaps.load() > 0);
    QVERIFY(successfulReads.load() > 0);
}

void TestAudioPipeline::testEpochReclaimerDeterministicShutdownRaces()
{
    // P0-20, P0-28: Deterministic verification of EpochReclaimer shutdown concurrency (Cases A - D)

    // Case A: Writer swapChain during Shutdown
    {
        EpochReclaimer<std::string> reclaimer;
        reclaimer.swapChain(std::make_unique<std::string>("Initial"));

        std::latch syncPoint(2);
        std::atomic<bool> swapOutcome{false};

        std::thread writerThread([&]() {
            syncPoint.arrive_and_wait();
            swapOutcome.store(reclaimer.swapChain(std::make_unique<std::string>("FromWriter")));
        });

        std::thread shutdownThread([&]() {
            syncPoint.arrive_and_wait();
            reclaimer.drainAll();
        });

        writerThread.join();
        shutdownThread.join();

        QVERIFY(reclaimer.isClosing());
        QCOMPARE(reclaimer.retiredCount(), 0u);
    }

    // Case B: Reader holds guard during swapChain and drainAll (drainAll deterministically blocks until Reader exits)
    {
        EpochReclaimer<std::string> reclaimer;
        reclaimer.swapChain(std::make_unique<std::string>("ReaderActiveChain"));

        std::latch readerHolding(1);
        std::atomic<bool> releaseReader{false};
        std::atomic<bool> readerFinished{false};
        std::atomic<bool> drainDone{false};

        std::thread readerThread([&]() {
            auto guard = reclaimer.enterRead();
            QVERIFY(guard.isValid() && guard.chain != nullptr);
            QCOMPARE(*guard.chain, std::string("ReaderActiveChain"));
            readerHolding.count_down();

            // Wait deterministically for Shutdown drainAll to reach the waiting loop (bounded timeout B-1, B-2)
            const auto readerTimeout = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!releaseReader.load(std::memory_order_acquire)) {
                if (std::chrono::steady_clock::now() >= readerTimeout) {
                    break;
                }
                std::this_thread::yield();
            }
            // Verify data is still intact (never prematurely freed)
            QCOMPARE(*guard.chain, std::string("ReaderActiveChain"));
            readerFinished.store(true, std::memory_order_release);
        });

        readerHolding.wait();
        // Retire the chain that reader is observing
        reclaimer.swapChain(std::make_unique<std::string>("NewerChain"));

        // Launch drainAll which MUST block waiting for readerThread to finish
        std::thread drainThread([&]() {
            reclaimer.drainAll();
            drainDone.store(true, std::memory_order_release);
        });

        // Deterministically wait for drainAll to enter its active reader wait loop (bounded timeout B-1, B-2)
        const auto waitTimeout = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!reclaimer.isWaitingForReaders()) {
            if (std::chrono::steady_clock::now() >= waitTimeout) {
                break;
            }
            std::this_thread::yield();
        }

        // While reader has not finished, drainAll must NOT have completed
        QVERIFY(!drainDone.load(std::memory_order_acquire));

        // Signal reader to finish and release its guard
        releaseReader.store(true, std::memory_order_release);

        readerThread.join();
        drainThread.join();

        QVERIFY(readerFinished.load());
        QVERIFY(drainDone.load());
        QVERIFY(reclaimer.isClosing());
        QCOMPARE(reclaimer.retiredCount(), 0u);
    }

    // Case C: Subsequent calls after drainAll completed
    {
        EpochReclaimer<std::string> reclaimer;
        reclaimer.swapChain(std::make_unique<std::string>("Initial"));
        reclaimer.drainAll();
        QVERIFY(reclaimer.isClosing());

        // Strictly reject new swaps
        bool swapOk = reclaimer.swapChain(std::make_unique<std::string>("PostClose"));
        QVERIFY(!swapOk);

        // Strictly reject new reads
        auto guard = reclaimer.enterRead();
        QVERIFY(!guard.isValid());
        QVERIFY(guard.chain == nullptr);
    }

    // Case D: Multiple writers (4 writers) at barrier calling swapChain concurrently with drainAll
    {
        EpochReclaimer<std::string> reclaimer;
        reclaimer.swapChain(std::make_unique<std::string>("InitialD"));

        const int numWriters = 4;
        std::latch syncAll(numWriters + 1);

        std::vector<std::thread> writers;
        std::atomic<int> successCount{0};
        std::atomic<int> rejectCount{0};

        for (int i = 0; i < numWriters; ++i) {
            writers.emplace_back([&, i]() {
                syncAll.arrive_and_wait();
                bool ok = reclaimer.swapChain(std::make_unique<std::string>("Chain_D_" + std::to_string(i)));
                if (ok) {
                    successCount.fetch_add(1, std::memory_order_relaxed);
                } else {
                    rejectCount.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        std::thread drainThread([&]() {
            syncAll.arrive_and_wait();
            reclaimer.drainAll();
        });

        for (auto& w : writers) w.join();
        drainThread.join();

        QVERIFY(reclaimer.isClosing());
        QCOMPARE(reclaimer.retiredCount(), 0u);
        QCOMPARE(successCount.load() + rejectCount.load(), numWriters);
    }
}

void TestAudioPipeline::testEpochReclaimerTimeoutSafetyAndThreadCleanup()
{
    // B-T03, B-T04, B-T05, B-T06: Verification of timeout safety, thread joining, and shared object lifetime
    // Simulates a reader thread that does not advance its cycle count, verifying finite timeout,
    // guaranteed thread joining, and safe memory reclamation with zero hanging or leak.
    auto reclaimer = std::make_unique<EpochReclaimer<std::string>>();
    reclaimer->swapChain(std::make_unique<std::string>("InitialProtectedString"));

    std::atomic<bool> stopReader{false};
    std::atomic<uint64_t> readCycles{0};
    std::atomic<bool> readerStarted{false};

    // Reader thread simulates a stalled condition (does not increment readCycles)
    std::thread readerThread([&]() {
        readerStarted.store(true, std::memory_order_release);
        while (!stopReader.load(std::memory_order_relaxed)) {
            // Actively observing epoch without advancing cycle count (simulating stall/block)
            auto guard = reclaimer->enterRead();
            if (guard.chain) {
                volatile size_t sz = guard.chain->size();
                (void)sz;
            }
            std::this_thread::yield();
        }
    });

    // Wait until reader thread actually starts
    const auto startTimeout = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!readerStarted.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= startTimeout) break;
        std::this_thread::yield();
    }
    QVERIFY(readerStarted.load());

    // 1. Verify finite timeout detection (B-T03)
    const auto timeoutDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    bool targetReached = false;
    while (std::chrono::steady_clock::now() < timeoutDeadline) {
        if (readCycles.load(std::memory_order_relaxed) >= 100) {
            targetReached = true;
            break;
        }
        std::this_thread::yield();
    }

    // Must have timed out because reader was stalled
    QVERIFY(!targetReached);

    // 2. Guaranteed thread stop and cleanup even when timed out (B-T04)
    stopReader.store(true, std::memory_order_relaxed);
    QVERIFY(readerThread.joinable());
    readerThread.join();

    // 3. Shared object lifetime validation (B-T05):
    // Reclaimer must still be 100% valid, intact, and safe to drain/reclaim
    reclaimer->swapChain(std::make_unique<std::string>("PostTimeoutChain"));
    reclaimer->reclaimRetired();
    QCOMPARE(reclaimer->retiredCount(), 0u);

    // Drain all and destroy safely
    reclaimer->drainAll();
    QVERIFY(reclaimer->isClosing());
    reclaimer.reset(); // Safe destruction without use-after-free
}

void TestAudioPipeline::testRealTimeSafetyInLivePipeline()
{
    // P0-23, P0-27, P0-29: Live AudioPipeline Real-Time Safety Verification
    // Verifies zero dynamic heap allocation, zero blocking/mutex, continuous mailbox draining,
    // prepared instance swapping, telemetry publishing, and measured CPU budget.

    AudioPipeline pipeline(48000.0);

    auto master = std::make_unique<AudioChain>("Mastering", ChainPurpose::MasteringChain);
    master->appendNode(std::make_shared<ParametricEqNode>("Master PEQ"));
    master->appendNode(std::make_shared<GraphicEqNode>("Master GEQ"));
    master->appendNode(std::make_shared<HighLowPassNode>("Master Filters"));
    master->appendNode(std::make_shared<CompressorNode>("Master Comp"));
    auto insertNode = std::make_shared<desk32::engine::insert::ExternalInsertNode>("Master Insert");
    desk32::engine::insert::ExternalPortInfo insertCfg{"Carla", "in_l", "in_r", "out_l", "out_r", 48000.0, 256, 64};
    insertNode->bridge().configure(insertCfg, 48000.0, 256);
    insertNode->setBypassed(false);
    master->appendNode(insertNode);
    master->appendNode(std::make_shared<PeakLimiterNode>("Master Limiter"));
    master->appendNode(std::make_shared<ReverbNode>("Master Reverb"));
    pipeline.swapMasterChain(std::move(master));

    auto listening = std::make_unique<AudioChain>("Listening", ChainPurpose::ListeningCorrectionChain);
    listening->appendNode(std::make_shared<ParametricEqNode>("Headphone EQ", NodeAffiliation::ListeningCorrection));
    pipeline.swapListeningChain(std::move(listening));

    const size_t frames = 256;
    std::vector<double> inL(frames, 0.4);
    std::vector<double> inR(frames, -0.4);
    std::vector<float> out(frames * 2, 0.0f);

    // Warm-up block
    pipeline.process(inL.data(), inR.data(), frames, out.data(), SampleFormat::Float32, false);

    std::atomic<bool> stopAudio{false};
    std::atomic<uint64_t> blocksRendered{0};
    std::atomic<int64_t> maxProcessNanos{0};

    // Begin allocation tracking strictly for the Audio Thread
    s_rtStats.reset();

    std::thread audioThread([&]() {
        while (!stopAudio.load(std::memory_order_relaxed)) {
            t_isAudioThread = true;
            auto t0 = std::chrono::steady_clock::now();
            pipeline.process(inL.data(), inR.data(), frames, out.data(), SampleFormat::Float32, false);
            auto t1 = std::chrono::steady_clock::now();
            t_isAudioThread = false;

            int64_t elapsedNanos = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
            int64_t curMax = maxProcessNanos.load(std::memory_order_relaxed);
            while (elapsedNanos > curMax && !maxProcessNanos.compare_exchange_weak(curMax, elapsedNanos, std::memory_order_relaxed));

            blocksRendered.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    // Control Thread: Concurrently mutate Mailbox, swap DSP instances, push discrete commands, swap mock processor, read telemetry, and reclaim memory
    for (int i = 0; i < 100; ++i) {
        pipeline.updateCompressor(-12.0 - (i % 8), 3.0, 10.0, 100.0, 0.0, 4.0, true);
        pipeline.updatePeakLimiter(-0.5 - (i % 4) * 0.2, 50.0, 1.0, true);
        pipeline.updateReverb(0.6 + (i % 3) * 0.1, 0.3, 0.2, 0.0, true, true);
        pipeline.paramMailbox().setListeningPreset("Monitor Flat");

        // Also test discrete command queue paths (SetCompressorParams / SetLimiterParams / SetReverbParams / SetNodeBypass)
        desk32::engine::dsp::DspCommand cmdComp;
        cmdComp.type = desk32::engine::dsp::DspCommandType::SetCompressorParams;
        cmdComp.boolFlag1 = 1;
        pipeline.paramQueue().push(cmdComp);

        desk32::engine::dsp::DspCommand cmdLim;
        cmdLim.type = desk32::engine::dsp::DspCommandType::SetLimiterParams;
        cmdLim.boolFlag1 = 1;
        pipeline.paramQueue().push(cmdLim);

        desk32::engine::dsp::DspCommand cmdRev;
        cmdRev.type = desk32::engine::dsp::DspCommandType::SetReverbParams;
        cmdRev.boolFlag1 = 1;
        pipeline.paramQueue().push(cmdRev);

        insertNode->bridge().setMockProcessor([gain = 0.9 + (i % 2) * 0.05](double* l, double* r, size_t f) {
            for (size_t s = 0; s < f; ++s) {
                l[s] *= gain;
                r[s] *= gain;
            }
        });

        auto telem = pipeline.currentTelemetry();
        (void)telem;

        if (i % 5 == 0) {
            pipeline.reclaimRetiredMemory();
        }
        std::this_thread::yield();
    }

    // Ensure audio thread renders at least 50 blocks
    while (blocksRendered.load(std::memory_order_relaxed) < 50) {
        std::this_thread::yield();
    }

    stopAudio.store(true, std::memory_order_release);
    audioThread.join();

    pipeline.reclaimRetiredMemory();

    qDebug() << "Live AudioPipeline: Rendered" << blocksRendered.load() << "blocks (256 frames each).";
    qDebug() << "Audio Thread Allocations (new/new[]/aligned):"
             << s_rtStats.opNewCount.load() << "/"
             << s_rtStats.opNewArrayCount.load() << "/"
             << s_rtStats.opNewAlignedCount.load();
    qDebug() << "Audio Thread Deallocations (delete/delete[]):"
             << s_rtStats.opDeleteCount.load() << "/"
             << s_rtStats.opDeleteArrayCount.load();
    qDebug() << "Audio Thread Blocking Calls:" << s_rtStats.rtBlockingCount.load();
    qDebug() << "Max Block Process Time:" << (maxProcessNanos.load() / 1000) << "microseconds";

    // 1. Strictly zero heap allocations of any kind during live audio rendering
    QCOMPARE(s_rtStats.opNewCount.load(), 0u);
    QCOMPARE(s_rtStats.opNewArrayCount.load(), 0u);
    QCOMPARE(s_rtStats.opNewNothrowCount.load(), 0u);
    QCOMPARE(s_rtStats.opNewAlignedCount.load(), 0u);
    QCOMPARE(s_rtStats.opDeleteCount.load(), 0u);
    QCOMPARE(s_rtStats.opDeleteArrayCount.load(), 0u);
    QCOMPARE(s_rtStats.rtBlockingCount.load(), 0u);
    QCOMPARE(s_rtStats.totalAllocations(), 0u);

    // 2. Verified real-time progress
    QVERIFY(blocksRendered.load() >= 50);

    // 3. Max process time must stay well within the audio deadline (5.33ms for 256 frames @ 48kHz)
    // Under native Release, process() runs in ~10-50 microseconds; under Sanitizers within 4ms
#if defined(__SANITIZE_THREAD__)
    QVERIFY(maxProcessNanos.load() < 20000000); // 20ms budget under ThreadSanitizer instrumentation
#elif defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_UNDEFINED__) || defined(DESK32_SANITIZE_UNDEFINED)
    QVERIFY(maxProcessNanos.load() < 10000000); // 10ms budget under ASan/UBSan instrumentation
#elif !defined(NDEBUG) || defined(DEBUG)
    QVERIFY(maxProcessNanos.load() < 5000000); // 5ms budget under unoptimized Debug (-O0)
#else
    QVERIFY(maxProcessNanos.load() < 1000000); // Strict 1.0 ms budget in native optimized Release
#endif

    // 4. Maximum Capacity & Block Stress Test: 16 nodes, 4096 frames
    {
        AudioPipeline stressPipeline(48000.0);
        auto stressChain = std::make_unique<AudioChain>("StressMaster", ChainPurpose::MasteringChain);
        for (int n = 0; n < 16; ++n) {
            stressChain->appendNode(std::make_shared<ParametricEqNode>("StressEQ_" + std::to_string(n)));
        }
        stressPipeline.swapMasterChain(std::move(stressChain));

        const size_t maxFrames = 4096;
        std::vector<double> bigL(maxFrames, 0.3);
        std::vector<double> bigR(maxFrames, -0.3);
        std::vector<float> bigOut(maxFrames * 2, 0.0f);

        s_rtStats.reset();
        t_isAudioThread = true;

        auto t0 = std::chrono::steady_clock::now();
        stressPipeline.process(bigL.data(), bigR.data(), maxFrames, bigOut.data(), SampleFormat::Float32, false);
        auto t1 = std::chrono::steady_clock::now();

        t_isAudioThread = false;

        int64_t stressNanos = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        qDebug() << "16-Node Max-Load (4096 frames) Process Time:" << (stressNanos / 1000) << "microseconds";

        QCOMPARE(s_rtStats.totalAllocations(), 0u);
        QCOMPARE(s_rtStats.rtBlockingCount.load(), 0u);
#if !defined(__SANITIZE_THREAD__) && !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_UNDEFINED__) && !defined(DESK32_SANITIZE_UNDEFINED)
        // 4096 frames @ 48kHz audio deadline is 85.3 ms (85,333,333 ns).
#if defined(NDEBUG)
        // In optimized Release build, 16 nodes must finish well under 10 ms
        QVERIFY(stressNanos < 10000000);
#else
        // In unoptimized Debug build (-O0), must still comfortably beat the 85.3 ms audio deadline
        QVERIFY(stressNanos < 85333333);
#endif
#endif
    }
}

void TestAudioPipeline::testRealtimeSafetyNegativeDetection()
{
    // P0-23: Negative Test for RT Safety Detection.
    // Intentionally violates RT constraints under t_isAudioThread = true,
    // proving that heap allocation counters and blocking call counters are live, active, and detectable.
    s_rtStats.reset();
    QCOMPARE(s_rtStats.totalAllocations(), 0u);
    QCOMPARE(s_rtStats.rtBlockingCount.load(), 0u);

    t_isAudioThread = true;

    // 1. Intentionally trigger C++ heap scalar and array allocations (using explicit operator new to prevent compiler elision)
    void* scalar = ::operator new(64);
    asm volatile("" : "+r"(scalar) : : "memory");
    ::operator delete(scalar);

    void* arr = ::operator new[](128);
    asm volatile("" : "+r"(arr) : : "memory");
    ::operator delete[](arr);

    // 2. Intentionally trigger C ABI heap allocations (malloc, calloc, realloc, posix_memalign, aligned_alloc, free)
    void* cPtr1 = malloc(64);
    asm volatile("" : "+r"(cPtr1) : : "memory");
    free(cPtr1);

    void* cPtr2 = calloc(2, 32);
    asm volatile("" : "+r"(cPtr2) : : "memory");
    cPtr2 = realloc(cPtr2, 128);
    asm volatile("" : "+r"(cPtr2) : : "memory");
    free(cPtr2);

    void* cPtr3 = nullptr;
    posix_memalign(&cPtr3, 64, 128);
    asm volatile("" : "+r"(cPtr3) : : "memory");
    free(cPtr3);

    void* cPtr4 = aligned_alloc(64, 128);
    asm volatile("" : "+r"(cPtr4) : : "memory");
    free(cPtr4);

    // 3. Intentionally trigger POSIX mutex lock (via std::mutex)
    std::mutex mtx;
    mtx.lock();
    mtx.unlock();

    // 4. Intentionally trigger POSIX sleep and yield blocking calls
    usleep(50);
    sched_yield();

    pthread_mutex_t pmtx = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_trylock(&pmtx);
    pthread_mutex_unlock(&pmtx);

    t_isAudioThread = false;

    // Verify negative detections fired
    QVERIFY(s_rtStats.opNewCount.load() >= 1);
    QVERIFY(s_rtStats.opDeleteCount.load() >= 1);
    QVERIFY(s_rtStats.opNewArrayCount.load() >= 1);
    QVERIFY(s_rtStats.opDeleteArrayCount.load() >= 1);
#if !defined(__SANITIZE_THREAD__) && !defined(__SANITIZE_ADDRESS__)
    QVERIFY(s_rtStats.cMallocCount.load() >= 1);
    QVERIFY(s_rtStats.cCallocCount.load() >= 1);
    QVERIFY(s_rtStats.cReallocCount.load() >= 1);
    QVERIFY(s_rtStats.cMemalignCount.load() >= 2);
    QVERIFY(s_rtStats.cFreeCount.load() >= 4);
#endif
    QVERIFY(s_rtStats.rtBlockingCount.load() >= 4); // mutex lock + usleep + sched_yield + trylock

    // Clean up tracker
    s_rtStats.reset();
}

void TestAudioPipeline::testAudioPipelineWorstCaseRealtimeConditions()
{
    // P0-23, P0-29: Worst-case real-time condition stress test:
    // - High sample rate: 192,000 Hz
    // - Large block size: 4096 frames (deadline = 21.33 ms)
    // - Full 16-node audio chain populated with diverse DSP:
    //   ParametricEq, GraphicEq, HighLowPass, Compressor, Limiter, Reverb, ExternalInsertBridge
    // - Reverb with all 8 ringout slots saturated
    // - Maximum discrete command drain (64 commands drained per block from 128 queued)
    // - Concurrent Control Thread parameter updates and instance exchanges
    // - Verification: Zero heap allocations, Zero blocking calls, execution time strictly within audio deadline.

    const double sampleRate = 192000.0;
    const size_t frames = 4096;
    AudioPipeline pipeline(sampleRate);

    auto chain = std::make_unique<AudioChain>("WorstCaseMaster", ChainPurpose::MasteringChain);

    // Build 16 heterogeneous DSP nodes:
    // 4 PEQs, 2 GEQs, 2 Filter nodes, 2 Compressors, 2 Limiters, 2 Reverbs, 2 Inserts
    auto peq1 = std::make_shared<ParametricEqNode>("PEQ_1");
    auto peq2 = std::make_shared<ParametricEqNode>("PEQ_2");
    auto peq3 = std::make_shared<ParametricEqNode>("PEQ_3");
    auto peq4 = std::make_shared<ParametricEqNode>("PEQ_4");
    auto geq1 = std::make_shared<GraphicEqNode>("GEQ_1");
    auto geq2 = std::make_shared<GraphicEqNode>("GEQ_2");
    auto flt1 = std::make_shared<HighLowPassNode>("FLT_1");
    auto flt2 = std::make_shared<HighLowPassNode>("FLT_2");
    auto comp1 = std::make_shared<CompressorNode>("COMP_1");
    auto comp2 = std::make_shared<CompressorNode>("COMP_2");
    auto lim1 = std::make_shared<PeakLimiterNode>("LIM_1");
    auto lim2 = std::make_shared<PeakLimiterNode>("LIM_2");
    auto rev1 = std::make_shared<ReverbNode>("REV_1");
    auto rev2 = std::make_shared<ReverbNode>("REV_2");
    auto ins1 = std::make_shared<desk32::engine::insert::ExternalInsertNode>("INS_1");
    auto ins2 = std::make_shared<desk32::engine::insert::ExternalInsertNode>("INS_2");

    chain->appendNode(peq1);
    chain->appendNode(geq1);
    chain->appendNode(flt1);
    chain->appendNode(comp1);
    chain->appendNode(lim1);
    chain->appendNode(rev1);
    chain->appendNode(ins1);
    chain->appendNode(peq2);
    chain->appendNode(geq2);
    chain->appendNode(flt2);
    chain->appendNode(comp2);
    chain->appendNode(lim2);
    chain->appendNode(rev2);
    chain->appendNode(ins2);
    chain->appendNode(peq3);
    chain->appendNode(peq4);
    QCOMPARE(chain->nodeCount(), size_t(16));

    pipeline.swapMasterChain(std::move(chain));

    // Saturate Reverb 1 with all 8 ringout slots active
    // Pass initial excitement block so m_active has non-zero internal energy (> 1e-12),
    // then trigger 8 prepared instance exchanges with tailPreserving=true to occupy all 8 slots.
    std::vector<double> exciteL(frames, 0.5), exciteR(frames, 0.5);
    std::vector<float> pcmOut(frames * 2, 0.0f);
    pipeline.process(exciteL.data(), exciteR.data(), frames, pcmOut.data(), SampleFormat::Float32, false);

    for (int s = 0; s < 8; ++s) {
        rev1->prepareInstance(sampleRate, 0.7 + s * 0.02, 0.3, 0.3, 0.0, true, true);
        pipeline.process(exciteL.data(), exciteR.data(), frames, pcmOut.data(), SampleFormat::Float32, false);
    }
    QCOMPARE(rev1->activeRingoutSlotCount(), size_t(8));

    // Queue 128 discrete commands to ensure full 64-command drain saturation in Audio Thread
    for (int i = 0; i < 128; ++i) {
        desk32::engine::dsp::DspCommand cmd;
        cmd.type = desk32::engine::dsp::DspCommandType::SetNodeBypass;
        cmd.index = static_cast<uint8_t>(i % 16);
        cmd.subIndex = 0;
        cmd.boolFlag1 = static_cast<uint8_t>(i % 2);
        pipeline.paramQueue().push(cmd);
    }

    // Launch concurrent background Control Thread performing continuous parameter mailbox updates
    std::atomic<bool> stopProducer{false};
    std::thread controlThread([&]() {
        size_t iter = 0;
        while (!stopProducer.load(std::memory_order_relaxed)) {
            pipeline.paramMailbox().setGeqBandGain(iter % 32, static_cast<float>((static_cast<int>(iter % 10)) - 5));
            pipeline.updateCompressor(-18.0 - static_cast<double>(iter % 6), 4.0, 10.0, 100.0, 2.0, 4.0, true);
            pipeline.updatePeakLimiter(-0.5 - static_cast<double>(iter % 3) * 0.2, 50.0, 1.0, true);
            ++iter;
            std::this_thread::yield();
        }
    });

    // Audio Thread: Render under worst-case conditions
    s_rtStats.reset();
    t_isAudioThread = true;

    auto t0 = std::chrono::steady_clock::now();
    for (int b = 0; b < 10; ++b) {
        pipeline.process(exciteL.data(), exciteR.data(), frames, pcmOut.data(), SampleFormat::Float32, false);
    }
    auto t1 = std::chrono::steady_clock::now();

    t_isAudioThread = false;
    stopProducer.store(true, std::memory_order_release);
    controlThread.join();

    int64_t totalNanos = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    int64_t avgBlockNanos = totalNanos / 10;
    qDebug() << "Worst-Case 192kHz/16-Node/8-Slot Reverb (4096 frames) Avg Process Time:"
             << (avgBlockNanos / 1000) << "microseconds";

    // Strict Real-Time Verification
    QCOMPARE(s_rtStats.totalAllocations(), 0u);
    QCOMPARE(s_rtStats.rtBlockingCount.load(), 0u);

    // Audio deadline for 4096 frames @ 192kHz is (4096 / 192000) = 21,333 microseconds (21.33 ms)
    // Under native Release, should finish well within 15 ms
#if !defined(__SANITIZE_THREAD__) && !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_UNDEFINED__) && !defined(DESK32_SANITIZE_UNDEFINED)
    QVERIFY(avgBlockNanos < 15000000); // 15 ms max (deadline = 21.33 ms)
#endif

    pipeline.reclaimRetiredMemory();
}

void TestAudioPipeline::testAudioChainMaxNodesAndBoundedCommandDrain()
{
    // P0-29: AudioChain MAX_NODES = 16 boundary, non-destructive rejection, removal/re-addition,
    // and AudioPipeline MAX_COMMANDS_PER_BLOCK = 64 bounded command drain verification.

    // 1. AudioChain MAX_NODES (16) boundary test
    AudioChain chain("BoundaryChain", ChainPurpose::MasteringChain);
    QCOMPARE(chain.nodeCount(), size_t(0));
    QCOMPARE(chain.rejectedNodeCount(), uint64_t(0));

    // Add 15 nodes
    for (size_t i = 0; i < AudioChain::MAX_NODES - 1; ++i) {
        QVERIFY(chain.appendNode(std::make_shared<ParametricEqNode>("Node_" + std::to_string(i))));
    }
    QCOMPARE(chain.nodeCount(), size_t(15));
    QCOMPARE(chain.rejectedNodeCount(), uint64_t(0));

    // Add 16th node (exact capacity limit)
    QVERIFY(chain.appendNode(std::make_shared<GraphicEqNode>("Node_15")));
    QCOMPARE(chain.nodeCount(), AudioChain::MAX_NODES);
    QCOMPARE(chain.rejectedNodeCount(), uint64_t(0));

    // Attempt to add 17th node -> must fail cleanly without mutating existing chain
    QVERIFY(!chain.appendNode(std::make_shared<HighLowPassNode>("Node_16_Overflow")));
    QCOMPARE(chain.nodeCount(), AudioChain::MAX_NODES);
    QCOMPARE(chain.rejectedNodeCount(), uint64_t(1));
    QCOMPARE(chain.node(15)->name(), std::string("Node_15"));

    // Verify processing works cleanly at full 16-node capacity
    chain.configure(48000.0);
    std::vector<double> left(256, 0.25), right(256, -0.25);
    chain.process(left.data(), right.data(), 256);
    QCOMPARE(left[0], 0.25);
    QCOMPARE(right[0], -0.25);

    // Remove one node -> count drops to 15 -> re-adding a 16th node must succeed
    QVERIFY(!chain.removeNode(16)); // Out-of-bounds remove returns false
    QVERIFY(chain.removeNode(15));
    QCOMPARE(chain.nodeCount(), size_t(15));
    QVERIFY(chain.appendNode(std::make_shared<HighLowPassNode>("Node_15_Readded")));
    QCOMPARE(chain.nodeCount(), AudioChain::MAX_NODES);
    QCOMPARE(chain.node(15)->name(), std::string("Node_15_Readded"));

    // Clear all nodes -> count drops to 0 -> re-adding succeeds
    chain.clearNodes();
    QCOMPARE(chain.nodeCount(), size_t(0));
    QVERIFY(chain.appendNode(std::make_shared<CompressorNode>("Node_0_AfterClear")));
    QCOMPARE(chain.nodeCount(), size_t(1));

    // 2. AudioPipeline MAX_COMMANDS_PER_BLOCK (64) bounded drain per block
    AudioPipeline pipeline(48000.0);
    auto master = std::make_unique<AudioChain>("Master", ChainPurpose::MasteringChain);
    master->appendNode(std::make_shared<ParametricEqNode>("PEQ"));
    pipeline.swapMasterChain(std::move(master));

    // Push 150 discrete commands into parameter queue
    for (int i = 0; i < 150; ++i) {
        desk32::engine::dsp::DspCommand cmd;
        cmd.type = desk32::engine::dsp::DspCommandType::SetNodeBypass;
        cmd.index = 0;
        cmd.subIndex = 0;
        cmd.boolFlag1 = static_cast<uint8_t>(i % 2);
        QVERIFY(pipeline.paramQueue().push(cmd));
    }
    QCOMPARE(pipeline.paramQueue().size(), size_t(150));

    std::vector<float> out(256 * 2, 0.0f);
    s_rtStats.reset();
    t_isAudioThread = true;

    // Block 1: Drains at most MAX_COMMANDS_PER_BLOCK (64) commands -> 150 - 64 = 86 remaining
    pipeline.process(left.data(), right.data(), 256, out.data(), SampleFormat::Float32, false);
    QCOMPARE(pipeline.paramQueue().size(), size_t(150 - AudioPipeline::MAX_COMMANDS_PER_BLOCK));

    // Block 2: Drains next 64 commands -> 86 - 64 = 22 remaining
    pipeline.process(left.data(), right.data(), 256, out.data(), SampleFormat::Float32, false);
    QCOMPARE(pipeline.paramQueue().size(), size_t(150 - 2 * AudioPipeline::MAX_COMMANDS_PER_BLOCK));

    // Block 3: Drains remaining 22 commands -> 0 remaining
    pipeline.process(left.data(), right.data(), 256, out.data(), SampleFormat::Float32, false);
    t_isAudioThread = false;

    QVERIFY(pipeline.paramQueue().empty());
    QCOMPARE(s_rtStats.totalAllocations(), 0u);
    QCOMPARE(s_rtStats.opDeleteCount.load() + s_rtStats.opDeleteArrayCount.load(), 0u);
}

void TestAudioPipeline::testAudioThreadRealtimeLogQueueAndLatencyDistribution()
{
    // P0-23 Phase D: Audio Thread Safety & Latency Profiling
    // D-3: RT Log Queue Verification (BoundedLogQueue try_push non-blocking, drop on overflow, 0 allocation)
    // D-4: Audio Thread Latency Profiling across >= 100,000 blocks at 48kHz and 192kHz

    // =========================================================================
    // 1. D-3: RT Log Queue Safety & Non-blocking Guarantee
    // =========================================================================
    {
        desk32::core::log::BoundedLogQueue<int, 4> queue;
        QCOMPARE(queue.size(), 0u);
        QCOMPARE(queue.overflowCount(), 0u);

        // Fill queue to capacity (4 items)
        QVERIFY(queue.try_push(101));
        QVERIFY(queue.try_push(102));
        QVERIFY(queue.try_push(103));
        QVERIFY(queue.try_push(104));
        QCOMPARE(queue.size(), 4u);
        QCOMPARE(queue.overflowCount(), 0u);

        // Attempt to push 5th item when full -> MUST fail immediately without blocking or throwing
        s_rtStats.reset();
        t_isAudioThread = true;
        bool pushOk = queue.try_push(105);
        t_isAudioThread = false;

        QVERIFY(!pushOk);
        QCOMPARE(queue.size(), 4u);
        QCOMPARE(queue.overflowCount(), 1u);
        QCOMPARE(s_rtStats.totalAllocations(), 0u);
        QCOMPARE(s_rtStats.rtBlockingCount.load(), 0u);

        // Drain queue
        auto drained = queue.drain();
        QCOMPARE(drained.size(), 4u);
        QCOMPARE(queue.size(), 0u);
        QCOMPARE(queue.resetOverflowCount(), 1u);
        QCOMPARE(queue.overflowCount(), 0u);
    }

    // =========================================================================
    // 2. D-4: 100,000-Block Latency Distribution at 48 kHz (Deadline = 5.333 ms)
    // =========================================================================
    {
        const double rate48 = 48000.0;
        const size_t frames = 256;
        const int64_t deadlineNanos48 = 5333333; // 256 / 48000 = 5.333 ms
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_UNDEFINED__) || defined(DESK32_SANITIZE_UNDEFINED)
        const size_t numBlocks = 5000;
#else
        const size_t numBlocks = 100000;
#endif

        AudioPipeline pipeline48(rate48);
        auto chain = std::make_unique<AudioChain>("Master48", ChainPurpose::MasteringChain);

        auto peq = std::make_shared<ParametricEqNode>("PEQ");
        auto comp = std::make_shared<CompressorNode>("COMP");
        auto lim = std::make_shared<PeakLimiterNode>("LIM");
        auto rev = std::make_shared<ReverbNode>("REV");

        chain->appendNode(peq);
        chain->appendNode(comp);
        chain->appendNode(lim);
        chain->appendNode(rev);
        pipeline48.swapMasterChain(std::move(chain));

        std::vector<double> inL(frames, 0.35), inR(frames, -0.35);
        std::vector<float> pcmOut(frames * 2, 0.0f);
        std::vector<int64_t> latencies(numBlocks);

        // Warm up 1,000 blocks
        for (size_t w = 0; w < 1000; ++w) {
            pipeline48.process(inL.data(), inR.data(), frames, pcmOut.data(), SampleFormat::Float32, false);
        }

        s_rtStats.reset();
        t_isAudioThread = true;

        for (size_t i = 0; i < numBlocks; ++i) {
            auto t0 = std::chrono::steady_clock::now();
            pipeline48.process(inL.data(), inR.data(), frames, pcmOut.data(), SampleFormat::Float32, false);
            auto t1 = std::chrono::steady_clock::now();
            latencies[i] = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        }

        t_isAudioThread = false;

        // Zero heap allocations & zero blocking calls across all 100,000 blocks
        QCOMPARE(s_rtStats.totalAllocations(), 0u);
        QCOMPARE(s_rtStats.totalDeallocations(), 0u);
        QCOMPARE(s_rtStats.rtBlockingCount.load(), 0u);

        // Sort latencies to compute percentiles
        std::sort(latencies.begin(), latencies.end());

        int64_t minNs = latencies.front();
        int64_t maxNs = latencies.back();
        int64_t sumNs = std::accumulate(latencies.begin(), latencies.end(), 0LL);
        double meanNs = static_cast<double>(sumNs) / static_cast<double>(numBlocks);
        int64_t p50Ns = latencies[numBlocks * 50 / 100];
        int64_t p90Ns = latencies[numBlocks * 90 / 100];
        int64_t p99Ns = latencies[numBlocks * 99 / 100];
        int64_t p999Ns = latencies[numBlocks * 999 / 1000];
        int64_t marginP999Ns = deadlineNanos48 - p999Ns;
        int64_t marginMaxNs = deadlineNanos48 - maxNs;

        // Histogram buckets
        size_t hUnder20 = 0, h20to50 = 0, h50to100 = 0, h100to250 = 0, h250to500 = 0, hOver500 = 0;
        for (int64_t ns : latencies) {
            double us = static_cast<double>(ns) / 1000.0;
            if (us < 20.0) ++hUnder20;
            else if (us < 50.0) ++h20to50;
            else if (us < 100.0) ++h50to100;
            else if (us < 250.0) ++h100to250;
            else if (us < 500.0) ++h250to500;
            else ++hOver500;
        }

        qDebug().noquote() << "\n========================================================";
        qDebug().noquote() << "AUDIO THREAD LATENCY PROFILE: 48,000 Hz (256 frames)";
        qDebug().noquote() << "========================================================";
        qDebug().noquote() << QString("Sample Count   : %1 blocks").arg(static_cast<qlonglong>(numBlocks));
        qDebug().noquote() << QString("Audio Deadline : %1 µs (%2 ms)").arg(deadlineNanos48 / 1000).arg(static_cast<double>(deadlineNanos48) / 1000000.0, 0, 'f', 3);
        qDebug().noquote() << QString("Min Latency    : %1 µs").arg(static_cast<double>(minNs) / 1000.0, 0, 'f', 2);
        qDebug().noquote() << QString("Mean Latency   : %1 µs").arg(meanNs / 1000.0, 0, 'f', 2);
        qDebug().noquote() << QString("P50 (Median)   : %1 µs").arg(static_cast<double>(p50Ns) / 1000.0, 0, 'f', 2);
        qDebug().noquote() << QString("P90            : %1 µs").arg(static_cast<double>(p90Ns) / 1000.0, 0, 'f', 2);
        qDebug().noquote() << QString("P99            : %1 µs").arg(static_cast<double>(p99Ns) / 1000.0, 0, 'f', 2);
        qDebug().noquote() << QString("P99.9          : %1 µs").arg(static_cast<double>(p999Ns) / 1000.0, 0, 'f', 2);
        qDebug().noquote() << QString("Max Latency    : %1 µs").arg(static_cast<double>(maxNs) / 1000.0, 0, 'f', 2);
        qDebug().noquote() << QString("P99.9 Margin   : %1 µs (%2% of deadline remaining)")
                                  .arg(static_cast<double>(marginP999Ns) / 1000.0, 0, 'f', 2)
                                  .arg((static_cast<double>(marginP999Ns) / static_cast<double>(deadlineNanos48)) * 100.0, 0, 'f', 1);
        qDebug().noquote() << QString("Max Margin     : %1 µs (%2% of deadline remaining)")
                                  .arg(static_cast<double>(marginMaxNs) / 1000.0, 0, 'f', 2)
                                  .arg((static_cast<double>(marginMaxNs) / static_cast<double>(deadlineNanos48)) * 100.0, 0, 'f', 1);
        qDebug().noquote() << "Latency Distribution Histogram:";
        qDebug().noquote() << QString("  < 20 µs      : %1 (%2%)").arg(static_cast<qlonglong>(hUnder20)).arg(static_cast<double>(hUnder20) * 100.0 / static_cast<double>(numBlocks), 0, 'f', 2);
        qDebug().noquote() << QString("  20 - 50 µs   : %1 (%2%)").arg(static_cast<qlonglong>(h20to50)).arg(static_cast<double>(h20to50) * 100.0 / static_cast<double>(numBlocks), 0, 'f', 2);
        qDebug().noquote() << QString("  50 - 100 µs  : %1 (%2%)").arg(static_cast<qlonglong>(h50to100)).arg(static_cast<double>(h50to100) * 100.0 / static_cast<double>(numBlocks), 0, 'f', 2);
        qDebug().noquote() << QString("  100 - 250 µs : %1 (%2%)").arg(static_cast<qlonglong>(h100to250)).arg(static_cast<double>(h100to250) * 100.0 / static_cast<double>(numBlocks), 0, 'f', 2);
        qDebug().noquote() << QString("  250 - 500 µs : %1 (%2%)").arg(static_cast<qlonglong>(h250to500)).arg(static_cast<double>(h250to500) * 100.0 / static_cast<double>(numBlocks), 0, 'f', 2);
        qDebug().noquote() << QString("  > 500 µs     : %1 (%2%)").arg(static_cast<qlonglong>(hOver500)).arg(static_cast<double>(hOver500) * 100.0 / static_cast<double>(numBlocks), 0, 'f', 2);
        qDebug().noquote() << "========================================================\n";

        QVERIFY(marginP999Ns > 0);
        QVERIFY(p999Ns < deadlineNanos48);
    }

    // =========================================================================
    // 3. D-4: 100,000-Block Latency Distribution at 192 kHz (Deadline = 1.333 ms)
    // =========================================================================
    {
        const double rate192 = 192000.0;
        const size_t frames = 256;
        const int64_t deadlineNanos192 = 1333333; // 256 / 192000 = 1.333 ms
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_UNDEFINED__) || defined(DESK32_SANITIZE_UNDEFINED)
        const size_t numBlocks = 5000;
#else
        const size_t numBlocks = 100000;
#endif

        AudioPipeline pipeline192(rate192);
        auto chain = std::make_unique<AudioChain>("Master192", ChainPurpose::MasteringChain);

        auto peq = std::make_shared<ParametricEqNode>("PEQ");
        auto comp = std::make_shared<CompressorNode>("COMP");
        auto lim = std::make_shared<PeakLimiterNode>("LIM");
        auto rev = std::make_shared<ReverbNode>("REV");

        chain->appendNode(peq);
        chain->appendNode(comp);
        chain->appendNode(lim);
        chain->appendNode(rev);
        pipeline192.swapMasterChain(std::move(chain));

        std::vector<double> inL(frames, 0.35), inR(frames, -0.35);
        std::vector<float> pcmOut(frames * 2, 0.0f);
        std::vector<int64_t> latencies(numBlocks);

        // Warm up 1,000 blocks
        for (size_t w = 0; w < 1000; ++w) {
            pipeline192.process(inL.data(), inR.data(), frames, pcmOut.data(), SampleFormat::Float32, false);
        }

        s_rtStats.reset();
        t_isAudioThread = true;

        for (size_t i = 0; i < numBlocks; ++i) {
            auto t0 = std::chrono::steady_clock::now();
            pipeline192.process(inL.data(), inR.data(), frames, pcmOut.data(), SampleFormat::Float32, false);
            auto t1 = std::chrono::steady_clock::now();
            latencies[i] = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        }

        t_isAudioThread = false;

        // Zero heap allocations & zero blocking calls across all 100,000 blocks
        QCOMPARE(s_rtStats.totalAllocations(), 0u);
        QCOMPARE(s_rtStats.totalDeallocations(), 0u);
        QCOMPARE(s_rtStats.rtBlockingCount.load(), 0u);

        // Sort latencies to compute percentiles
        std::sort(latencies.begin(), latencies.end());

        int64_t minNs = latencies.front();
        int64_t maxNs = latencies.back();
        int64_t sumNs = std::accumulate(latencies.begin(), latencies.end(), 0LL);
        double meanNs = static_cast<double>(sumNs) / static_cast<double>(numBlocks);
        int64_t p50Ns = latencies[numBlocks * 50 / 100];
        int64_t p90Ns = latencies[numBlocks * 90 / 100];
        int64_t p99Ns = latencies[numBlocks * 99 / 100];
        int64_t p999Ns = latencies[numBlocks * 999 / 1000];
        int64_t marginP999Ns = deadlineNanos192 - p999Ns;
        int64_t marginMaxNs = deadlineNanos192 - maxNs;

        // Histogram buckets
        size_t hUnder20 = 0, h20to50 = 0, h50to100 = 0, h100to250 = 0, h250to500 = 0, hOver500 = 0;
        for (int64_t ns : latencies) {
            double us = static_cast<double>(ns) / 1000.0;
            if (us < 20.0) ++hUnder20;
            else if (us < 50.0) ++h20to50;
            else if (us < 100.0) ++h50to100;
            else if (us < 250.0) ++h100to250;
            else if (us < 500.0) ++h250to500;
            else ++hOver500;
        }

        qDebug().noquote() << "\n========================================================";
        qDebug().noquote() << "AUDIO THREAD LATENCY PROFILE: 192,000 Hz (256 frames)";
        qDebug().noquote() << "========================================================";
        qDebug().noquote() << QString("Sample Count   : %1 blocks").arg(static_cast<qlonglong>(numBlocks));
        qDebug().noquote() << QString("Audio Deadline : %1 µs (%2 ms)").arg(deadlineNanos192 / 1000).arg(static_cast<double>(deadlineNanos192) / 1000000.0, 0, 'f', 3);
        qDebug().noquote() << QString("Min Latency    : %1 µs").arg(static_cast<double>(minNs) / 1000.0, 0, 'f', 2);
        qDebug().noquote() << QString("Mean Latency   : %1 µs").arg(meanNs / 1000.0, 0, 'f', 2);
        qDebug().noquote() << QString("P50 (Median)   : %1 µs").arg(static_cast<double>(p50Ns) / 1000.0, 0, 'f', 2);
        qDebug().noquote() << QString("P90            : %1 µs").arg(static_cast<double>(p90Ns) / 1000.0, 0, 'f', 2);
        qDebug().noquote() << QString("P99            : %1 µs").arg(static_cast<double>(p99Ns) / 1000.0, 0, 'f', 2);
        qDebug().noquote() << QString("P99.9          : %1 µs").arg(static_cast<double>(p999Ns) / 1000.0, 0, 'f', 2);
        qDebug().noquote() << QString("Max Latency    : %1 µs").arg(static_cast<double>(maxNs) / 1000.0, 0, 'f', 2);
        qDebug().noquote() << QString("P99.9 Margin   : %1 µs (%2% of deadline remaining)")
                                  .arg(static_cast<double>(marginP999Ns) / 1000.0, 0, 'f', 2)
                                  .arg((static_cast<double>(marginP999Ns) / static_cast<double>(deadlineNanos192)) * 100.0, 0, 'f', 1);
        qDebug().noquote() << QString("Max Margin     : %1 µs (%2% of deadline remaining)")
                                  .arg(static_cast<double>(marginMaxNs) / 1000.0, 0, 'f', 2)
                                  .arg((static_cast<double>(marginMaxNs) / static_cast<double>(deadlineNanos192)) * 100.0, 0, 'f', 1);
        qDebug().noquote() << "Latency Distribution Histogram:";
        qDebug().noquote() << QString("  < 20 µs      : %1 (%2%)").arg(static_cast<qlonglong>(hUnder20)).arg(static_cast<double>(hUnder20) * 100.0 / static_cast<double>(numBlocks), 0, 'f', 2);
        qDebug().noquote() << QString("  20 - 50 µs   : %1 (%2%)").arg(static_cast<qlonglong>(h20to50)).arg(static_cast<double>(h20to50) * 100.0 / static_cast<double>(numBlocks), 0, 'f', 2);
        qDebug().noquote() << QString("  50 - 100 µs  : %1 (%2%)").arg(static_cast<qlonglong>(h50to100)).arg(static_cast<double>(h50to100) * 100.0 / static_cast<double>(numBlocks), 0, 'f', 2);
        qDebug().noquote() << QString("  100 - 250 µs : %1 (%2%)").arg(static_cast<qlonglong>(h100to250)).arg(static_cast<double>(h100to250) * 100.0 / static_cast<double>(numBlocks), 0, 'f', 2);
        qDebug().noquote() << QString("  250 - 500 µs : %1 (%2%)").arg(static_cast<qlonglong>(h250to500)).arg(static_cast<double>(h250to500) * 100.0 / static_cast<double>(numBlocks), 0, 'f', 2);
        qDebug().noquote() << QString("  > 500 µs     : %1 (%2%)").arg(static_cast<qlonglong>(hOver500)).arg(static_cast<double>(hOver500) * 100.0 / static_cast<double>(numBlocks), 0, 'f', 2);
        qDebug().noquote() << "========================================================\n";

#if !defined(__SANITIZE_THREAD__) && !defined(__SANITIZE_ADDRESS__)
        QVERIFY(marginP999Ns > 0);
        QVERIFY(p999Ns < deadlineNanos192);
#endif
    }
}

void TestAudioPipeline::cleanupTestCase()
{
    qDebug() << "TestAudioPipeline completed successfully.";
}

QTEST_MAIN(TestAudioPipeline)
#include "test_audio_pipeline.moc"
