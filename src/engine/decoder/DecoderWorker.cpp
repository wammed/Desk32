#include "DecoderWorker.hpp"
#include <algorithm>
#include <chrono>

namespace desk32::engine::decoder {

int64_t DecoderWorker::steadyNowNs() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void DecoderWorker::publishOpenTransition(const OpenTransitionSnapshot &snap) noexcept {
  const uint64_t seq = m_openTelemSeq.load(std::memory_order_relaxed);
  m_openTelemSeq.store(seq + 1, std::memory_order_release);
  m_openTelemData.openSeq.store(snap.openSeq, std::memory_order_relaxed);
  m_openTelemData.requestId.store(snap.requestId, std::memory_order_relaxed);
  m_openTelemData.openRequestedNs.store(snap.openRequestedNs, std::memory_order_relaxed);
  m_openTelemData.cmdNotifiedNs.store(snap.cmdNotifiedNs, std::memory_order_relaxed);
  m_openTelemData.workerWokeNs.store(snap.workerWokeNs, std::memory_order_relaxed);
  m_openTelemData.openProcessStartNs.store(snap.openProcessStartNs, std::memory_order_relaxed);
  m_openTelemData.decoderOpenStartNs.store(snap.decoderOpenStartNs, std::memory_order_relaxed);
  m_openTelemData.decoderOpenEndNs.store(snap.decoderOpenEndNs, std::memory_order_relaxed);
  m_openTelemData.stateCommitStartNs.store(snap.stateCommitStartNs, std::memory_order_relaxed);
  m_openTelemData.ringResetNs.store(snap.ringResetNs, std::memory_order_relaxed);
  m_openTelemData.eofClearedNs.store(snap.eofClearedNs, std::memory_order_relaxed);
  m_openTelemData.resamplerConfigStartNs.store(snap.resamplerConfigStartNs, std::memory_order_relaxed);
  m_openTelemData.resamplerConfigEndNs.store(snap.resamplerConfigEndNs, std::memory_order_relaxed);
  m_openTelemData.playingClearedNs.store(snap.playingClearedNs, std::memory_order_relaxed);
  m_openTelemData.stateCommitEndNs.store(snap.stateCommitEndNs, std::memory_order_relaxed);
  m_openTelemData.prefillStartNs.store(snap.prefillStartNs, std::memory_order_relaxed);
  m_openTelemData.firstPcmSupplyNs.store(snap.firstPcmSupplyNs, std::memory_order_relaxed);
  m_openTelemData.prefillEndNs.store(snap.prefillEndNs, std::memory_order_relaxed);
  m_openTelemData.openCompleteNs.store(snap.openCompleteNs, std::memory_order_relaxed);
  m_openTelemData.prefillInputFrames.store(snap.prefillInputFrames, std::memory_order_relaxed);
  m_openTelemData.prefillRingAvailAfter.store(snap.prefillRingAvailAfter, std::memory_order_relaxed);
  m_openTelemData.eofBefore.store(snap.eofBefore, std::memory_order_relaxed);
  m_openTelemData.eofAfter.store(snap.eofAfter, std::memory_order_relaxed);
  m_openTelemData.playingBefore.store(snap.playingBefore, std::memory_order_relaxed);
  m_openTelemData.playingAfter.store(snap.playingAfter, std::memory_order_relaxed);
  m_openTelemData.valid.store(snap.valid, std::memory_order_relaxed);
  m_openTelemSeq.store(seq + 2, std::memory_order_release);
}

OpenTransitionSnapshot DecoderWorker::readOpenTransitionSnapshot() const noexcept {
  OpenTransitionSnapshot snap{};
  for (int attempt = 0; attempt < 64; ++attempt) {
    const uint64_t s1 = m_openTelemSeq.load(std::memory_order_acquire);
    if ((s1 & 1ULL) != 0ULL) {
      continue;
    }
    snap.openSeq = m_openTelemData.openSeq.load(std::memory_order_relaxed);
    snap.requestId = m_openTelemData.requestId.load(std::memory_order_relaxed);
    snap.openRequestedNs = m_openTelemData.openRequestedNs.load(std::memory_order_relaxed);
    snap.cmdNotifiedNs = m_openTelemData.cmdNotifiedNs.load(std::memory_order_relaxed);
    snap.workerWokeNs = m_openTelemData.workerWokeNs.load(std::memory_order_relaxed);
    snap.openProcessStartNs = m_openTelemData.openProcessStartNs.load(std::memory_order_relaxed);
    snap.decoderOpenStartNs = m_openTelemData.decoderOpenStartNs.load(std::memory_order_relaxed);
    snap.decoderOpenEndNs = m_openTelemData.decoderOpenEndNs.load(std::memory_order_relaxed);
    snap.stateCommitStartNs = m_openTelemData.stateCommitStartNs.load(std::memory_order_relaxed);
    snap.ringResetNs = m_openTelemData.ringResetNs.load(std::memory_order_relaxed);
    snap.eofClearedNs = m_openTelemData.eofClearedNs.load(std::memory_order_relaxed);
    snap.resamplerConfigStartNs = m_openTelemData.resamplerConfigStartNs.load(std::memory_order_relaxed);
    snap.resamplerConfigEndNs = m_openTelemData.resamplerConfigEndNs.load(std::memory_order_relaxed);
    snap.playingClearedNs = m_openTelemData.playingClearedNs.load(std::memory_order_relaxed);
    snap.stateCommitEndNs = m_openTelemData.stateCommitEndNs.load(std::memory_order_relaxed);
    snap.prefillStartNs = m_openTelemData.prefillStartNs.load(std::memory_order_relaxed);
    snap.firstPcmSupplyNs = m_openTelemData.firstPcmSupplyNs.load(std::memory_order_relaxed);
    snap.prefillEndNs = m_openTelemData.prefillEndNs.load(std::memory_order_relaxed);
    snap.openCompleteNs = m_openTelemData.openCompleteNs.load(std::memory_order_relaxed);
    snap.prefillInputFrames = m_openTelemData.prefillInputFrames.load(std::memory_order_relaxed);
    snap.prefillRingAvailAfter = m_openTelemData.prefillRingAvailAfter.load(std::memory_order_relaxed);
    snap.eofBefore = m_openTelemData.eofBefore.load(std::memory_order_relaxed);
    snap.eofAfter = m_openTelemData.eofAfter.load(std::memory_order_relaxed);
    snap.playingBefore = m_openTelemData.playingBefore.load(std::memory_order_relaxed);
    snap.playingAfter = m_openTelemData.playingAfter.load(std::memory_order_relaxed);
    snap.valid = m_openTelemData.valid.load(std::memory_order_relaxed);
    const uint64_t s2 = m_openTelemSeq.load(std::memory_order_acquire);
    if (s1 == s2) {
      const int64_t playNs = m_lastPlayRequestedNs.load(std::memory_order_acquire);
      const uint64_t playReqId = m_lastPlayRequestId.load(std::memory_order_acquire);
      if (snap.valid && playReqId == snap.requestId && playNs > 0) {
        snap.playRequestedNs = playNs;
      }
      return snap;
    }
  }
  return snap;
}

void DecoderWorker::publishDecodePerf(const DecodePerfStats &stats) noexcept {
  const uint64_t seq = m_perfSeq.load(std::memory_order_relaxed);
  m_perfSeq.store(seq + 1, std::memory_order_release);
  m_perfData.lastChunkTotalNs.store(stats.lastChunkTotalNs, std::memory_order_relaxed);
  m_perfData.maxChunkTotalNs.store(stats.maxChunkTotalNs, std::memory_order_relaxed);
  m_perfData.lastReadFramesNs.store(stats.lastReadFramesNs, std::memory_order_relaxed);
  m_perfData.maxReadFramesNs.store(stats.maxReadFramesNs, std::memory_order_relaxed);
  m_perfData.lastFileReadPacketNs.store(stats.lastFileReadPacketNs, std::memory_order_relaxed);
  m_perfData.maxFileReadPacketNs.store(stats.maxFileReadPacketNs, std::memory_order_relaxed);
  m_perfData.lastResampleNs.store(stats.lastResampleNs, std::memory_order_relaxed);
  m_perfData.maxResampleNs.store(stats.maxResampleNs, std::memory_order_relaxed);
  m_perfData.lastWorkerIterIntervalNs.store(stats.lastWorkerIterIntervalNs, std::memory_order_relaxed);
  m_perfData.maxWorkerIterIntervalNs.store(stats.maxWorkerIterIntervalNs, std::memory_order_relaxed);
  m_perfData.totalChunksDecoded.store(stats.totalChunksDecoded, std::memory_order_relaxed);
  m_perfSeq.store(seq + 2, std::memory_order_release);
}

DecodePerfStats DecoderWorker::readDecodePerfStats() const noexcept {
  DecodePerfStats stats{};
  for (int attempt = 0; attempt < 64; ++attempt) {
    const uint64_t s1 = m_perfSeq.load(std::memory_order_acquire);
    if ((s1 & 1ULL) != 0ULL) {
      continue;
    }
    stats.lastChunkTotalNs = m_perfData.lastChunkTotalNs.load(std::memory_order_relaxed);
    stats.maxChunkTotalNs = m_perfData.maxChunkTotalNs.load(std::memory_order_relaxed);
    stats.lastReadFramesNs = m_perfData.lastReadFramesNs.load(std::memory_order_relaxed);
    stats.maxReadFramesNs = m_perfData.maxReadFramesNs.load(std::memory_order_relaxed);
    stats.lastFileReadPacketNs = m_perfData.lastFileReadPacketNs.load(std::memory_order_relaxed);
    stats.maxFileReadPacketNs = m_perfData.maxFileReadPacketNs.load(std::memory_order_relaxed);
    stats.lastResampleNs = m_perfData.lastResampleNs.load(std::memory_order_relaxed);
    stats.maxResampleNs = m_perfData.maxResampleNs.load(std::memory_order_relaxed);
    stats.lastWorkerIterIntervalNs = m_perfData.lastWorkerIterIntervalNs.load(std::memory_order_relaxed);
    stats.maxWorkerIterIntervalNs = m_perfData.maxWorkerIterIntervalNs.load(std::memory_order_relaxed);
    stats.totalChunksDecoded = m_perfData.totalChunksDecoded.load(std::memory_order_relaxed);
    const uint64_t s2 = m_perfSeq.load(std::memory_order_acquire);
    if (s1 == s2) {
      return stats;
    }
  }
  return stats;
}

bool DecoderWorker::popUnderrunSnapshot(UnderrunSnapshot &out) noexcept {
  const uint64_t readIdx = m_underrunReadIdx.load(std::memory_order_relaxed);
  const uint64_t writeIdx = m_underrunWriteIdx.load(std::memory_order_acquire);
  if (readIdx >= writeIdx) {
    return false;
  }
  out = m_underrunRing[readIdx % UNDERRUN_RING_SIZE];
  m_underrunReadIdx.store(readIdx + 1, std::memory_order_release);
  return true;
}

DecoderWorker::DecoderWorker() {
  m_thread = std::thread(&DecoderWorker::workerLoop, this);
}

DecoderWorker::~DecoderWorker() {
  {
    std::lock_guard<std::mutex> trackLock(m_trackMutex);
    m_activeRequestId.store(0, std::memory_order_release);
    m_running.store(false, std::memory_order_release);
  }
  {
    std::lock_guard<std::mutex> lock(m_cmdMutex);
    for (auto &cmd : m_pendingCommands) {
      if (cmd.openPromise) {
        try {
          cmd.openPromise->set_value(false);
        } catch (...) {
        }
      }
    }
    m_pendingCommands.clear();
    m_cmdCv.notify_all();
  }
  if (m_thread.joinable()) {
    m_thread.join();
  }
}

bool DecoderWorker::openTrack(const std::string &filePath,
                              double graphSampleRate) {
  auto promise = std::make_shared<std::promise<bool>>();
  auto future = promise->get_future();
  uint64_t seq = 0;
  uint64_t reqId = 0;
  uint32_t timeoutMs = 5000;

  // Linearization Point: Lock m_trackMutex and m_cmdMutex together in strict
  // hierarchical order to atomically issue the Request ID and enqueue the
  // command, eliminating Close/Open interleaving race
  {
    std::lock_guard<std::mutex> trackLock(m_trackMutex);
    std::lock_guard<std::mutex> cmdLock(m_cmdMutex);

    timeoutMs = m_openTimeoutMs.load(std::memory_order_acquire);
    reqId = m_nextRequestId.fetch_add(1, std::memory_order_relaxed);
    m_activeRequestId.store(reqId, std::memory_order_release);
    m_lastOpenStatus.store(OpenStatus::None, std::memory_order_release);

    seq = ++m_seekSequence;
    WorkerCommand cmd;
    cmd.type = WorkerCommandType::Open;
    cmd.path = filePath;
    cmd.graphRate = graphSampleRate;
    cmd.sequence = seq;
    cmd.requestId = reqId;
    cmd.requestedNs = steadyNowNs();
    cmd.openPromise = promise;
    m_openPhase.store(static_cast<uint8_t>(OpenTransitionPhase::OpenQueued),
                      std::memory_order_release);
    m_pendingCommands.push_back(std::move(cmd));
    m_cmdCv.notify_one();
    m_lastCmdNotifiedNs.store(steadyNowNs(), std::memory_order_release);
  }

  if (future.wait_for(std::chrono::milliseconds(timeoutMs)) ==
      std::future_status::ready) {
    return future.get();
  }

  // P0-26: Timed out: Invalidate active request ID under m_trackMutex to
  // serialize with worker commit
  {
    std::lock_guard<std::mutex> lock(m_trackMutex);
    if (m_committedRequestId.load(std::memory_order_acquire) == reqId) {
      return true;
    }
    uint64_t expected = reqId;
    if (m_activeRequestId.compare_exchange_strong(expected, 0,
                                                  std::memory_order_acq_rel)) {
      m_lastOpenStatus.store(OpenStatus::TimedOut, std::memory_order_release);
    }
  }
  return false;
}

bool DecoderWorker::openSyntheticTrack(uint64_t totalFrames, double sampleRate,
                                       double graphSampleRate,
                                       const std::string &path) {
  auto promise = std::make_shared<std::promise<bool>>();
  auto future = promise->get_future();
  uint64_t seq = 0;
  uint64_t reqId = 0;
  uint32_t timeoutMs = 5000;

  // Linearization Point: Lock m_trackMutex and m_cmdMutex together in strict
  // hierarchical order
  {
    std::lock_guard<std::mutex> trackLock(m_trackMutex);
    std::lock_guard<std::mutex> cmdLock(m_cmdMutex);

    timeoutMs = m_openTimeoutMs.load(std::memory_order_acquire);
    reqId = m_nextRequestId.fetch_add(1, std::memory_order_relaxed);
    m_activeRequestId.store(reqId, std::memory_order_release);
    m_lastOpenStatus.store(OpenStatus::None, std::memory_order_release);

    seq = ++m_seekSequence;
    WorkerCommand cmd;
    cmd.type = WorkerCommandType::OpenSynthetic;
    cmd.path = path;
    cmd.sampleRate = sampleRate;
    cmd.graphRate = graphSampleRate;
    cmd.totalFrames = totalFrames;
    cmd.sequence = seq;
    cmd.requestId = reqId;
    cmd.isSynthetic = true;
    cmd.requestedNs = steadyNowNs();
    cmd.openPromise = promise;
    m_openPhase.store(static_cast<uint8_t>(OpenTransitionPhase::OpenQueued),
                      std::memory_order_release);
    m_pendingCommands.push_back(std::move(cmd));
    m_cmdCv.notify_one();
    m_lastCmdNotifiedNs.store(steadyNowNs(), std::memory_order_release);
  }

  if (future.wait_for(std::chrono::milliseconds(timeoutMs)) ==
      std::future_status::ready) {
    return future.get();
  }

  // P0-26: Timed out: Invalidate active request ID under m_trackMutex
  {
    std::lock_guard<std::mutex> lock(m_trackMutex);
    if (m_committedRequestId.load(std::memory_order_acquire) == reqId) {
      return true;
    }
    uint64_t expected = reqId;
    if (m_activeRequestId.compare_exchange_strong(expected, 0,
                                                  std::memory_order_acq_rel)) {
      m_lastOpenStatus.store(OpenStatus::TimedOut, std::memory_order_release);
    }
  }
  return false;
}

void DecoderWorker::closeTrack() {
  auto promise = std::make_shared<std::promise<bool>>();
  auto future = promise->get_future();

  // P0-26: Atomically invalidate request ID and enqueue CloseTrack command
  // under hierarchical lock
  {
    std::lock_guard<std::mutex> trackLock(m_trackMutex);
    std::lock_guard<std::mutex> cmdLock(m_cmdMutex);

    uint64_t closingReqId = m_activeRequestId.load(std::memory_order_relaxed);
    if (closingReqId == 0) {
      closingReqId = m_committedRequestId.load(std::memory_order_relaxed);
    }
    m_activeRequestId.store(0, std::memory_order_release);
    m_lastOpenStatus.store(OpenStatus::Cancelled, std::memory_order_release);

    WorkerCommand cmd;
    cmd.type = WorkerCommandType::CloseTrack;
    cmd.targetRequestId =
        closingReqId; // Tag generation so older close doesn't close newer open
    cmd.openPromise = promise;
    m_pendingCommands.push_back(std::move(cmd));
    m_cmdCv.notify_one();
  }

  if (future.wait_for(std::chrono::milliseconds(1000)) ==
      std::future_status::ready) {
    future.get();
  }
}

void DecoderWorker::play() noexcept {
  {
    std::lock_guard<std::mutex> lock(m_cmdMutex);
    m_lastPlayRequestId.store(m_committedRequestId.load(std::memory_order_acquire),
                              std::memory_order_release);
    m_lastPlayRequestedNs.store(steadyNowNs(), std::memory_order_release);
    m_isPlaying.store(true, std::memory_order_release);
    WorkerCommand cmd;
    cmd.type = WorkerCommandType::Play;
    m_pendingCommands.push_back(cmd);
    m_cmdCv.notify_one();
  }
}

void DecoderWorker::pause() noexcept {
  {
    std::lock_guard<std::mutex> lock(m_cmdMutex);
    m_isPlaying.store(false, std::memory_order_release);
    m_eofEpoch.fetch_add(1, std::memory_order_acq_rel);
    m_eofDelivered.store(false, std::memory_order_release);
    WorkerCommand cmd;
    cmd.type = WorkerCommandType::Pause;
    m_pendingCommands.push_back(cmd);
    m_cmdCv.notify_one();
  }
}

void DecoderWorker::stop() noexcept {
  {
    std::lock_guard<std::mutex> lock(m_cmdMutex);
    m_isPlaying.store(false, std::memory_order_release);
    m_playhead.store(0, std::memory_order_release);
    m_eofEpoch.fetch_add(1, std::memory_order_acq_rel);
    m_eofDelivered.store(false, std::memory_order_release);
    WorkerCommand cmd;
    cmd.type = WorkerCommandType::Stop;
    m_pendingCommands.push_back(cmd);
    m_cmdCv.notify_one();
  }
}

void DecoderWorker::seek(int64_t targetFrame) noexcept {
  (void)seekWithResult(targetFrame);
}

SeekResult DecoderWorker::seekWithResult(int64_t targetFrame) noexcept {
  SeekResult res;
  res.requestedFrame = targetFrame;

  const WorkerTrackInfo info = trackInfo();
  res.sampleRateHz = info.sampleRate;
  res.totalFrames = info.totalFrames;

  if (!m_isOpen.load(std::memory_order_acquire)) {
    res.success = false;
    res.actualFrame = m_playhead.load(std::memory_order_acquire);
    res.errorCode = 1010; // ErrorCode::SeekNotOpen
    res.stage = "not_open";
    res.errorMessage = "Track is not open";
    return res;
  }

  if (targetFrame < 0 ||
      (info.totalFrames > 0 &&
       static_cast<uint64_t>(targetFrame) > info.totalFrames)) {
    res.success = false;
    res.actualFrame = m_playhead.load(std::memory_order_acquire);
    res.errorCode = 1011; // ErrorCode::SeekOutOfRange
    res.stage = "out_of_range";
    res.errorMessage = "Seek target frame out of track range";
    return res;
  }

  // Determine whether targetFrame falls within currently buffered PCM range
  const int64_t beforePlayhead = m_playhead.load(std::memory_order_acquire);
  const size_t availOutFrames = m_ringBuffer.availableRead();
  const double gRate = m_graphRate.load(std::memory_order_acquire);
  const double tRate = info.sampleRate;
  const int64_t availTrackFrames =
      (gRate > 0.0 && tRate > 0.0)
          ? static_cast<int64_t>(static_cast<double>(availOutFrames) *
                                 (tRate / gRate))
          : static_cast<int64_t>(availOutFrames);
  res.withinBuffer = (availTrackFrames > 0 && targetFrame >= beforePlayhead &&
                      targetFrame < beforePlayhead + availTrackFrames);

  uint64_t seq = 0;
  {
    std::lock_guard<std::mutex> lock(m_cmdMutex);
    m_playhead.store(targetFrame, std::memory_order_release);
    m_eofEpoch.fetch_add(1, std::memory_order_acq_rel);
    m_eofDelivered.store(false, std::memory_order_release);
    // Note: Do NOT touch m_ringBuffer directly from Control Thread (P0-04)
    seq = ++m_seekSequence;
    WorkerCommand cmd;
    cmd.type = WorkerCommandType::Seek;
    cmd.targetFrame = targetFrame;
    cmd.sequence = seq;
    m_pendingCommands.push_back(cmd);
    m_cmdCv.notify_one();
  }

  // Wait briefly for worker to process seek and prime buffer with at least 256
  // frames
  bool timedOut = false;
  auto start = std::chrono::steady_clock::now();
  while (m_seekCompleted.load(std::memory_order_acquire) < seq &&
         !m_isEof.load(std::memory_order_acquire)) {
    std::this_thread::yield();
    if (std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start)
            .count() > 50.0) {
      timedOut = true;
      break;
    }
  }

