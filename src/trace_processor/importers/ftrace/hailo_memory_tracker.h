/*
 * Copyright (c) 2026 Hailo Technologies Ltd. All rights reserved.
 */

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_FTRACE_HAILO_MEMORY_TRACKER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_FTRACE_HAILO_MEMORY_TRACKER_H_

#include <stdint.h>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "perfetto/ext/base/string_utils.h"
#include "perfetto/protozero/proto_decoder.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/generic.pbzero.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/ftrace/ftrace_descriptors.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/destructible.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {

// Interface for tracker-specific parsers that extend HailoMemoryTracker
class MemoryTrackerExtension {
public:
    virtual ~MemoryTrackerExtension() = default;
    
    // Generic method to parse memory tracker events
    // Extensions can decide which events they handle
    virtual void ParseEvent(int64_t timestamp,
                           base::StringView event_name,
                           const std::unordered_map<std::string, StringId>& string_fields,
                           const std::unordered_map<std::string, uint64_t>& uint_fields) = 0;
};

class HailoMemoryTracker : public Destructible {
public:
    explicit HailoMemoryTracker(TraceProcessorContext* context);
    ~HailoMemoryTracker() = default;

    HailoMemoryTracker(const HailoMemoryTracker&) = delete;
    HailoMemoryTracker& operator=(const HailoMemoryTracker&) = delete;

    static HailoMemoryTracker* GetOrCreate(TraceProcessorContext* context);

    // Parse mem_tracker_event
    void ParseMemTrackerEvent(int64_t timestamp,
                              uint32_t pid,
                              StringId tracker_name,
                              StringId event_type,
                              uint64_t address,
                              uint64_t size,
                              uint64_t allocation_uid,
                              StringId process_name,
                              std::optional<int32_t> tracepoint_pid,
                              std::optional<uint64_t> tracepoint_timestamp,
                              StringId stack_trace,
                              StringId memory_pool);

    // Parse mem_tracker_hist_event
    void ParseMemTrackerHistEvent(int64_t timestamp,
                                  uint32_t pid,
                                  StringId tracker_name,
                                  uint64_t address,
                                  uint64_t size,
                                  uint64_t allocation_uid,
                                  StringId alloc_process,
                                  int32_t alloc_pid,
                                  uint64_t alloc_timestamp,
                                  StringId alloc_stack_trace,
                                  StringId memory_pool,
                                  bool freed,
                                  StringId free_process,
                                  int32_t free_pid,
                                  uint64_t free_timestamp,
                                  StringId free_stack_trace);

    // Flush pending allocations - creates all slices
    void FlushPendingAllocations();

    // Main entry point for parsing memory tracker events from ftrace
    // Checks event name and routes to appropriate handler
    // Returns true if the event was handled, false otherwise
    bool ParseMemoryTrackerEventFromFtrace(int64_t timestamp,
                                          uint32_t tid,
                                          base::StringView event_name,
                                          const protos::pbzero::GenericFtraceEvent::Decoder& evt);

    // Parse memory tracker events from typed ftrace (ProtoDecoder)
    // Returns true if the event was handled, false otherwise
    bool ParseMemoryTrackerEventFromTypedFtrace(int64_t timestamp,
                                                uint32_t tid,
                                                base::StringView event_name,
                                                protozero::ConstBytes blob,
                                                const FtraceMessageDescriptor* descriptor,
                                                StringId message_name_id,
                                                const StringId* field_name_ids);

    // Generic function to parse memory tracker events
    // Extracts fields from the decoder and routes to registered extensions
    void ParseMemoryTrackerEvent(int64_t timestamp,
                                 base::StringView event_name,
                                 const protos::pbzero::GenericFtraceEvent::Decoder& evt);

    // Register a tracker-specific extension parser
    void RegisterExtension(MemoryTrackerExtension* extension);

private:
    // Structure to hold event marker data (for ALLOC/FREE instant events)
    struct EventMarkerData {
        StringId tracker_name;
        StringId event_type;
        uint64_t address;
        uint64_t size;
        uint64_t allocation_uid;
        StringId process_name;
        int32_t pid;
        std::optional<int32_t> tracepoint_pid;
        std::optional<uint64_t> tracepoint_timestamp;
        StringId stack_trace;
        StringId memory_pool;
    };

