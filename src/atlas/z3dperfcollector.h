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
    // Descriptor/arena
    uint32_t descriptorSetsAllocated = 0;
    uint32_t overrideSetsAllocated = 0;
    uint32_t pipelinesCreated = 0;
    uint32_t pipelinesBoundCount = 0;
    uint32_t renderingSegmentsBegan = 0;
    uint32_t attachmentClears = 0;
    uint32_t attachmentLoads = 0;
    uint32_t descriptorWritesWhileRecording = 0;
    uint32_t boundSetRewriteAttempts = 0;

    // Upload/readback/staging
    size_t uploadHighWatermarkBytes = 0; // max within submission
    size_t staticBytesStaged = 0;
    size_t linesBytesStaged = 0;
    size_t fontsBytesStaged = 0;
    size_t meshesBytesStaged = 0;
    size_t spheresBytesStaged = 0;
    size_t readbackBytesCopied = 0;
    uint32_t readbackSlotsInFlight = 0;
    uint32_t allSamples = 0; // perf-frame-start → host-ready observations (end-to-end UX latency)
    double allMaxMs = 0.0; // max over allSamples
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

  // Ingest a submission's scopes under a token.
  void addSubmission(uint64_t token,
                     uint32_t submissionId,
                     double cpuMs,
                     std::vector<Scope> gpuScopes,
                     std::vector<Scope> cpuScopes,
                     Stats stats);

  // Mark a token closed at the end of a rendering pass (one engine-driven
  // filter pipeline evaluation).
  // Actual flush happens once the last submission has been ingested (typically
  // on the first begin of the next token).
  void markClosed(uint64_t token);

  // Attempt to flush any closed tokens that are now safe. If force is true,
  // flush all closed tokens regardless (used during backend/device switches).
  void maybeFlush(bool force = false);

private:
  struct TokenData
  {
    std::vector<Submission> submissions;
    bool closed = false;
    bool flushed = false;
  };

  std::map<uint64_t, TokenData> m_tokens; // ordered by token
  uint64_t m_maxSeenToken = 0; // largest token observed via addSubmission

  // Rolling statistics for diagnostics
  std::unordered_map<std::string, std::vector<double>> m_gpuLabelHistory; // last N values per label
  std::vector<double> m_totalGpuHistory;
  std::vector<double> m_totalCpuHistory;
  static constexpr size_t kHistory = 120; // frames

  void flush(uint64_t token);
};

} // namespace nim
