#pragma once

#include "zjson.h"

#include <string>
#include <vector>

namespace nim {

// Port of legacy `ZRunNeuTuCommand::runGeneral` / `runTraceCommand` for the
// `"command":"trace_neuron"` module.
//
// Strict parity goals:
// - Same input/command validation and return-code semantics as legacy.
// - Same trace-config resolution (`path`, `"default"`, and command_config include fallback).
// - Same optional predefined-mask behavior via `inputJson` (`mask`, `maskThreshold`).
int runGeneral(const std::string& generalConfigTextOrPath,
               const json::object& generalCfg,
               const json::object& inputJson,
               const std::vector<std::string>& positionalInput,
               const std::string& outputPath,
               int level,
               bool diagnosis,
               const std::string& traceIncludePath,
               const std::string& jsonDirPath,
               bool verbose);

} // namespace nim
