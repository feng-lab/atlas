#pragma once

#include <cstdint>
#include <map>
#include <unordered_map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nim {

// Lightweight, lock-free (single-threaded) perf collector.
// Aggregates per-submission CPU/GPU scopes under a user-visible frame token
// and emits a single summary when a token is safe to flush.
class Z3DPerfCollector
{
public:
  struct Scope
  {
    std::string label;
    double ms = 0.0;
    // Optional: calibrated CPU-axis timestamp in microseconds (relative baseline handled at writer)
    double tsUs = -1.0;
    bool isPassScope = false; // true for top-level pass scopes (non-nested "frame budget" scopes)
  };

  struct Stats
  {
    // Work submitted
    uint32_t drawsSubmitted = 0;
    uint32_t fenceWaits = 0;

    // Descriptor/arena
    uint32_t descriptorSetsAllocated = 0;
    uint32_t pipelinesCreated = 0;
    uint32_t pipelinesBoundCount = 0;
    uint32_t renderingSegmentsBegan = 0;
    uint32_t attachmentClears = 0;
    uint32_t attachmentLoads = 0;
    uint32_t descriptorWritesWhileRecording = 0;
    uint32_t boundSetRewriteAttempts = 0;

    // Upload/readback/staging
    size_t uploadHighWatermarkBytes = 0; // max within submission
    size_t uniformHighWatermarkBytes = 0; // max within submission
    size_t staticBytesStaged = 0;
    size_t linesBytesStaged = 0;
    size_t fontsBytesStaged = 0;
    size_t meshesBytesStaged = 0;
    size_t spheresBytesStaged = 0;
    size_t conesBytesStaged = 0;
    size_t ellipsoidsBytesStaged = 0;
    size_t readbackBytesCopied = 0;
    uint32_t readbackSlotsInFlight = 0;
    // Perf-frame-start → first Vulkan submission CPU-scope start.
    // Populated per submission; the perf collector reports the earliest
    // submission's value as "pre_cpu" in the per-frame summary line.
    uint32_t preCpuStartSamples = 0;
    double preCpuStartMs = 0.0;
    uint32_t allSamples = 0; // perf-frame-start → host-ready observations (end-to-end UX latency)
    double allMaxMs = 0.0; // max over allSamples

    // Optional: command buffer reuse diagnostics
    uint32_t drawSecondaryCacheAttempts = 0;
    uint32_t drawSecondaryCacheKeyFound = 0;
    uint32_t drawSecondaryCacheSignatureMismatches = 0;
    uint32_t drawSecondaryCacheSignatureMismatchMaskOr = 0;
    uint32_t drawSecondaryCacheHits = 0;
    uint32_t drawSecondaryCacheBuilds = 0;
    uint32_t drawSecondaryCacheExecutes = 0;

    // -----------------------------------------------------------------------
    // Vulkan CPU overhead attribution (per-submission)
    // -----------------------------------------------------------------------
    // Time spent inside Z3DRendererVulkanBackend::beginRender() before the
    // backend starts its measured CPU encode window (cpuStart).
    //
    // This is a subset of "pre_cpu" (perf-frame-start → cpuStart) and is useful
    // for separating engine/script overhead from backend preamble work (frame
    // slot acquisition, safe-point pumping, arena setup, etc).
    double vkBeginRenderPreambleMs = 0.0;

    // Script/build overhead that occurs before beginRender() opens a submission.
    // These values are measured by ZVulkanLinearScript and forwarded to the
    // backend for inclusion in the per-frame stats line.
    double scriptUniformHintMs = 0.0;
    size_t scriptUniformHintBytes = 0;
    uint32_t scriptNodeCount = 0;
    uint32_t scriptRasterNodeCount = 0;
    uint32_t scriptReplayNodeCount = 0;
    uint32_t scriptCommandsNodeCount = 0;
    uint32_t scriptPreRecordNodeCount = 0;
    uint32_t scriptBatchCount = 0;
  };

  struct Submission
  {
    uint32_t submissionId = 0; // ordering within a token
    double cpuMs = 0.0; // CPU encode time for this submission
    std::vector<Scope> gpuScopes; // per-pass GPU scopes
    std::vector<Scope> cpuScopes; // optional CPU micro-scopes
    Stats stats; // lightweight counters for diagnostics
  };

  static Z3DPerfCollector& instance();

  // `off` disables performance-token registration, CPU/GPU scopes, and
  // collector ingestion; functional render-frame identity remains active.
  // `light` keeps top-level submission/pass metrics; `full` additionally
  // enables nested GPU scopes recorded inside those passes.
  [[nodiscard]] static bool enabled();
  [[nodiscard]] static bool nestedGpuScopesEnabled();

  // Ingest a submission's scopes under a token.
  void addSubmission(uint64_t token,
                     uint32_t submissionId,
                     double cpuMs,
                     std::vector<Scope> gpuScopes,
                     std::vector<Scope> cpuScopes,
                     Stats stats);

  // Mark a token closed at the end of a rendering pass (one engine-driven
  // filter pipeline evaluation).
  // Actual flush happens once all submissions that were started for this token
  // have been ingested (may be later; Vulkan submissions for different tokens
  // can be interleaved).
  void markClosed(uint64_t token);

  // Tell the collector that a submission has been started for this token.
  // This must be called once per actual GPU submission. The collector uses the
  // count of started submissions as the expected ingestion count for deciding
  // when a token is safe to flush.
  void noteSubmissionStarted(uint64_t token, uint32_t submissionId);

  // Attempt to flush any closed tokens that are now safe. If force is true,
  // flush all closed tokens regardless (used during backend/device switches).
  void maybeFlush(bool force = false);

private:
  struct TokenData
  {
    std::vector<Submission> submissions;
    uint32_t startedSubmissions = 0; // count of submissions started (expected ingestions) for this token
    bool closed = false;
    bool flushed = false;
  };

  std::map<uint64_t, TokenData> m_tokens; // ordered by token

  // Rolling statistics for diagnostics
  std::unordered_map<std::string, std::vector<double>> m_gpuLabelHistory; // last N values per label
  std::vector<double> m_totalGpuHistory;
  std::vector<double> m_totalCpuHistory;
  static constexpr size_t kHistory = 120; // frames

  void flush(uint64_t token);
};

} // namespace nim
