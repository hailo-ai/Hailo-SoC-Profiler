/*
 * Copyright (c) 2025 Hailo Technologies Ltd. All rights reserved.
 */
#ifndef SRC_PROFILING_PERF_HAILO_NOC_H_
#define SRC_PROFILING_PERF_HAILO_NOC_H_

#include "src/profiling/perf/perf_producer.h"
#include "src/profiling/perf/event_config.h"
#include "protos/perfetto/common/perf_events.gen.h"
#include <optional>
#include <string>

namespace perfetto::profiling {

std::optional<uint32_t> GetHailoNocPerfEventType();

void EmitHailoNocAuxSample(TraceWriter* trace_writer,
                           const EventConfig& event_config,
                           std::vector<char> aux);

// Extract parameters from HailoNocEvent message and create a PerfCounter
PerfCounter HailoNocEventToPerfCounter(
    const protos::gen::PerfEvents::HailoNocEvent& noc_event);

// Extract parameters from HailoNocCounter message and create a PerfCounter
PerfCounter HailoNocCounterToPerfCounter(
    const std::string& name,
    const protos::gen::PerfEvents_HailoNocCounterFollowerEvent& hailo_noc_counter);

// Perform any necessary preconfiguration for Hailo NOC events before opening the perf event
bool PreconfigureHailoNocEvent(const EventConfig& event_cfg);

}  // namespace perfetto::profiling

#endif  // SRC_PROFILING_PERF_HAILO_NOC_H_