  if (timedOut) {
    res.success = false;
    res.actualFrame = m_playhead.load(std::memory_order_acquire);
    res.errorCode = 1013; // ErrorCode::SeekTimeout
    res.stage = "timeout";
    res.errorMessage = "Decoder worker timed out waiting for seek completion";
    return res;
  }

  if (!m_seekSuccess.load(std::memory_order_acquire)) {
    res.success = false;
    res.actualFrame = m_playhead.load(std::memory_order_acquire);
    res.errorCode = 1012; // ErrorCode::SeekDecoderFailed
    res.stage = "decoder_seek";
    res.errorMessage = "Audio decoder failed to seek to target frame";
    return res;
  }

  res.success = true;
  res.actualFrame = m_seekActualFrame.load(std::memory_order_acquire);
  return res;
}

void DecoderWorker::configureGraphRate(double graphSampleRate) noexcept {
  {
    std::lock_guard<std::mutex> lock(m_cmdMutex);
    m_graphRate.store(graphSampleRate, std::memory_order_release);
    WorkerCommand cmd;
    cmd.type = WorkerCommandType::ConfigureRate;
    cmd.graphRate = graphSampleRate;
    m_pendingCommands.push_back(cmd);
    m_cmdCv.notify_one();
  }
}

size_t DecoderWorker::readAudioBlock(double *outLeft, double *outRight,
                                     size_t frames) noexcept {
  const uint64_t transSeqEntry =
      m_stateTransitionSeq.load(std::memory_order_acquire);
  const bool openAtEntry = m_isOpen.load(std::memory_order_acquire);
  const bool playingAtEntry = m_isPlaying.load(std::memory_order_acquire);
  if ((transSeqEntry & 1ULL) != 0ULL || !openAtEntry || !playingAtEntry) {
    std::fill_n(outLeft, frames, 0.0);
    std::fill_n(outRight, frames, 0.0);
    return 0;
  }

  const uint64_t epoch = m_eofEpoch.load(std::memory_order_acquire);
  const uint64_t reqId = m_committedRequestId.load(std::memory_order_acquire);
  size_t read = m_ringBuffer.readPlanar(outLeft, outRight, frames);
  if (read < frames) {
    // Buffer underrun or EOF
    std::fill_n(outLeft + read, frames - read, 0.0);
    std::fill_n(outRight + read, frames - read, 0.0);

    if (m_stateTransitionSeq.load(std::memory_order_acquire) != transSeqEntry) {
      // Open state transition started while readAudioBlock() was in flight (TOCTOU guard).
      return read;
    }

    const bool eofAtCheck = m_isEof.load(std::memory_order_acquire);
    if (eofAtCheck && m_ringBuffer.availableRead() == 0) {
      if (m_eofEpoch.load(std::memory_order_acquire) == epoch &&
          m_eofDeliveredEpoch.load(std::memory_order_relaxed) != epoch) {
        m_eofDeliveredEpoch.store(epoch, std::memory_order_release);
        m_eofRequestId.store(reqId, std::memory_order_release);
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
        m_eofEmittedTimestampMs.store(nowMs, std::memory_order_release);
        m_eofDelivered.store(true, std::memory_order_release);
      }
    } else {
      const uint64_t newTotal =
          m_underrunCount.fetch_add(1, std::memory_order_relaxed) + 1;
      const uint64_t wIdx = m_underrunWriteIdx.load(std::memory_order_relaxed);
      const uint64_t rIdx = m_underrunReadIdx.load(std::memory_order_acquire);
      if (wIdx - rIdx < UNDERRUN_RING_SIZE) {
        UnderrunSnapshot &slot = m_underrunRing[wIdx % UNDERRUN_RING_SIZE];
        slot.steadyNs = steadyNowNs();
        slot.underrunTotal = newTotal;
        slot.requestId = reqId;
        slot.activeRequestId = m_activeRequestId.load(std::memory_order_acquire);
        slot.openSeq = m_openSeqCounter.load(std::memory_order_acquire);
        slot.eofEpoch = epoch;
        slot.ringCapacityFrames = m_ringBuffer.capacity();
        slot.ringAvailBeforeRead = read;
        slot.requestedFrames = frames;
        slot.readFrames = read;
        slot.openPhase = m_openPhase.load(std::memory_order_acquire);
        slot.isOpenAtEntry = openAtEntry;
        slot.isPlayingAtEntry = playingAtEntry;
        slot.isEofAtCheck = eofAtCheck;
        slot.isOpenAtUnderrun = m_isOpen.load(std::memory_order_acquire);
        slot.isPlayingAtUnderrun = m_isPlaying.load(std::memory_order_acquire);
        m_underrunWriteIdx.store(wIdx + 1, std::memory_order_release);
      } else {
        m_underrunSnapshotDropCount.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }

  m_playhead.fetch_add(static_cast<int64_t>(read), std::memory_order_relaxed);
  return read;
}

void DecoderWorker::workerLoop() {
  while (m_running.load(std::memory_order_acquire)) {
    const int64_t iterStartNs = steadyNowNs();
    if (m_lastWorkerIterNs > 0 && iterStartNs >= m_lastWorkerIterNs) {
      const uint64_t intervalNs =
          static_cast<uint64_t>(iterStartNs - m_lastWorkerIterNs);
      m_workerPerf.lastWorkerIterIntervalNs = intervalNs;
      if (intervalNs > m_workerPerf.maxWorkerIterIntervalNs) {
        m_workerPerf.maxWorkerIterIntervalNs = intervalNs;
      }
      publishDecodePerf(m_workerPerf);
    }
    m_lastWorkerIterNs = iterStartNs;

    std::vector<WorkerCommand> cmds;
    {
      std::unique_lock<std::mutex> lock(m_cmdMutex);
      if (m_pendingCommands.empty() &&
          (!m_isOpen.load(std::memory_order_relaxed) ||
           m_isEof.load(std::memory_order_relaxed) ||
           (m_pendingOutCount == 0 && m_ringBuffer.availableWrite() < 4096))) {
        const auto waitDuration = (m_isOpen.load(std::memory_order_relaxed) &&
                                   !m_isEof.load(std::memory_order_relaxed))
                                      ? std::chrono::milliseconds(2)
                                      : std::chrono::milliseconds(10);
        m_cmdCv.wait_for(lock, waitDuration, [this]() {
          return !m_running.load(std::memory_order_relaxed) ||
                 !m_pendingCommands.empty() ||
                 (m_isOpen.load(std::memory_order_relaxed) &&
                  !m_isEof.load(std::memory_order_relaxed) &&
                  (m_pendingOutCount > 0 ||
                   m_ringBuffer.availableWrite() >= 4096));
        });
      }
      cmds.swap(m_pendingCommands);
    }
    const int64_t wokeNs = steadyNowNs();
    if (!cmds.empty()) {
      m_workerOpenTelem.workerWokeNs = wokeNs;
    }

    if (!m_running.load(std::memory_order_acquire)) {
      for (auto &cmd : cmds) {
        if (cmd.openPromise) {
          try {
            cmd.openPromise->set_value(false);
          } catch (...) {
          }
        }
      }
      break;
    }

    for (const auto &cmd : cmds) {
      if (!m_running.load(std::memory_order_acquire)) {
        if (cmd.openPromise) {
          try {
            cmd.openPromise->set_value(false);
          } catch (...) {
          }
        }
        continue;
      }
      processCommand(cmd);
    }

    while (m_running.load(std::memory_order_relaxed) &&
           m_isOpen.load(std::memory_order_relaxed) &&
           !m_isEof.load(std::memory_order_relaxed) &&
           (m_pendingOutCount > 0 || m_ringBuffer.availableWrite() >= 4096)) {

      // Prevent busy spin if ring buffer is temporarily completely full
      if (m_pendingOutCount > 0 && m_ringBuffer.availableWrite() == 0) {
        std::unique_lock<std::mutex> lock(m_cmdMutex);
        m_cmdCv.wait_for(lock, std::chrono::milliseconds(5), [this]() {
          return !m_running.load(std::memory_order_relaxed) ||
                 !m_pendingCommands.empty() ||
                 m_ringBuffer.availableWrite() > 0;
        });
        if (!m_pendingCommands.empty() ||
            !m_running.load(std::memory_order_relaxed))
          break;
      }

      doDecodeChunk();
      {
        std::lock_guard<std::mutex> lock(m_cmdMutex);
        if (!m_pendingCommands.empty())
          break;
      }
    }
  }
}

void DecoderWorker::processCommand(const WorkerCommand &cmd) {
  switch (cmd.type) {
  case WorkerCommandType::Open: {
    // P0-26: Check if this open request was cancelled, timed out, or superseded
    // before heavy I/O
    if (cmd.requestId != 0 &&
        cmd.requestId != m_activeRequestId.load(std::memory_order_acquire)) {
      if (cmd.openPromise) {
        try {
          cmd.openPromise->set_value(false);
        } catch (...) {
        }
      }
      break;
    }

    const uint64_t openSeq = m_openSeqCounter.fetch_add(1, std::memory_order_acq_rel) + 1;
    const int64_t savedWorkerWokeNs = m_workerOpenTelem.workerWokeNs;
    m_workerOpenTelem = OpenTransitionSnapshot{};
    m_workerOpenTelem.openSeq = openSeq;
    m_workerOpenTelem.requestId = cmd.requestId;
    m_workerOpenTelem.openRequestedNs = cmd.requestedNs;
    m_workerOpenTelem.cmdNotifiedNs = m_lastCmdNotifiedNs.load(std::memory_order_acquire);
    m_workerOpenTelem.workerWokeNs = savedWorkerWokeNs;
    m_workerOpenTelem.openProcessStartNs = steadyNowNs();
    m_openPhase.store(static_cast<uint8_t>(OpenTransitionPhase::DecoderOpening),
                      std::memory_order_release);

    bool openSuccess = false;
    auto dec = std::make_unique<FFmpegAudioDecoder>();
    m_workerOpenTelem.decoderOpenStartNs = steadyNowNs();
    const bool decOpened = dec->open(cmd.path);
    m_workerOpenTelem.decoderOpenEndNs = steadyNowNs();
    if (decOpened) {
      // Trigger pre-commit hook if installed (testing synchronization)
      std::function<void(uint64_t)> hookCopy;
#ifdef DESK32_ENABLE_TEST_HOOKS
      std::function<void(uint64_t)> inTransitionHookCopy;
#endif
      {
        std::lock_guard<std::mutex> lock(m_trackMutex);
        hookCopy = m_preCommitHook;
#ifdef DESK32_ENABLE_TEST_HOOKS
        inTransitionHookCopy = m_inTransitionHook;
#endif
      }
      if (hookCopy) {
        hookCopy(cmd.requestId);
      }

      // P0-26: Re-verify request ID and commit state atomically under
      // m_trackMutex
      bool committed = false;
      {
        std::lock_guard<std::mutex> lock(m_trackMutex);
        if (cmd.requestId != 0 &&
            cmd.requestId ==
                m_activeRequestId.load(std::memory_order_acquire)) {
          m_openPhase.store(static_cast<uint8_t>(OpenTransitionPhase::StateCommitStart),
                            std::memory_order_release);
          m_workerOpenTelem.stateCommitStartNs = steadyNowNs();
          m_stateTransitionSeq.fetch_add(1, std::memory_order_acq_rel);

          m_workerOpenTelem.playingBefore = m_isPlaying.load(std::memory_order_relaxed);
          m_isPlaying.store(false, std::memory_order_release);
          m_workerOpenTelem.playingAfter = false;
          m_workerOpenTelem.playingClearedNs = steadyNowNs();

          m_ringBuffer.resetProducer(); // P0-04: Producer-owned reset
          m_workerOpenTelem.ringResetNs = steadyNowNs();
          m_pendingOutOffset = 0;
          m_pendingOutCount = 0;
          m_syntheticPos = 0;

          m_workerOpenTelem.eofBefore = m_isEof.load(std::memory_order_relaxed);
          m_isEof.store(false, std::memory_order_release);
          m_workerOpenTelem.eofAfter = false;
          m_workerOpenTelem.eofClearedNs = steadyNowNs();
          m_openPhase.store(
              static_cast<uint8_t>(OpenTransitionPhase::AfterEofClearedBeforePlayCleared),
              std::memory_order_release);

          m_eofDelivered.store(false, std::memory_order_release);
          m_eofEpoch.fetch_add(1, std::memory_order_acq_rel);

#ifdef DESK32_ENABLE_TEST_HOOKS
          if (inTransitionHookCopy) {
            inTransitionHookCopy(cmd.requestId);
          }
#endif

          const auto &info = dec->formatInfo();
          m_trackInfo.path = cmd.path;
          m_trackInfo.sampleRate = info.sampleRate;
          m_trackInfo.channels = info.channels;
          m_trackInfo.totalFrames =
              static_cast<uint64_t>(std::max<int64_t>(0, info.totalFrames));
          m_metadata =
              std::make_shared<metadata::TrackMetadata>(dec->metadata());
          m_decoder = std::move(dec);
          m_workerOpenTelem.resamplerConfigStartNs = steadyNowNs();
          m_resampler.configure(info.sampleRate, cmd.graphRate, info.channels);
          m_workerOpenTelem.resamplerConfigEndNs = steadyNowNs();
          m_isSynthetic.store(false, std::memory_order_release);
          m_graphRate.store(cmd.graphRate, std::memory_order_release);
          m_playhead.store(0, std::memory_order_release);
          m_isOpen.store(true, std::memory_order_release);
          m_openPhase.store(static_cast<uint8_t>(OpenTransitionPhase::AfterPlayCleared),
                            std::memory_order_release);

          m_committedRequestId.store(cmd.requestId, std::memory_order_release);
          m_lastOpenStatus.store(OpenStatus::Success,
                                 std::memory_order_release);
          m_stateTransitionSeq.fetch_add(1, std::memory_order_release);
          m_workerOpenTelem.stateCommitEndNs = steadyNowNs();
          committed = true;
        }
      }

      if (!committed) {
        dec.reset();
        m_openPhase.store(static_cast<uint8_t>(OpenTransitionPhase::Idle),
                          std::memory_order_release);
        if (cmd.openPromise) {
          try {
            cmd.openPromise->set_value(false);
          } catch (...) {
          }
        }
        break;
      }

      m_openPhase.store(static_cast<uint8_t>(OpenTransitionPhase::Prefilling),
                        std::memory_order_release);
      m_workerOpenTelem.prefillStartNs = steadyNowNs();
      for (int i = 0; i < 4; ++i) {
        doDecodeChunk();
      }
      m_workerOpenTelem.prefillEndNs = steadyNowNs();
      m_workerOpenTelem.prefillInputFrames = CHUNK_SIZE * 4;
      m_workerOpenTelem.prefillRingAvailAfter = m_ringBuffer.availableRead();
      m_workerOpenTelem.openCompleteNs = steadyNowNs();
      m_workerOpenTelem.valid = true;
      publishOpenTransition(m_workerOpenTelem);
      m_openPhase.store(static_cast<uint8_t>(OpenTransitionPhase::OpenComplete),
                        std::memory_order_release);
      openSuccess = true;
    } else {
      // Open failed: Only update status if this request is still the active
      // request
      std::lock_guard<std::mutex> lock(m_trackMutex);
      if (cmd.requestId == 0 ||
          cmd.requestId == m_activeRequestId.load(std::memory_order_acquire)) {
        m_isOpen.store(false, std::memory_order_release);
        m_activeRequestId.store(0, std::memory_order_release);
        m_lastOpenStatus.store(OpenStatus::Failed, std::memory_order_release);
      }
      m_openPhase.store(static_cast<uint8_t>(OpenTransitionPhase::Idle),
                        std::memory_order_release);
      openSuccess = false;
    }

    if (cmd.openPromise) {
      try {
        cmd.openPromise->set_value(openSuccess);
      } catch (...) {
      }
    }
    m_seekCompleted.store(cmd.sequence, std::memory_order_release);
    break;
  }
  case WorkerCommandType::OpenSynthetic: {
    // P0-26: Check if request was superseded before preparation
    if (cmd.requestId != 0 &&
        cmd.requestId != m_activeRequestId.load(std::memory_order_acquire)) {
      if (cmd.openPromise) {
        try {
          cmd.openPromise->set_value(false);
        } catch (...) {
        }
      }
      break;
    }

    const uint64_t openSeq = m_openSeqCounter.fetch_add(1, std::memory_order_acq_rel) + 1;
    const int64_t savedWorkerWokeNs = m_workerOpenTelem.workerWokeNs;
    m_workerOpenTelem = OpenTransitionSnapshot{};
    m_workerOpenTelem.openSeq = openSeq;
    m_workerOpenTelem.requestId = cmd.requestId;
    m_workerOpenTelem.openRequestedNs = cmd.requestedNs;
    m_workerOpenTelem.cmdNotifiedNs = m_lastCmdNotifiedNs.load(std::memory_order_acquire);
    m_workerOpenTelem.workerWokeNs = savedWorkerWokeNs;
    m_workerOpenTelem.openProcessStartNs = steadyNowNs();
    m_openPhase.store(static_cast<uint8_t>(OpenTransitionPhase::DecoderOpening),
                      std::memory_order_release);

    WorkerTrackInfo tInfo;
    tInfo.path = cmd.path;
    tInfo.sampleRate = cmd.sampleRate;
    tInfo.channels = 2;
    tInfo.totalFrames = cmd.totalFrames;

    metadata::TrackMetadata meta;
    meta.title = "Synthetic Reference Signal";
    meta.artist = "Desk32 DSP Test Engine";
    meta.sampleRate = cmd.sampleRate;
    meta.channels = 2;
    meta.totalFrames = cmd.totalFrames;
    meta.durationSeconds =
        (cmd.sampleRate > 0)
            ? (static_cast<double>(cmd.totalFrames) / cmd.sampleRate)
            : 0.0;

    // Trigger pre-commit hook if installed
    std::function<void(uint64_t)> hookCopy;
#ifdef DESK32_ENABLE_TEST_HOOKS
    std::function<void(uint64_t)> inTransitionHookCopy;
#endif
    {
      std::lock_guard<std::mutex> lock(m_trackMutex);
      hookCopy = m_preCommitHook;
#ifdef DESK32_ENABLE_TEST_HOOKS
      inTransitionHookCopy = m_inTransitionHook;
#endif
    }
    if (hookCopy) {
      hookCopy(cmd.requestId);
    }

    // P0-26: Re-verify request ID and commit state atomically under
    // m_trackMutex
    bool committed = false;
    {
      std::lock_guard<std::mutex> lock(m_trackMutex);
      if (cmd.requestId != 0 &&
          cmd.requestId == m_activeRequestId.load(std::memory_order_acquire)) {
        m_openPhase.store(static_cast<uint8_t>(OpenTransitionPhase::StateCommitStart),
                          std::memory_order_release);
        m_workerOpenTelem.stateCommitStartNs = steadyNowNs();
        m_stateTransitionSeq.fetch_add(1, std::memory_order_acq_rel);

        m_workerOpenTelem.playingBefore = m_isPlaying.load(std::memory_order_relaxed);
        m_isPlaying.store(false, std::memory_order_release);
        m_workerOpenTelem.playingAfter = false;
        m_workerOpenTelem.playingClearedNs = steadyNowNs();

        m_ringBuffer.resetProducer(); // P0-04: Producer-owned reset
        m_workerOpenTelem.ringResetNs = steadyNowNs();
        m_pendingOutOffset = 0;
        m_pendingOutCount = 0;
        m_syntheticPos = 0;

        m_workerOpenTelem.eofBefore = m_isEof.load(std::memory_order_relaxed);
        m_isEof.store(false, std::memory_order_release);
        m_workerOpenTelem.eofAfter = false;
        m_workerOpenTelem.eofClearedNs = steadyNowNs();
        m_openPhase.store(
            static_cast<uint8_t>(OpenTransitionPhase::AfterEofClearedBeforePlayCleared),
            std::memory_order_release);

        m_eofDelivered.store(false, std::memory_order_release);
        m_eofEpoch.fetch_add(1, std::memory_order_acq_rel);

#ifdef DESK32_ENABLE_TEST_HOOKS
        if (inTransitionHookCopy) {
          inTransitionHookCopy(cmd.requestId);
        }
#endif

        m_decoder.reset(); // Destroy active decoder exclusively on Worker
                           // Thread (P0-15)
        m_workerOpenTelem.resamplerConfigStartNs = steadyNowNs();
        m_resampler.configure(cmd.sampleRate, cmd.graphRate, 2);
        m_workerOpenTelem.resamplerConfigEndNs = steadyNowNs();
        m_trackInfo = tInfo;
        m_metadata = std::make_shared<metadata::TrackMetadata>(std::move(meta));
        m_isSynthetic.store(true, std::memory_order_release);
        m_graphRate.store(cmd.graphRate, std::memory_order_release);
        m_playhead.store(0, std::memory_order_release);
        m_isOpen.store(true, std::memory_order_release);
        m_openPhase.store(static_cast<uint8_t>(OpenTransitionPhase::AfterPlayCleared),
                          std::memory_order_release);

        m_committedRequestId.store(cmd.requestId, std::memory_order_release);
        m_lastOpenStatus.store(OpenStatus::Success, std::memory_order_release);
        m_stateTransitionSeq.fetch_add(1, std::memory_order_release);
        m_workerOpenTelem.stateCommitEndNs = steadyNowNs();
        committed = true;
      }
    }

    if (!committed) {
      m_openPhase.store(static_cast<uint8_t>(OpenTransitionPhase::Idle),
                        std::memory_order_release);
      if (cmd.openPromise) {
        try {
          cmd.openPromise->set_value(false);
        } catch (...) {
        }
      }
      break;
    }

    m_openPhase.store(static_cast<uint8_t>(OpenTransitionPhase::Prefilling),
                      std::memory_order_release);
    m_workerOpenTelem.prefillStartNs = steadyNowNs();
    for (int i = 0; i < 4; ++i) {
      doDecodeChunk();
    }
    m_workerOpenTelem.prefillEndNs = steadyNowNs();
    m_workerOpenTelem.prefillInputFrames = CHUNK_SIZE * 4;
    m_workerOpenTelem.prefillRingAvailAfter = m_ringBuffer.availableRead();
    m_workerOpenTelem.openCompleteNs = steadyNowNs();
    m_workerOpenTelem.valid = true;
    publishOpenTransition(m_workerOpenTelem);
    m_openPhase.store(static_cast<uint8_t>(OpenTransitionPhase::OpenComplete),
                      std::memory_order_release);

    if (cmd.openPromise) {
      try {
        cmd.openPromise->set_value(true);
      } catch (...) {
      }
    }
    m_seekCompleted.store(cmd.sequence, std::memory_order_release);
    break;
  }
  case WorkerCommandType::CloseTrack: {
    bool skipClose = false;
    {
      std::lock_guard<std::mutex> lock(m_trackMutex);
      uint64_t currentCommitted =
          m_committedRequestId.load(std::memory_order_relaxed);
      if (cmd.targetRequestId != 0 && currentCommitted > cmd.targetRequestId) {
        // A newer OpenTrack was already committed before this old CloseTrack
        // arrived; skip close
        skipClose = true;
      } else {
        m_isOpen.store(false, std::memory_order_release);
        m_isPlaying.store(false, std::memory_order_release);
        m_isEof.store(true, std::memory_order_release);
        m_eofDelivered.store(false, std::memory_order_release);
        m_eofEpoch.fetch_add(1, std::memory_order_acq_rel);
        m_committedRequestId.store(0, std::memory_order_release);
        m_trackInfo = WorkerTrackInfo{};
        m_metadata.reset();
        m_decoder.reset();
        m_resampler.reset();
      }
    }
    if (!skipClose) {
      m_ringBuffer.resetProducer();
      m_pendingOutOffset = 0;
      m_pendingOutCount = 0;
    }
    if (cmd.openPromise) {
      try {
        cmd.openPromise->set_value(true);
      } catch (...) {
      }
    }
    break;
  }
  case WorkerCommandType::Seek: {
    if (!m_isOpen.load(std::memory_order_acquire)) {
      m_seekSuccess.store(false, std::memory_order_release);
      m_seekCompleted.store(cmd.sequence, std::memory_order_release);
      break;
    }
    m_ringBuffer.resetProducer(); // P0-04: Producer-owned reset
    m_pendingOutOffset = 0;
    m_pendingOutCount = 0;
    m_isEof.store(false, std::memory_order_release);
    m_eofDelivered.store(false, std::memory_order_release);
    m_eofEpoch.fetch_add(1, std::memory_order_acq_rel);
    bool ok = false;
    int64_t actual = cmd.targetFrame;
    if (m_isSynthetic.load(std::memory_order_relaxed)) {
      m_syntheticPos = cmd.targetFrame;
      for (int i = 0; i < 4; ++i)
        doDecodeChunk();
      ok = true;
      actual = cmd.targetFrame;
    } else if (m_decoder) {
      ok = m_decoder->seek(cmd.targetFrame);
      m_resampler.reset();
      if (ok) {
        for (int i = 0; i < 4; ++i)
          doDecodeChunk();
        actual = m_decoder->lastSeekActualFrame();
      }
    }
    m_seekActualFrame.store(actual, std::memory_order_release);
    m_seekSuccess.store(ok, std::memory_order_release);
    m_seekCompleted.store(cmd.sequence, std::memory_order_release);
    break;
  }
  case WorkerCommandType::Play:
    break;
  case WorkerCommandType::Pause:
    break;
  case WorkerCommandType::Stop:
    m_ringBuffer.resetProducer(); // P0-04: Producer-owned reset
    m_pendingOutOffset = 0;
    m_pendingOutCount = 0;
    m_isEof.store(false, std::memory_order_release);
    m_eofDelivered.store(false, std::memory_order_release);
    m_eofEpoch.fetch_add(1, std::memory_order_acq_rel);
    m_syntheticPos = 0;
    if (m_decoder) {
      m_decoder->seek(0);
      m_resampler.reset();
    }
    break;
  case WorkerCommandType::ConfigureRate: {
    bool shouldPrefill = false;
    {
      std::lock_guard<std::mutex> lock(m_trackMutex);
      if (m_trackInfo.sampleRate > 0.0 && cmd.graphRate > 0.0) {
        m_resampler.configure(m_trackInfo.sampleRate, cmd.graphRate,
                              m_trackInfo.channels);
        m_ringBuffer.resetProducer();
        m_pendingOutOffset = 0;
        m_pendingOutCount = 0;
        if (m_isOpen.load(std::memory_order_acquire)) {
          m_isEof.store(false, std::memory_order_release);
          m_eofDelivered.store(false, std::memory_order_release);
          m_eofEpoch.fetch_add(1, std::memory_order_acq_rel);
          const int64_t resumeFrame = std::max<int64_t>(0, m_playhead.load(std::memory_order_acquire));
          if (m_isSynthetic.load(std::memory_order_relaxed)) {
            m_syntheticPos = resumeFrame;
            shouldPrefill = true;
          } else if (m_decoder) {
            m_decoder->seek(resumeFrame);
            m_resampler.reset();
            shouldPrefill = true;
          }
        }
      }
    }
    if (shouldPrefill) {
      for (int i = 0; i < 4; ++i) {
        doDecodeChunk();
      }
    }
  } break;
  default:
    break;
  }
}

void DecoderWorker::doDecodeChunk() {
  const int64_t chunkStartNs = steadyNowNs();
  auto recordChunkEnd = [this, chunkStartNs]() noexcept {
    const int64_t endNs = steadyNowNs();
    const uint64_t totalNs =
        static_cast<uint64_t>(std::max<int64_t>(0, endNs - chunkStartNs));
    m_workerPerf.lastChunkTotalNs = totalNs;
    if (totalNs > m_workerPerf.maxChunkTotalNs) {
      m_workerPerf.maxChunkTotalNs = totalNs;
    }
    ++m_workerPerf.totalChunksDecoded;
    publishDecodePerf(m_workerPerf);
  };

  // 1. Drain any pending resampled output frames from previous chunk first
  if (m_pendingOutCount > 0) {
    size_t written = m_ringBuffer.write(
        m_tempInterleavedOut.data() + m_pendingOutOffset, m_pendingOutCount);
    if (written > 0 && m_workerOpenTelem.firstPcmSupplyNs == 0) {
      m_workerOpenTelem.firstPcmSupplyNs = steadyNowNs();
    }
    m_pendingOutOffset += written;
    m_pendingOutCount -= written;
    if (m_pendingOutCount > 0) {
      recordChunkEnd();
      return; // Ring buffer full; resume draining on next loop iteration
    }
    m_pendingOutOffset = 0;
  }

  if (m_isSynthetic.load(std::memory_order_relaxed)) {
    if (m_syntheticPos >= static_cast<int64_t>(m_trackInfo.totalFrames)) {
      m_isEof.store(true, std::memory_order_release);
      recordChunkEnd();
      return;
    }

    size_t framesToGen = std::min(
        CHUNK_SIZE,
        static_cast<size_t>(static_cast<int64_t>(m_trackInfo.totalFrames) -
                            m_syntheticPos));
    for (size_t i = 0; i < framesToGen; ++i) {
      double t = static_cast<double>(m_syntheticPos + static_cast<int64_t>(i)) /
                 m_trackInfo.sampleRate;
      double s = 0.5 * std::sin(2.0 * M_PI * 440.0 * t);
      m_tempChunkL[i] = s;
      m_tempChunkR[i] = s;
    }
    m_syntheticPos += static_cast<int64_t>(framesToGen);

    size_t written = 0;
    while (written < framesToGen && m_running.load(std::memory_order_relaxed)) {
      size_t w = m_ringBuffer.writePlanar(m_tempChunkL.data() + written,
                                          m_tempChunkR.data() + written,
                                          framesToGen - written);
      if (w > 0 && m_workerOpenTelem.firstPcmSupplyNs == 0) {
        m_workerOpenTelem.firstPcmSupplyNs = steadyNowNs();
      }
      written += w;
      if (written < framesToGen) {
        std::unique_lock<std::mutex> lock(m_cmdMutex);
        if (!m_pendingCommands.empty())
          break;
        m_cmdCv.wait_for(lock, std::chrono::milliseconds(2), [this]() {
          return !m_running.load(std::memory_order_relaxed) ||
                 !m_pendingCommands.empty() ||
                 m_ringBuffer.availableWrite() > 0;
        });
      }
    }
    recordChunkEnd();
    return;
  }

  if (!m_decoder || !m_decoder->isOpen()) {
    m_isEof.store(true, std::memory_order_release);
    recordChunkEnd();
    return;
  }

  const int64_t readStartNs = steadyNowNs();
  size_t read = m_decoder->readFrames(m_tempChunkL.data(), m_tempChunkR.data(),
                                      CHUNK_SIZE);
  const int64_t readEndNs = steadyNowNs();
  const uint64_t readNs =
      static_cast<uint64_t>(std::max<int64_t>(0, readEndNs - readStartNs));
  m_workerPerf.lastReadFramesNs = readNs;
  if (readNs > m_workerPerf.maxReadFramesNs) {
    m_workerPerf.maxReadFramesNs = readNs;
  }
  m_workerPerf.lastFileReadPacketNs = m_decoder->lastReadPacketNs();
  if (m_decoder->maxReadPacketNs() > m_workerPerf.maxFileReadPacketNs) {
    m_workerPerf.maxFileReadPacketNs = m_decoder->maxReadPacketNs();
  }

  if (read == 0) {
    m_isEof.store(true, std::memory_order_release);
    recordChunkEnd();
    return;
  }

  if (m_resampler.isBypass()) {
    size_t written = 0;
    while (written < read && m_running.load(std::memory_order_relaxed)) {
      size_t w = m_ringBuffer.writePlanar(m_tempChunkL.data() + written,
                                          m_tempChunkR.data() + written,
                                          read - written);
      if (w > 0 && m_workerOpenTelem.firstPcmSupplyNs == 0) {
        m_workerOpenTelem.firstPcmSupplyNs = steadyNowNs();
      }
      written += w;
      if (written < read) {
        std::unique_lock<std::mutex> lock(m_cmdMutex);
        if (!m_pendingCommands.empty())
          break;
        m_cmdCv.wait_for(lock, std::chrono::milliseconds(2), [this]() {
          return !m_running.load(std::memory_order_relaxed) ||
                 !m_pendingCommands.empty() ||
                 m_ringBuffer.availableWrite() > 0;
        });
      }
    }
  } else {
    // Interleave for SoXR
    for (size_t i = 0; i < read; ++i) {
      m_tempInterleavedIn[i * 2] = m_tempChunkL[i];
      m_tempInterleavedIn[i * 2 + 1] = m_tempChunkR[i];
    }

    size_t inDone = 0, outDone = 0;
    size_t maxOut = CHUNK_SIZE * 8;
    const int64_t resStartNs = steadyNowNs();
    bool ok = m_resampler.process(
        m_tempInterleavedIn.data(), read, inDone,
        reinterpret_cast<double *>(m_tempInterleavedOut.data()), maxOut,
        outDone);
    const int64_t resEndNs = steadyNowNs();
    const uint64_t resNs =
        static_cast<uint64_t>(std::max<int64_t>(0, resEndNs - resStartNs));
    m_workerPerf.lastResampleNs = resNs;
    if (resNs > m_workerPerf.maxResampleNs) {
      m_workerPerf.maxResampleNs = resNs;
    }
    if (ok && outDone > 0) {
      size_t written = m_ringBuffer.write(m_tempInterleavedOut.data(), outDone);
      if (written > 0 && m_workerOpenTelem.firstPcmSupplyNs == 0) {
        m_workerOpenTelem.firstPcmSupplyNs = steadyNowNs();
      }
      if (written < outDone) {
        m_pendingOutOffset = written;
        m_pendingOutCount = outDone - written;
      }
    }
  }
  recordChunkEnd();
}

} // namespace desk32::engine::decoder