    // Structure to hold allocation data for deferred creation
    struct AllocationData {
        StringId tracker_name;
        uint64_t address;
        uint64_t size;
        uint64_t allocation_uid;
        StringId alloc_process;
        int32_t alloc_pid;
        uint64_t alloc_timestamp;
        StringId alloc_stack_trace;
        StringId memory_pool;
        bool freed;
        StringId free_process;
        int32_t free_pid;
        uint64_t free_timestamp;
        StringId free_stack_trace;
    };

    // Utility function to add arguments for regular ALLOC/FREE events
    void AddEventMarkerArgs(ArgsTracker::BoundInserter* inserter,
                          const EventMarkerData& event,
                          StringId formatted_stack_trace);

    // Deduplicate and sort allocations by allocation_uid
    // Prefers freed=true entries over freed=false (complete information)
    std::vector<AllocationData> DeduplicateAndSortAllocations(
        const std::vector<AllocationData>& allocations);

    // Utility function to add arguments for hist slices (with both alloc and free data)
    void AddHistEventArgs(ArgsTracker::BoundInserter* inserter,
                         const AllocationData& alloc,
                         StringId formatted_alloc_stack_trace,
                         StringId formatted_free_stack_trace);
    
    // Initialize and update totals counters from allocation history
    void InitializeTotalsCounters(const std::vector<AllocationData>& unique_allocations);
    
    TraceProcessorContext* context_;
    
    // String IDs for event names (determines color)
    StringId mem_alloc_name_;
    StringId mem_free_name_;
    StringId track_name_;
    
    // Track ALLOC event markers (duration=0 slices) by allocation_uid to connect with FREE events via flows
    std::mutex uid_to_alloc_marker_mutex_;
    std::unordered_map<uint64_t, std::optional<SliceId>> uid_to_alloc_marker_;
    
    // Track FREE event markers (duration=0 slices) by allocation_uid to connect with hist slices via flows
    std::mutex uid_to_free_marker_mutex_;
    std::unordered_map<uint64_t, std::optional<SliceId>> uid_to_free_marker_;
    
    // Track hist slices by allocation_uid to connect with ALLOC/FREE events via flows
    std::mutex uid_to_hist_slice_mutex_;
    std::unordered_map<uint64_t, std::optional<SliceId>> uid_to_hist_slice_;

    // Store allocations for deferred creation (sorted by alloc_timestamp)
    std::mutex pending_allocations_mutex_;
    std::vector<AllocationData> pending_allocations_;
    
    // Track which Events parent tracks have been initialized (to avoid duplicate dummy slices)
    std::mutex events_parent_initialized_mutex_;
    std::unordered_set<TrackId> events_parent_initialized_;
    
    // Track total allocated bytes per memory pool and overall
    // Key: (tracker_name, memory_pool) -> total_bytes
    struct PairHash {
        std::size_t operator()(const std::pair<StringId, StringId>& p) const {
            return std::hash<uint32_t>()(p.first.raw_id()) ^
                   (std::hash<uint32_t>()(p.second.raw_id()) << 1);
        }
    };
    std::mutex totals_mutex_;
    std::unordered_map<std::pair<StringId, StringId>, int64_t, PairHash> totals_by_pool_;
    std::unordered_map<StringId, int64_t> totals_by_tracker_;  // Overall total per tracker
    
    // Registered extension parsers for tracker-specific events
    std::mutex extensions_mutex_;
    std::vector<MemoryTrackerExtension*> extensions_;
    
    // Internal function to parse CMA heap stats - delegates to registered extensions
    void ParseCmaHeapStats(int64_t timestamp,
                           StringId heap_name,
                           uint64_t total_pages,
                           uint64_t used_pages,
                           uint64_t free_pages,
                           uint64_t max_chunk_pages);
};

} // namespace trace_processor
} // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_FTRACE_HAILO_MEMORY_TRACKER_H_

