#pragma once

#include "zjson.h"

#include <QString>

#include <vector>

namespace nim {

// Port of legacy `ZRunNeuTuCommand::runGeneral` / `runTraceCommand` for the
// `"command":"trace_neuron"` module.
//
// Strict parity goals:
// - Same input/command validation and return-code semantics as legacy.
// - Same trace-config resolution (`path`, `"default"`, and command_config include fallback).
// - Same optional predefined-mask behavior via `inputJson` (`mask`, `maskThreshold`).
int runGeneral(const QString& generalConfigTextOrPath,
               const json::object& generalCfg,
               const json::object& inputJson,
               const std::vector<QString>& positionalInput,
               const QString& outputPath,
               int level,
               bool diagnosis,
               const QString& traceIncludePath,
               const QString& jsonDirPath,
               bool verbose);

} // namespace nim
