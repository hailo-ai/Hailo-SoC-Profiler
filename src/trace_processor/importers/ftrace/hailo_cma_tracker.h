/*
 * Copyright (c) 2026 Hailo Technologies Ltd. All rights reserved.
 */

 #ifndef SRC_TRACE_PROCESSOR_IMPORTERS_FTRACE_HAILO_CMA_TRACKER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_FTRACE_HAILO_CMA_TRACKER_H_

#include <stdint.h>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>

#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/ftrace/hailo_memory_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/destructible.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {

class HailoCmaTracker : public Destructible, public MemoryTrackerExtension {
public:
    explicit HailoCmaTracker(TraceProcessorContext* context);
    ~HailoCmaTracker() = default;

    HailoCmaTracker(const HailoCmaTracker&) = delete;
    HailoCmaTracker& operator=(const HailoCmaTracker&) = delete;

    static HailoCmaTracker* GetOrCreate(TraceProcessorContext* context);

    // MemoryTrackerExtension interface implementation
    void ParseEvent(int64_t timestamp,
                   base::StringView event_name,
                   const std::unordered_map<std::string, StringId>& string_fields,
                   const std::unordered_map<std::string, uint64_t>& uint_fields) override;

private:
    // Internal method to parse CMA heap stats event
    void ParseCmaHeapStats(int64_t timestamp,
                           StringId heap_name,
                           uint64_t total_pages,
                           uint64_t used_pages,
                           uint64_t free_pages,
                           uint64_t max_chunk_pages);
    TraceProcessorContext* context_;
    
    // Track which parent tracks have been initialized (to avoid duplicate dummy counters)
    std::mutex heap_stats_parent_initialized_mutex_;
    std::set<TrackId> heap_stats_parent_initialized_;
};

} // namespace trace_processor
} // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_FTRACE_HAILO_CMA_TRACKER_H_

