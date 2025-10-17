#include "z3dperfcollector.h"

#include "zlog.h"
#include <gflags/gflags.h>

#include <algorithm>
#include <fstream>
#include <fmt/format.h>
#include <limits>
#include <unordered_map>
#include "zjson.h"

DEFINE_string(atlas_perf_mode, "light", "Perf mode: off|light|full");
DEFINE_string(atlas_perf_trace,
              "",
              "If non-empty, write Chrome trace JSON for each flushed frame to this path (overwrites)");
DEFINE_string(atlas_perf_trace_append,
              "",
              "If non-empty, append this frame's Chrome trace events into the given file (accumulates frames)");
DEFINE_string(atlas_perf_summary,
              "",
              "Summary export. Format 'csv:path' or 'json:path'. Appends per-frame totals, top labels, and stats");
DEFINE_bool(atlas_perf_trace_calibrated,
            false,
            "If true and supported, align GPU events to CPU time axis in trace output (best-effort)");

namespace nim {

Z3DPerfCollector& Z3DPerfCollector::instance()
{
  static Z3DPerfCollector inst;
  return inst;
}

void Z3DPerfCollector::addSubmission(uint64_t token,
                                     uint32_t submissionId,
                                     double cpuMs,
                                     std::vector<Scope> gpuScopes,
                                     std::vector<Scope> cpuScopes,
                                     Stats stats)
{
  m_maxSeenToken = std::max(m_maxSeenToken, token);
  auto& td = m_tokens[token];
  td.submissions.push_back(Submission{.submissionId = submissionId,
                                      .cpuMs = cpuMs,
                                      .gpuScopes = std::move(gpuScopes),
                                      .cpuScopes = std::move(cpuScopes),
                                      .stats = std::move(stats)});

  // Keep submissions ordered for readable summaries
  std::sort(td.submissions.begin(), td.submissions.end(), [](const Submission& a, const Submission& b) {
    return a.submissionId < b.submissionId;
  });

  // Flush any earlier closed tokens now that we know we've advanced to (or past) this token.
  maybeFlush(false);
}

void Z3DPerfCollector::markClosed(uint64_t token)
{
  auto it = m_tokens.find(token);
  if (it == m_tokens.end()) {
    // Create and mark closed; ingestion will arrive later.
    TokenData td;
    td.closed = true;
    m_tokens.emplace(token, std::move(td));
  } else {
    it->second.closed = true;
  }
}

void Z3DPerfCollector::maybeFlush(bool force)
{
  if (m_tokens.empty()) {
    return;
  }
  // Tokens strictly less than maxSeenToken are safe to flush if closed.
  const uint64_t safeBefore = force ? std::numeric_limits<uint64_t>::max() : (m_maxSeenToken);
  for (auto& [tok, td] : m_tokens) {
    if (td.flushed) {
      continue;
    }
    if (!td.closed) {
      continue;
    }
    if (tok < safeBefore || force) {
      flush(tok);
      td.flushed = true;
    }
  }

  // Optionally prune flushed tokens to keep memory bounded.
  for (auto it = m_tokens.begin(); it != m_tokens.end();) {
    if (it->second.flushed) {
      it = m_tokens.erase(it);
    } else {
      ++it;
    }
  }
}

void Z3DPerfCollector::flush(uint64_t token)
{
  auto it = m_tokens.find(token);
  if (it == m_tokens.end()) {
    return;
  }
  const auto& td = it->second;
  if (td.submissions.empty()) {
    return;
  }

  double totalCpuMs = 0.0;
  std::unordered_map<std::string, double> gpuByLabel;
  std::unordered_map<std::string, double> cpuByLabel;
  // Aggregate stats across submissions
  Stats agg{};

  for (const auto& sub : td.submissions) {
    totalCpuMs += sub.cpuMs;
    for (const auto& s : sub.gpuScopes) {
      gpuByLabel[s.label] += s.ms;
    }
    for (const auto& s : sub.cpuScopes) {
      cpuByLabel[s.label] += s.ms;
    }
    agg.descriptorSetsAllocated += sub.stats.descriptorSetsAllocated;
    agg.overrideSetsAllocated += sub.stats.overrideSetsAllocated;
    agg.pipelinesCreated += sub.stats.pipelinesCreated;
    agg.pipelinesBoundCount += sub.stats.pipelinesBoundCount;
    agg.renderingSegmentsBegan += sub.stats.renderingSegmentsBegan;
    agg.attachmentClears += sub.stats.attachmentClears;
    agg.attachmentLoads += sub.stats.attachmentLoads;
    agg.descriptorWritesWhileRecording += sub.stats.descriptorWritesWhileRecording;
    agg.boundSetRewriteAttempts += sub.stats.boundSetRewriteAttempts;
    agg.uploadHighWatermarkBytes = std::max(agg.uploadHighWatermarkBytes, sub.stats.uploadHighWatermarkBytes);
    agg.staticBytesStaged += sub.stats.staticBytesStaged;
    agg.linesBytesStaged += sub.stats.linesBytesStaged;
    agg.fontsBytesStaged += sub.stats.fontsBytesStaged;
    agg.meshesBytesStaged += sub.stats.meshesBytesStaged;
    agg.spheresBytesStaged += sub.stats.spheresBytesStaged;
    agg.readbackBytesCopied += sub.stats.readbackBytesCopied;
    agg.readbackSlotsInFlight += sub.stats.readbackSlotsInFlight;
  }

  // Compute total GPU ms and top contributors
  double totalGpuMs = 0.0;
  for (const auto& kv : gpuByLabel) {
    totalGpuMs += kv.second;
  }

  // Sort by contribution
  std::vector<std::pair<std::string, double>> sortedGpu(gpuByLabel.begin(), gpuByLabel.end());
  std::sort(sortedGpu.begin(), sortedGpu.end(), [](const auto& a, const auto& b) {
    return a.second > b.second;
  });
  if (sortedGpu.size() > 8) {
    sortedGpu.resize(8);
  }

  std::string msg = fmt::format("Frame#{} CPU {:.3f} ms, GPU {:.3f} ms", token, totalCpuMs, totalGpuMs);
  for (const auto& [label, ms] : sortedGpu) {
    double pct = (totalGpuMs > 0.0) ? (ms * 100.0 / totalGpuMs) : 0.0;
    msg += fmt::format(" | {} {:.3f} ms ({:.0f}%)", label, ms, pct);
  }

  // Rolling totals and jank detection
  auto pushHist = [](std::vector<double>& v, double x) {
    if (v.size() >= kHistory) {
      v.erase(v.begin());
    }
    v.push_back(x);
  };
  pushHist(m_totalGpuHistory, totalGpuMs);
  pushHist(m_totalCpuHistory, totalCpuMs);

  auto percentile = [](std::vector<double> v, double p) -> double {
    if (v.empty()) {
      return 0.0;
    }
    std::sort(v.begin(), v.end());
    double idx = p * (v.size() - 1);
    size_t lo = static_cast<size_t>(std::floor(idx));
    size_t hi = static_cast<size_t>(std::ceil(idx));
    if (hi == lo) {
      return v[lo];
    }
    double f = idx - lo;
    return v[lo] * (1.0 - f) + v[hi] * f;
  };

  double p95Gpu = percentile(m_totalGpuHistory, 0.95);
  double p95Cpu = percentile(m_totalCpuHistory, 0.95);
  bool jankGpu = (m_totalGpuHistory.size() >= 20) && (totalGpuMs > p95Gpu * 1.5);
  bool jankCpu = (m_totalCpuHistory.size() >= 20) && (totalCpuMs > p95Cpu * 1.5);
  if (p95Gpu > 0.0 || p95Cpu > 0.0) {
    msg += fmt::format(" [p95 gpu={:.3f} ms cpu={:.3f} ms]", p95Gpu, p95Cpu);
  }
  if (jankGpu || jankCpu) {
    msg += " JANK";
  }

  VLOG(1) << msg;

  // Stats line (concise)
  std::string stats = fmt::format(
    "Stats: upload_hi={}B static_staged={}B rb={}B dsets={} ovsets={} pipes+={} bound={} segs={} clr={} ld={} dwr={} rew={}",
    agg.uploadHighWatermarkBytes,
    agg.staticBytesStaged,
    agg.readbackBytesCopied,
    agg.descriptorSetsAllocated,
    agg.overrideSetsAllocated,
    agg.pipelinesCreated,
    agg.pipelinesBoundCount,
    agg.renderingSegmentsBegan,
    agg.attachmentClears,
    agg.attachmentLoads,
    agg.descriptorWritesWhileRecording,
    agg.boundSetRewriteAttempts);
  VLOG(1) << stats;

  // Optional: emit a Chrome trace JSON for this frame
  if (!FLAGS_atlas_perf_trace.empty()) {
    struct Ev
    {
      std::string name;
      const char* tid; // lane name
      double tsUs; // start time in microseconds
      double durUs; // duration in microseconds
    };

    std::vector<Ev> events;
    bool calibrated = FLAGS_atlas_perf_trace_calibrated;
    double minGpuTs = std::numeric_limits<double>::infinity();
    if (calibrated) {
      for (const auto& sub : td.submissions) {
        for (const auto& s : sub.gpuScopes) {
          if (s.tsUs >= 0.0) {
            minGpuTs = std::min(minGpuTs, s.tsUs);
          }
        }
      }
      if (!std::isfinite(minGpuTs)) {
        calibrated = false;
      }
    }
    double cpuTs = calibrated ? minGpuTs : 0.0;
    for (const auto& sub : td.submissions) {
      if (sub.cpuMs > 0.0) {
        events.push_back(
          Ev{fmt::format("submit.{}.encode", sub.submissionId), "Render Thread", cpuTs, sub.cpuMs * 1000.0});
        cpuTs += sub.cpuMs * 1000.0;
      }
      for (const auto& s : sub.cpuScopes) {
        if (s.ms <= 0.0) {
          continue;
        }
        events.push_back(Ev{s.label, "Render Thread", cpuTs, s.ms * 1000.0});
        cpuTs += s.ms * 1000.0;
      }
      for (const auto& s : sub.gpuScopes) {
        if (s.ms <= 0.0) {
          continue;
        }
        double ts = calibrated ? s.tsUs : 0.0;
        events.push_back(Ev{s.label, "GPU Queue", ts, s.ms * 1000.0});
      }
    }
    double minTs = std::numeric_limits<double>::infinity();
    for (const auto& e : events) {
      minTs = std::min(minTs, e.tsUs);
    }
    if (std::isfinite(minTs) && minTs != 0.0) {
      for (auto& e : events) {
        e.tsUs -= minTs;
      }
    }

    try {
      std::ofstream ofs(FLAGS_atlas_perf_trace, std::ios::out | std::ios::trunc);
      ofs << "{\n\"traceEvents\":[\n";
      for (size_t i = 0; i < events.size(); ++i) {
        const auto& e = events[i];
        ofs << fmt::format("{{\"name\":\"{}\",\"ph\":\"X\",\"ts\":{},\"dur\":{},\"pid\":1,\"tid\":\"{}\"}}{}\n",
                           e.name,
                           static_cast<long long>(e.tsUs),
                           static_cast<long long>(e.durUs),
                           e.tid,
                           (i + 1 < events.size()) ? "," : "");
      }
      ofs << "]\n}\n";
      ofs.close();
      VLOG(1) << "Wrote Chrome trace to " << FLAGS_atlas_perf_trace;
    }
    catch (const std::exception& ex) {
      LOG(ERROR) << "Failed to write perf trace: " << ex.what();
    }
  }

  // Optional: append this frame's trace events to a multi-frame Chrome trace file
  if (!FLAGS_atlas_perf_trace_append.empty()) {
    // Build events as above
    struct Ev
    {
      std::string name;
      const char* tid;
      double tsUs;
      double durUs;
    };
    std::vector<Ev> events;
    bool calibrated2 = FLAGS_atlas_perf_trace_calibrated;
    double minGpuTs2 = std::numeric_limits<double>::infinity();
    if (calibrated2) {
      for (const auto& sub : td.submissions) {
        for (const auto& s : sub.gpuScopes) {
          if (s.tsUs >= 0.0) {
            minGpuTs2 = std::min(minGpuTs2, s.tsUs);
          }
        }
      }
      if (!std::isfinite(minGpuTs2)) {
        calibrated2 = false;
      }
    }
    double cpuTs = calibrated2 ? minGpuTs2 : 0.0;
    events.push_back(Ev{fmt::format("frame#{}", token), "Render Thread", cpuTs, 0.0});
    for (const auto& sub : td.submissions) {
      if (sub.cpuMs > 0.0) {
        events.push_back(
          Ev{fmt::format("submit.{}.encode", sub.submissionId), "Render Thread", cpuTs, sub.cpuMs * 1000.0});
        cpuTs += sub.cpuMs * 1000.0;
      }
      for (const auto& s : sub.cpuScopes) {
        if (s.ms <= 0.0) {
          continue;
        }
        events.push_back(Ev{s.label, "Render Thread", cpuTs, s.ms * 1000.0});
        cpuTs += s.ms * 1000.0;
      }
      for (const auto& s : sub.gpuScopes) {
        if (s.ms <= 0.0) {
          continue;
        }
        double ts = calibrated2 ? s.tsUs : 0.0;
        events.push_back(Ev{s.label, "GPU Queue", ts, s.ms * 1000.0});
      }
    }
    double minTs2 = std::numeric_limits<double>::infinity();
    for (const auto& e : events) {
      minTs2 = std::min(minTs2, e.tsUs);
    }
    if (std::isfinite(minTs2) && minTs2 != 0.0) {
      for (auto& e : events) {
        e.tsUs -= minTs2;
      }
    }

    try {
      // Append by reopening and inserting before closing ]}
      std::ifstream ifs(FLAGS_atlas_perf_trace_append);
      std::string existing;
      if (ifs.good()) {
        existing.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
      }
      ifs.close();

      std::ofstream ofs(FLAGS_atlas_perf_trace_append, std::ios::out | std::ios::trunc);
      if (!ofs.good()) {
        throw std::runtime_error("Cannot open trace append file for write");
      }
      if (existing.empty()) {
        ofs << "{\n\"traceEvents\":[\n";
        for (size_t i = 0; i < events.size(); ++i) {
          const auto& e = events[i];
          ofs << fmt::format("{{\"name\":\"{}\",\"ph\":\"X\",\"ts\":{},\"dur\":{},\"pid\":1,\"tid\":\"{}\"}}{}\n",
                             e.name,
                             static_cast<long long>(e.tsUs),
                             static_cast<long long>(e.durUs),
                             e.tid,
                             (i + 1 < events.size()) ? "," : "");
        }
        ofs << "]\n}\n";
      } else {
        // Insert new events before the final ]}
        auto pos = existing.rfind("]");
        if (pos == std::string::npos) {
          // Fallback: rewrite entire file
          ofs << "{\n\"traceEvents\":[\n";
          // no comma before first
          for (size_t i = 0; i < events.size(); ++i) {
            const auto& e = events[i];
            ofs << fmt::format("{{\"name\":\"{}\",\"ph\":\"X\",\"ts\":{},\"dur\":{},\"pid\":1,\"tid\":\"{}\"}}{}\n",
                               e.name,
                               static_cast<long long>(e.tsUs),
                               static_cast<long long>(e.durUs),
                               e.tid,
                               (i + 1 < events.size()) ? "," : "");
          }
          ofs << "]\n}\n";
        } else {
          // Determine if there's already at least one event; check if char before ']' is not '['
          bool needComma = true;
          auto bracketPos = existing.find("[", existing.find("traceEvents"));
          if (bracketPos != std::string::npos) {
            // if nothing between [ and ], then no preceding events
            std::string between = existing.substr(bracketPos + 1, pos - bracketPos - 1);
            // trim spaces/newlines
            bool hasContent = false;
            for (char c : between) {
              if (!std::isspace(static_cast<unsigned char>(c))) {
                hasContent = true;
                break;
              }
            }
            needComma = hasContent;
          }
          // Write up to before ']' (assuming trailing "\n}\n" after ])
          ofs << existing.substr(0, pos);
          if (needComma) {
            ofs << ",\n";
          }
          for (size_t i = 0; i < events.size(); ++i) {
            const auto& e = events[i];
            ofs << fmt::format("{{\"name\":\"{}\",\"ph\":\"X\",\"ts\":{},\"dur\":{},\"pid\":1,\"tid\":\"{}\"}}{}\n",
                               e.name,
                               static_cast<long long>(e.tsUs),
                               static_cast<long long>(e.durUs),
                               e.tid,
                               (i + 1 < events.size()) ? "," : "");
          }
          // Add rest of file from ']' onwards
          ofs << existing.substr(pos);
        }
      }
      ofs.close();
      VLOG(1) << "Appended Chrome trace events to " << FLAGS_atlas_perf_trace_append;
    }
    catch (const std::exception& ex) {
      LOG(ERROR) << "Failed to append perf trace: " << ex.what();
    }
  }

  // Optional: per-frame summary export (CSV/JSON)
  if (!FLAGS_atlas_perf_summary.empty()) {
    try {
      // parse format:path
      const auto sep = FLAGS_atlas_perf_summary.find(":");
      if (sep == std::string::npos) {
        throw std::runtime_error("atlas_perf_summary must be 'csv:path' or 'json:path'");
      }
      const std::string fmtKind = FLAGS_atlas_perf_summary.substr(0, sep);
      const std::string outPath = FLAGS_atlas_perf_summary.substr(sep + 1);
      if (fmtKind == "csv") {
        // Append CSV with header if file new. Top5 labels for stable columns.
        std::ifstream ifs(outPath);
        const bool exists = ifs.good();
        ifs.close();
        std::ofstream ofs(outPath, std::ios::out | std::ios::app);
        if (!exists) {
          ofs
            << "frame,cpu_ms,gpu_ms,top1_label,top1_ms,top1_pct,top2_label,top2_ms,top2_pct,top3_label,top3_ms,top3_pct,top4_label,top4_ms,top4_pct,top5_label,top5_ms,top5_pct,upload_hi,static_staged,readback,dsets,ovsets,pipes_created,pipes_bound,segs,clears,loads,dwr,rew\n";
        }
        auto getTop = [&](size_t i) {
          if (i < sortedGpu.size()) {
            return sortedGpu[i];
          }
          return std::pair<std::string, double>{"", 0.0};
        };
        auto t1 = getTop(0), t2 = getTop(1), t3 = getTop(2), t4 = getTop(3), t5 = getTop(4);
        auto pct = [&](double ms) {
          return (totalGpuMs > 0.0) ? (ms * 100.0 / totalGpuMs) : 0.0;
        };
        ofs << fmt::format(
          "{}, {:.3f}, {:.3f}, {}, {:.3f}, {:.0f}, {}, {:.3f}, {:.0f}, {}, {:.3f}, {:.0f}, {}, {:.3f}, {:.0f}, {}, {:.3f}, {:.0f}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}\n",
          token,
          totalCpuMs,
          totalGpuMs,
          t1.first,
          t1.second,
          pct(t1.second),
          t2.first,
          t2.second,
          pct(t2.second),
          t3.first,
          t3.second,
          pct(t3.second),
          t4.first,
          t4.second,
          pct(t4.second),
          t5.first,
          t5.second,
          pct(t5.second),
          agg.uploadHighWatermarkBytes,
          agg.staticBytesStaged,
          agg.readbackBytesCopied,
          agg.descriptorSetsAllocated,
          agg.overrideSetsAllocated,
          agg.pipelinesCreated,
          agg.pipelinesBoundCount,
          agg.renderingSegmentsBegan,
          agg.attachmentClears,
          agg.attachmentLoads,
          agg.descriptorWritesWhileRecording,
          agg.boundSetRewriteAttempts);
        ofs.close();
      } else if (fmtKind == "json") {
        json::object jo;
        jo["frame"] = token;
        jo["cpu_ms"] = totalCpuMs;
        jo["gpu_ms"] = totalGpuMs;
        json::array tops;
        for (const auto& [label, ms] : sortedGpu) {
          json::object t;
          t["label"] = label;
          t["ms"] = ms;
          t["pct"] = (totalGpuMs > 0.0) ? (ms * 100.0 / totalGpuMs) : 0.0;
          tops.push_back(std::move(t));
        }
        jo["top"] = std::move(tops);
        json::object st;
        st["upload_hi"] = agg.uploadHighWatermarkBytes;
        st["static_staged"] = agg.staticBytesStaged;
        st["readback"] = agg.readbackBytesCopied;
        st["descriptor_sets"] = agg.descriptorSetsAllocated;
        st["override_sets"] = agg.overrideSetsAllocated;
        st["pipelines_created"] = agg.pipelinesCreated;
        st["pipelines_bound"] = agg.pipelinesBoundCount;
        st["segments"] = agg.renderingSegmentsBegan;
        st["clears"] = agg.attachmentClears;
        st["loads"] = agg.attachmentLoads;
        st["descriptor_writes_recording"] = agg.descriptorWritesWhileRecording;
        st["bound_set_rewrites"] = agg.boundSetRewriteAttempts;
        jo["stats"] = std::move(st);
        // Append NDJSON: one JSON object per line
        std::ofstream ofs(outPath, std::ios::out | std::ios::app);
        ofs << jsonToString(jo) << "\n";
        ofs.close();
      } else {
        throw std::runtime_error("atlas_perf_summary format must be 'csv' or 'json'");
      }
    }
    catch (const std::exception& ex) {
      LOG(ERROR) << "Failed to write perf summary: " << ex.what();
    }
  }
}

} // namespace nim
