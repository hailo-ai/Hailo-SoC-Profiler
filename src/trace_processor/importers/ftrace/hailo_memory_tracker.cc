/*
 * Copyright (c) 2026 Hailo Technologies Ltd. All rights reserved.
 */

#include "src/trace_processor/importers/ftrace/hailo_memory_tracker.h"

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/protozero/proto_decoder.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/clock_converter.h"
#include "src/trace_processor/importers/common/flow_tracker.h"
#include "src/trace_processor/importers/common/global_args_tracker.h"
#include "src/trace_processor/importers/common/metadata_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/slice_translation_table.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/common/tracks.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/ftrace/ftrace_descriptors.h"
#include "src/trace_processor/importers/ftrace/ftrace_parser.h"
#include "src/trace_processor/importers/ftrace/hailo_cma_tracker.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/tables/slice_tables_py.h"
#include "src/trace_processor/storage/metadata.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "perfetto/trace_processor/basic_types.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"

namespace perfetto {
namespace trace_processor {

namespace {
// Helper function to format stack trace string (replaces commas with newlines)
// Used when kernel provides symbolized stack trace as string
StringId FormatStackTraceString(TraceStorage* storage, StringId stack_trace) {
    if (stack_trace.is_null()) {
        return kNullStringId;
    }
    base::StringView stack_str = storage->GetString(stack_trace);
    std::string formatted_stack = stack_str.ToStdString();
    size_t pos = 0;
    while ((pos = formatted_stack.find(", ", pos)) != std::string::npos) {
        formatted_stack.replace(pos, 2, "\n");
        pos += 1;
    }
    pos = 0;
    while ((pos = formatted_stack.find(",", pos)) != std::string::npos) {
        formatted_stack.replace(pos, 1, "\n");
        pos += 1;
    }
    
    return storage->InternString(base::StringView(formatted_stack));
}

// Helper function to parse raw stack entries from bytes
// Parses a byte array containing uint64_t values into a vector
// Only adds non-zero entries, and respects nr_entries limit if provided
static void ParseRawStackEntriesFromBytes(base::StringView bytes_view,
                                         uint32_t nr_entries,
                                         std::vector<uint64_t>& stack_entries) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(bytes_view.data());
    size_t bytes_len = bytes_view.size();
    size_t num_elements = bytes_len / sizeof(uint64_t);
    
    // Use nr_entries if available to limit parsing, otherwise parse all non-zero entries
    size_t max_entries = (nr_entries > 0) ? nr_entries : num_elements;
    for (size_t i = 0; i < num_elements && i < max_entries; i++) {
        uint64_t value = 0;
        memcpy(&value, bytes + i * sizeof(uint64_t), sizeof(uint64_t));
        if (value != 0) {  // Only add non-zero entries
            stack_entries.push_back(value);
        }
    }
}

// Helper function to format stack trace from array of addresses
// Used when kernel provides raw addresses as array
StringId FormatStackTraceArray(TraceStorage* storage, const std::vector<uint64_t>& stack_entries) {
    if (stack_entries.empty()) {
        return kNullStringId;
    }
    
    std::string formatted_stack;
    for (size_t i = 0; i < stack_entries.size(); i++) {
        if (i > 0) {
            formatted_stack += "\n";
        }
        char addr_str[32];
        snprintf(addr_str, sizeof(addr_str), "0x%" PRIx64, stack_entries[i]);
        formatted_stack += addr_str;
    }
    
    return storage->InternString(base::StringView(formatted_stack));
}

// Helper function to add all arguments for a hist event to an inserter
// Always adds all fields, including free_* fields even if empty (for consistency)
// Blueprint for mem_tracker track with tracker_name and event_type dimensions
// Creates hierarchy: tracker_name (e.g., "CMA") -> "Events" (parent) -> event_type (ALLOC/FREE children)
static constexpr auto kHailoMemoryTrackerEventsParentBlueprint =
    tracks::SliceBlueprint(
        "MemoryTrackerEvents",
        tracks::DimensionBlueprints(
            tracks::StringDimensionBlueprint("tracker_name")  // Creates "CMA" group
        ),
        tracks::StaticNameBlueprint("Events")
    );

static constexpr auto kHailoMemoryTrackerBlueprint =
    tracks::SliceBlueprint(
        "MemoryTracker",
        tracks::DimensionBlueprints(
            tracks::StringDimensionBlueprint("tracker_name"),  // First dimension creates "CMA" group
            tracks::StringDimensionBlueprint("event_type")    // Second dimension creates "ALLOC"/"FREE" tracks under "Events"
        ),
        tracks::FnNameBlueprint([](base::StringView /* tracker_name_dim */, base::StringView event_type) {
            // Format: "ALLOC" or "FREE" (parent "Events" is created separately)
            if (event_type.size() > 0) {
                return base::StackString<128>("%.*s",
                                             static_cast<int>(event_type.size()),
                                             event_type.data());
            }
            return base::StackString<128>("Events");
        })
    );

// Blueprint for mem_tracker history track with tracker_name dimension
// Creates hierarchy: tracker_name (e.g., "CMA") -> "Allocations History"
static constexpr auto kHailoMemoryTrackerHistBlueprint =
    tracks::SliceBlueprint(
        "MemoryTrackerHist",
        tracks::DimensionBlueprints(
            tracks::StringDimensionBlueprint("tracker_name")  // Creates "CMA" group
        ),
        tracks::StaticNameBlueprint("Allocations History")
    );

// Blueprint for mem_tracker totals parent counter track (creates "Size Totals" group)
static constexpr auto kHailoMemoryTrackerTotalsParentBlueprint =
    tracks::CounterBlueprint(
        "MemoryTrackerTotals",
        tracks::StaticUnitBlueprint("bytes"),
        tracks::DimensionBlueprints(
            tracks::StringDimensionBlueprint("tracker_name")  // Creates "CMA" group
        ),
        tracks::StaticNameBlueprint("Size Totals")
    );

// Blueprint for mem_tracker totals counter track with tracker_name and memory_pool dimensions
// Note: "∑ Total" uses sum symbol (U+2211) to indicate aggregate and sort after device names
static constexpr auto kHailoMemoryTrackerTotalsBlueprint =
    tracks::CounterBlueprint(
        "MemoryTrackerTotals",
        tracks::StaticUnitBlueprint("bytes"),
        tracks::DimensionBlueprints(
            tracks::StringDimensionBlueprint("tracker_name"),  // Creates "CMA" group
            tracks::StringDimensionBlueprint("memory_pool")   // Creates per-pool or "∑ Total" tracks
        ),
        tracks::FnNameBlueprint([](base::StringView /* tracker_name_dim */, base::StringView memory_pool) {
            if (memory_pool.size() > 0 && memory_pool != base::StringView("∑ Total")) {
                return base::StackString<128>("%.*s",
                                             static_cast<int>(memory_pool.size()),
                                             memory_pool.data());
            }
            return base::StackString<128>("∑ Total");
        })
    );
} // namespace

HailoMemoryTracker::HailoMemoryTracker(TraceProcessorContext* context)
    : context_(context),
      mem_alloc_name_(context->storage->InternString("mem_alloc")),
      mem_free_name_(context->storage->InternString("mem_free")),
      track_name_(context->storage->InternString("mem_tracker")) {
}

HailoMemoryTracker* HailoMemoryTracker::GetOrCreate(TraceProcessorContext* context) {
    if (!context->hailo_memory_tracker) {
        context->hailo_memory_tracker.reset(new HailoMemoryTracker(context));
        // Initialize CMA tracker extension
        HailoCmaTracker::GetOrCreate(context);
    }
    return static_cast<HailoMemoryTracker*>(context->hailo_memory_tracker.get());
}

void HailoMemoryTracker::ParseMemTrackerEvent(int64_t timestamp,
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
                                              StringId memory_pool,
                                              StringId device_name) {
    // Validate we have essential data - use fallback if missing
    StringId actual_tracker_name = tracker_name;
    if (tracker_name.is_null()) {
        PERFETTO_LOG("HailoMemoryTracker: tracker_name is null, using 'unknown'");
        actual_tracker_name = context_->storage->InternString("unknown");
    }
    
    StringId actual_event_type = event_type;
    if (event_type.is_null()) {
        PERFETTO_LOG("HailoMemoryTracker: event_type is null, using UNKNOWN");
        actual_event_type = context_->storage->InternString("UNKNOWN");
    }
    
    // Create event marker data struct
    EventMarkerData event_data = {
        actual_tracker_name,
        actual_event_type,
        address,
        size,
        allocation_uid,
        process_name,
        static_cast<int32_t>(pid),
        tracepoint_pid,
        tracepoint_timestamp,
        stack_trace,
        memory_pool,
        device_name
    };
    
    // Get the track for mem_tracker events - separate tracks per tracker_name and event_type
    // Creates hierarchy: tracker_name (uppercased) -> "Events" (parent) -> event_type (ALLOC/FREE children)
    // Uppercase tracker_name for display (e.g., "cma" -> "CMA")
    base::StringView tracker_name_raw = context_->storage->GetString(actual_tracker_name);
    std::string tracker_name_upper = tracker_name_raw.ToStdString();
    std::transform(tracker_name_upper.begin(), tracker_name_upper.end(), 
                   tracker_name_upper.begin(), ::toupper);
    base::StringView tracker_name_str(tracker_name_upper);
    base::StringView event_type_str = context_->storage->GetString(actual_event_type);
    
    // Create parent "Events" track first
    TrackId events_parent_track_id = context_->track_tracker->InternTrack(
        kHailoMemoryTrackerEventsParentBlueprint,
        tracks::Dimensions(tracker_name_str)
    );
    
    // Create a dummy slice on the parent "Events" track so it shows up in the UI
    // (parent tracks without slices don't appear in _slice_track_summary)
    {
        std::lock_guard<std::mutex> lock(events_parent_initialized_mutex_);
        if (events_parent_initialized_.find(events_parent_track_id) == events_parent_initialized_.end()) {
            StringId events_parent_name_id = context_->storage->InternString("Events");
            StringId events_parent_category = context_->storage->InternString("events_parent");
            (void)context_->slice_tracker->Scoped(
                timestamp,
                events_parent_track_id,
                events_parent_category,
                events_parent_name_id,
                0, // Duration 0 - just a marker to make the track visible
                [](ArgsTracker::BoundInserter* /* inserter */) {
                    // Empty args - just making the track visible
                }
            );
            events_parent_initialized_.insert(events_parent_track_id);
        }
    }
    
    // Create child track for this specific event type (ALLOC or FREE)
    TrackId track_id = context_->track_tracker->InternTrack(
        kHailoMemoryTrackerBlueprint,
        tracks::Dimensions(tracker_name_str, event_type_str)
    );
    
    // Set parent_id for the child track
    auto track_row = context_->storage->mutable_track_table()->FindById(track_id);
    if (track_row) {
        track_row->set_parent_id(events_parent_track_id);
    }
    
    // Determine slice name based on event type (ALLOC or FREE)
    StringId slice_category = actual_event_type;
    
    // Get event type string for suffix
    base::StringView event_type_str_view = context_->storage->GetString(event_data.event_type);
    std::string event_type_suffix = " - " + event_type_str_view.ToStdString();
    
    auto marker_name = base::StackString<256>(
        "[UID=%" PRIu64 " address=0x%" PRIx64 " pid=%u]%s",
        event_data.allocation_uid,
        event_data.address,
        event_data.pid,
        event_type_suffix.c_str()
    );

    StringId marker_name_id = context_->storage->InternString(marker_name.string_view());

    PERFETTO_DLOG("HailoMemoryTracker: Creating slice for %.*s event: addr=0x%" PRIx64 " size=%" PRIu64 " pid=%u track_id=%u",
                  static_cast<int>(event_type_str.size()), event_type_str.data(), event_data.address, event_data.size, event_data.pid, track_id.value);

    // Use tracepoint_timestamp if available (from the "body" of the event - closer to actual event time),
    // otherwise use event timestamp from the ftrace's header (timestamp field)
    int64_t event_ts = event_data.tracepoint_timestamp.has_value() 
                       ? static_cast<int64_t>(event_data.tracepoint_timestamp.value()) 
                       : timestamp;

    auto marker_id = context_->slice_tracker->Scoped(
        event_ts,
        track_id,
        slice_category,
        marker_name_id,
        0, // Duration 0 for instant events
        [this, event_data](ArgsTracker::BoundInserter* inserter) {
            AddEventMarkerArgs(inserter, event_data, event_data.stack_trace);
        }
    );
    
    if (!marker_id.has_value()) {
        PERFETTO_LOG("HailoMemoryTracker: Failed to create slice for event");
        return;
    }
    
    PERFETTO_DLOG("HailoMemoryTracker: Created marker_id=%u", marker_id->value);
    
    // Manage flows: connect FREE events to their corresponding ALLOC events
    StringId alloc_string_id = context_->storage->InternString("ALLOC");
    StringId free_string_id = context_->storage->InternString("FREE");
    
    // Only create flows if we have a valid allocation_uid (non-zero)
    if (event_data.allocation_uid == 0) {
        PERFETTO_DLOG("HailoMemoryTracker: Skipping flow creation - allocation_uid is 0 (unknown) for address 0x%" PRIx64, event_data.address);
        return;
    }
    
    // Use composite key (tracker_name, allocation_uid) since each tracker has its own UID counter
    TrackerUidKey key = {event_data.tracker_name.raw_id(), event_data.allocation_uid};
    std::lock_guard<std::mutex> lock(uid_to_alloc_marker_mutex_);
    
    if (event_data.event_type == alloc_string_id) {
        // Store ALLOC marker slice_id for this (tracker_name, allocation_uid)
        uid_to_alloc_marker_[key] = marker_id;
    } else if (event_data.event_type == free_string_id) {
        // Store FREE marker slice_id for this (tracker_name, allocation_uid) (for linking with hist slices)
        {
            std::lock_guard<std::mutex> free_lock(uid_to_free_marker_mutex_);
            uid_to_free_marker_[key] = marker_id;
        }
        
        // Look up corresponding ALLOC event and create flow
        // Don't erase the ALLOC entry yet - we need it for hist slice flows later
        auto it = uid_to_alloc_marker_.find(key);
        if (it != uid_to_alloc_marker_.end() && it->second.has_value()) {
            context_->flow_tracker->InsertFlow(it->second.value(), marker_id.value());
            PERFETTO_DLOG("HailoMemoryTracker: Created flow from ALLOC to FREE for allocation_uid %" PRIu64, event_data.allocation_uid);
        } else {
            PERFETTO_DLOG("HailoMemoryTracker: FREE event for address 0x%" PRIx64 " has no matching ALLOC", event_data.address);
        }
    }
}

void HailoMemoryTracker::ParseMemTrackerHistEvent(int64_t /* timestamp */,
                                                  uint32_t /* pid */,
                                                  StringId tracker_name,
                                                  uint64_t address,
                                                  uint64_t size,
                                                  uint64_t allocation_uid,
                                                  StringId alloc_process,
                                                  int32_t alloc_pid,
                                                  uint64_t alloc_timestamp,
                                                  StringId alloc_stack_trace,
                                                  StringId memory_pool,
                                                  StringId device_name,
                                                  bool freed,
                                                  StringId free_process,
                                                  int32_t free_pid,
                                                  uint64_t free_timestamp,
                                                  StringId free_stack_trace) {
    // Validate we have essential data
    StringId actual_tracker_name = tracker_name;
    if (tracker_name.is_null()) {
        PERFETTO_LOG("HailoMemoryTracker: tracker_name is null in hist event, using 'unknown'");
        actual_tracker_name = context_->storage->InternString("unknown");
    }
    
    // Store allocation data for deferred creation (sorted by alloc_timestamp)
    // This handles both freed allocations (freed=true) and unterminated allocations (freed=false)
    //
    // Note: Pre-recording allocations (allocated before trace started) will have freed=false
    // and will be exported by export_history_to_ftrace_once() when the first event occurs.
    // These allocations won't have corresponding ALLOC events because they happened before
    // recording started, but they will have slices created with their original alloc_timestamp.
    AllocationData alloc_data = {
        actual_tracker_name,
        address,
        size,
        allocation_uid,  // Always present (0 = unknown/invalid)
        alloc_process,
        alloc_pid,
        alloc_timestamp,
        alloc_stack_trace,
        memory_pool,
        device_name,
        freed,
        free_process,
        free_pid,
        free_timestamp,
        free_stack_trace
    };
    
    std::lock_guard<std::mutex> lock(pending_allocations_mutex_);
    pending_allocations_.push_back(alloc_data);
    PERFETTO_DLOG("HailoMemoryTracker: Stored allocation for deferred creation: address 0x%" PRIx64 " alloc_ts=%" PRIu64 " freed=%d",
                  address, alloc_timestamp, freed);
}

std::vector<HailoMemoryTracker::AllocationData> HailoMemoryTracker::DeduplicateAndSortAllocations(
    const std::vector<AllocationData>& allocations) {
    // Deduplicate by (tracker_name, allocation_uid): prefer freed=true over freed=false (complete information)
    // Each tracker has its own UID counter, so we need to include tracker_name in the key
    std::unordered_map<TrackerUidKey, AllocationData, TrackerUidKeyHash> deduplicated;
    for (const auto& alloc : allocations) {
        // Skip entries with invalid UID (0)
        if (alloc.allocation_uid == 0) {
            continue;
        }
        
        TrackerUidKey key = {alloc.tracker_name.raw_id(), alloc.allocation_uid};
        auto it = deduplicated.find(key);
        
        if (it == deduplicated.end()) {
            // First entry for this (tracker_name, allocation_uid)
            deduplicated[key] = alloc;
        } else {
            // Duplicate found - prefer freed=true over freed=false
            if (alloc.freed && !it->second.freed) {
                // Replace freed=false with freed=true (complete information)
                it->second = alloc;
            } else if (alloc.freed && it->second.freed && alloc.free_timestamp > it->second.free_timestamp) {
                // Both freed=true - prefer later free_timestamp (shouldn't happen, but handle it)
                it->second = alloc;
            }
            // Otherwise keep existing (either both freed=false, or existing is freed=true)
        }
    }
    
    // Convert to vector and sort by allocation timestamp
    std::vector<AllocationData> result;
    result.reserve(deduplicated.size());
    for (const auto& pair : deduplicated) {
        result.push_back(pair.second);
    }
    
    std::sort(result.begin(), result.end(),
              [](const AllocationData& a, const AllocationData& b) {
                  return a.alloc_timestamp < b.alloc_timestamp;
              });
    
    return result;
}

void HailoMemoryTracker::FlushPendingAllocations() {
    std::lock_guard<std::mutex> lock(pending_allocations_mutex_);
    
    // Deduplicate and sort allocations
    std::vector<AllocationData> unique_allocations = DeduplicateAndSortAllocations(pending_allocations_);
    
    // Initialize counters from allocation history
    InitializeTotalsCounters(unique_allocations);
    
    // Group allocations by tracker_name for per-track depth calculation
    // This is critical because depths must be contiguous within each track to avoid visual gaps
    std::unordered_map<uint32_t, std::vector<size_t>> allocations_by_tracker;
    for (size_t i = 0; i < unique_allocations.size(); i++) {
        allocations_by_tracker[unique_allocations[i].tracker_name.raw_id()].push_back(i);
    }
    
    // Calculate depths per tracker and store in a map
    std::unordered_map<size_t, uint32_t> alloc_depths;  // alloc_index -> depth
    
    for (auto& [tracker_id, indices] : allocations_by_tracker) {
        // Sort this tracker's allocations by start time
        std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
            const auto& alloc_a = unique_allocations[a];
            const auto& alloc_b = unique_allocations[b];
            int64_t ts_a = (alloc_a.alloc_timestamp > 0) 
                           ? static_cast<int64_t>(alloc_a.alloc_timestamp)
                           : static_cast<int64_t>(alloc_a.free_timestamp);
            int64_t ts_b = (alloc_b.alloc_timestamp > 0) 
                           ? static_cast<int64_t>(alloc_b.alloc_timestamp)
                           : static_cast<int64_t>(alloc_b.free_timestamp);
            return ts_a < ts_b;
        });
        
        // Assign depths using interval scheduling - reuse depths when slices end
        // This is efficient AND gap-free because we calculate per-tracker
        std::vector<int64_t> depth_end_times;  // depth_end_times[d] = end time of slice at depth d
        
        for (size_t idx : indices) {
            const auto& alloc = unique_allocations[idx];
            int64_t start_ts = (alloc.alloc_timestamp > 0) 
                               ? static_cast<int64_t>(alloc.alloc_timestamp) 
                               : static_cast<int64_t>(alloc.free_timestamp);
            int64_t end_ts = alloc.freed ? static_cast<int64_t>(alloc.free_timestamp) : INT64_MAX;
            
            // Find the lowest depth where the previous slice has ended
            uint32_t depth = 0;
            bool found = false;
            for (size_t d = 0; d < depth_end_times.size(); d++) {
                if (start_ts >= depth_end_times[d]) {
                    // This depth is available - previous slice has ended
                    depth = static_cast<uint32_t>(d);
                    depth_end_times[d] = end_ts;
                    found = true;
                    break;
                }
            }
            
            if (!found) {
                // No available depth - need a new one
                depth = static_cast<uint32_t>(depth_end_times.size());
                depth_end_times.push_back(end_ts);
            }
            
            alloc_depths[idx] = depth;
        }
    }
    
    // Create slices with calculated depths (process in original order for consistent flows)
    for (size_t alloc_idx = 0; alloc_idx < unique_allocations.size(); alloc_idx++) {
        const auto& alloc = unique_allocations[alloc_idx];
        uint32_t depth = alloc_depths[alloc_idx];
        
        // Get the track for "Allocations History" under tracker_name
        base::StringView tracker_name_raw = context_->storage->GetString(alloc.tracker_name);
        std::string tracker_name_upper = tracker_name_raw.ToStdString();
        std::transform(tracker_name_upper.begin(), tracker_name_upper.end(), 
                       tracker_name_upper.begin(), ::toupper);
        base::StringView tracker_name_str(tracker_name_upper);
        
        TrackId track_id = context_->track_tracker->InternTrack(
            kHailoMemoryTrackerHistBlueprint,
            tracks::Dimensions(tracker_name_str)
        );
        
        // Calculate slice start timestamp
        int64_t slice_start_ts = (alloc.alloc_timestamp > 0) 
                                  ? static_cast<int64_t>(alloc.alloc_timestamp) 
                                  : static_cast<int64_t>(alloc.free_timestamp);
        
        auto slice_name = base::StackString<256>(
            "[UID=%" PRIu64 " address=0x%" PRIx64 " pid=%d]",
            alloc.allocation_uid,
            alloc.address,
            alloc.alloc_pid
        );
        
        StringId slice_name_id = context_->storage->InternString(slice_name.string_view());
        StringId slice_category = alloc.memory_pool.is_null() ? alloc.tracker_name : alloc.memory_pool;
        
        // Calculate duration for the slice table
        int64_t duration;
        if (alloc.freed) {
            duration = static_cast<int64_t>(alloc.free_timestamp) - slice_start_ts;
            PERFETTO_DLOG("HailoMemoryTracker: Creating hist slice: UID=%" PRIu64 " address=0x%" PRIx64 
                        " start_ts=%" PRId64 " free_ts=%" PRIu64 " duration=%" PRId64 "ns (%.3fms) depth=%u",
                        alloc.allocation_uid, alloc.address, slice_start_ts, 
                        alloc.free_timestamp, duration, duration / 1000000.0, depth);
            if (duration < 0) {
                PERFETTO_LOG("HailoMemoryTracker: Negative duration for address 0x%" PRIx64
                            " (start=%" PRId64 " free=%" PRIu64 ")",
                            alloc.address, slice_start_ts, alloc.free_timestamp);
                duration = 0;
            }
        } else {
            // Unterminated allocation: use -1 duration (kPendingDuration)
            duration = -1;
            PERFETTO_DLOG("HailoMemoryTracker: Unterminated allocation 0x%" PRIx64 " depth=%u",
                          alloc.address, depth);
        }
        
        // Directly insert into slice table to bypass SliceTracker's stack-based nesting
        auto* slice_table = context_->storage->mutable_slice_table();
        tables::SliceTable::Row row;
        row.ts = slice_start_ts;
        row.dur = duration;
        row.track_id = track_id;
        row.category = slice_category;
        row.name = slice_name_id;
        row.depth = depth;  // Use per-tracker calculated depth for proper visual stacking
        
        auto slice_id = slice_table->Insert(row).id;
        std::optional<SliceId> hist_slice_id = slice_id;
        
        // Add arguments using ArgsTracker
        auto args_inserter = context_->args_tracker->AddArgsTo(slice_id);
        AddHistEventArgs(&args_inserter, alloc, alloc.alloc_stack_trace, alloc.free_stack_trace);
        
        // Create flows: ALLOC event → hist slice → FREE event
        // Only create flows if we have a valid allocation_uid (non-zero)
        if (hist_slice_id.has_value() && alloc.allocation_uid != 0) {
            // Use composite key (tracker_name, allocation_uid) since each tracker has its own UID counter
            TrackerUidKey key = {alloc.tracker_name.raw_id(), alloc.allocation_uid};
            
            // Link ALLOC marker to hist slice
            {
                std::lock_guard<std::mutex> alloc_lock(uid_to_alloc_marker_mutex_);
                auto alloc_it = uid_to_alloc_marker_.find(key);
                if (alloc_it != uid_to_alloc_marker_.end() && alloc_it->second.has_value()) {
                    context_->flow_tracker->InsertFlow(alloc_it->second.value(), hist_slice_id.value());
                    // Now we can remove the ALLOC entry since we've created the hist slice flow
                    uid_to_alloc_marker_.erase(alloc_it);
                    PERFETTO_DLOG("HailoMemoryTracker: Created flow from ALLOC marker to hist slice for allocation_uid %" PRIu64, alloc.allocation_uid);
                }
            }
            
            // Link hist slice to FREE marker
            {
                std::lock_guard<std::mutex> free_lock(uid_to_free_marker_mutex_);
                auto free_it = uid_to_free_marker_.find(key);
                if (free_it != uid_to_free_marker_.end() && free_it->second.has_value()) {
                    context_->flow_tracker->InsertFlow(hist_slice_id.value(), free_it->second.value());
                    // Remove the FREE entry after creating the flow
                    uid_to_free_marker_.erase(free_it);
                    PERFETTO_DLOG("HailoMemoryTracker: Created flow from hist slice to FREE marker for allocation_uid %" PRIu64, alloc.allocation_uid);
                }
            }
        }
    }
    
    // Clear pending allocations after creating slices
    pending_allocations_.clear();
}

void HailoMemoryTracker::AddEventMarkerArgs(ArgsTracker::BoundInserter* inserter,
                                            const EventMarkerData& event,
                                            StringId formatted_stack_trace) {
    // Store address as combined hex and decimal format: "0x8d8c0 (579776)"
    auto address_str = base::StackString<64>("0x%" PRIx64 " (%" PRIu64 ")", 
                                             event.address, event.address);
    StringId address_id = context_->storage->InternString(address_str.string_view());
    inserter->AddArg(context_->storage->InternString("address"),
                   Variadic::String(address_id));
    inserter->AddArg(context_->storage->InternString("size"),
                   Variadic::UnsignedInteger(event.size));
    // Store allocation_uid (always present, 0 = unknown/invalid)
    inserter->AddArg(context_->storage->InternString("allocation_uid"),
                   Variadic::UnsignedInteger(event.allocation_uid));
    // Store memory_pool (specific CMA heap name like "hailo_media_buf,cma" or "linux,cma")
    // tracker_name is not stored as it's already shown in the grouping hierarchy
    if (!event.memory_pool.is_null()) {
        inserter->AddArg(context_->storage->InternString("memory_pool"),
                       Variadic::String(event.memory_pool));
    }
    // Store device_name (e.g., "7c040000.xrp" - the device that triggered the allocation)
    if (!event.device_name.is_null()) {
        inserter->AddArg(context_->storage->InternString("device"),
                       Variadic::String(event.device_name));
    }
    // Add process name and PID as separate fields
    if (!event.process_name.is_null()) {
        inserter->AddArg(context_->storage->InternString("process"),
                       Variadic::String(event.process_name));
    }
    inserter->AddArg(context_->storage->InternString("pid"),
                   Variadic::UnsignedInteger(event.pid));
    // Store event_type (ALLOC/FREE)
    inserter->AddArg(context_->storage->InternString("event_type"),
                   Variadic::String(event.event_type));
    // Store tracepoint PID if available
    if (event.tracepoint_pid.has_value()) {
        inserter->AddArg(context_->storage->InternString("tracepoint_pid"),
                       Variadic::Integer(event.tracepoint_pid.value()));
    }
    // Store tracepoint timestamp if available as combined absolute time and raw timestamp
    if (event.tracepoint_timestamp.has_value()) {
        uint64_t raw_ts = event.tracepoint_timestamp.value();
        int64_t ts = static_cast<int64_t>(raw_ts);
        base::StatusOr<std::string> abs_time = context_->clock_converter->ToAbsTime(ts);
        if (abs_time.ok()) {
            auto timestamp_str = base::StackString<128>("%s (%" PRIu64 ")", 
                                                        abs_time->c_str(), raw_ts);
            StringId timestamp_id = context_->storage->InternString(timestamp_str.string_view());
            inserter->AddArg(context_->storage->InternString("tracepoint_timestamp"),
                           Variadic::String(timestamp_id));
        } else {
            // Fallback to raw timestamp if conversion fails
            inserter->AddArg(context_->storage->InternString("tracepoint_timestamp"),
                           Variadic::UnsignedInteger(raw_ts));
        }
    }
    if (!formatted_stack_trace.is_null()) {
        inserter->AddArg(context_->storage->InternString("stack"),
                       Variadic::String(formatted_stack_trace));
    }
}

void HailoMemoryTracker::AddHistEventArgs(ArgsTracker::BoundInserter* inserter,
                                          const AllocationData& alloc,
                                          StringId formatted_alloc_stack_trace,
                                          StringId formatted_free_stack_trace) {
    // Store address as combined hex and decimal format: "0x8d8c0 (579776)"
    auto address_str = base::StackString<64>("0x%" PRIx64 " (%" PRIu64 ")", 
                                             alloc.address, alloc.address);
    StringId address_id = context_->storage->InternString(address_str.string_view());
    inserter->AddArg(context_->storage->InternString("address"),
                   Variadic::String(address_id));
    inserter->AddArg(context_->storage->InternString("size"),
                   Variadic::UnsignedInteger(alloc.size));
    // Store allocation_uid (always present, 0 = unknown/invalid)
    inserter->AddArg(context_->storage->InternString("allocation_uid"),
                   Variadic::UnsignedInteger(alloc.allocation_uid));
    if (!alloc.memory_pool.is_null()) {
        inserter->AddArg(context_->storage->InternString("memory_pool"),
                       Variadic::String(alloc.memory_pool));
    }
    // Store device_name (e.g., "7c040000.xrp" - the device that triggered the allocation)
    if (!alloc.device_name.is_null()) {
        inserter->AddArg(context_->storage->InternString("device"),
                       Variadic::String(alloc.device_name));
    }
    if (!alloc.alloc_process.is_null()) {
        inserter->AddArg(context_->storage->InternString("alloc_process"),
                       Variadic::String(alloc.alloc_process));
    }
    inserter->AddArg(context_->storage->InternString("alloc_pid"),
                   Variadic::Integer(alloc.alloc_pid));
    // Store alloc_timestamp as combined absolute time and raw timestamp
    {
        uint64_t raw_ts = alloc.alloc_timestamp;
        int64_t ts = static_cast<int64_t>(raw_ts);
        base::StatusOr<std::string> abs_time = context_->clock_converter->ToAbsTime(ts);
        if (abs_time.ok()) {
            auto timestamp_str = base::StackString<128>("%s (%" PRIu64 ")", 
                                                        abs_time->c_str(), raw_ts);
            StringId timestamp_id = context_->storage->InternString(timestamp_str.string_view());
            inserter->AddArg(context_->storage->InternString("alloc_timestamp"),
                           Variadic::String(timestamp_id));
        } else {
            // Fallback to raw timestamp if conversion fails
            inserter->AddArg(context_->storage->InternString("alloc_timestamp"),
                           Variadic::UnsignedInteger(raw_ts));
        }
    }
    if (!formatted_alloc_stack_trace.is_null()) {
        inserter->AddArg(context_->storage->InternString("alloc_stack"),
                       Variadic::String(formatted_alloc_stack_trace));
    }
    inserter->AddArg(context_->storage->InternString("freed"),
                   Variadic::Boolean(alloc.freed));
    if (alloc.freed) {
        // Only add free-related arguments if the allocation was actually freed
        if (!alloc.free_process.is_null()) {
            inserter->AddArg(context_->storage->InternString("free_process"),
                           Variadic::String(alloc.free_process));
        }
        inserter->AddArg(context_->storage->InternString("free_pid"),
                       Variadic::Integer(alloc.free_pid));
        // Store free_timestamp as combined absolute time and raw timestamp
        {
            uint64_t raw_ts = alloc.free_timestamp;
            int64_t ts = static_cast<int64_t>(raw_ts);
            base::StatusOr<std::string> abs_time = context_->clock_converter->ToAbsTime(ts);
            if (abs_time.ok()) {
                auto timestamp_str = base::StackString<128>("%s (%" PRIu64 ")", 
                                                            abs_time->c_str(), raw_ts);
                StringId timestamp_id = context_->storage->InternString(timestamp_str.string_view());
                inserter->AddArg(context_->storage->InternString("free_timestamp"),
                               Variadic::String(timestamp_id));
            } else {
                // Fallback to raw timestamp if conversion fails
                inserter->AddArg(context_->storage->InternString("free_timestamp"),
                               Variadic::UnsignedInteger(raw_ts));
            }
        }
        if (!formatted_free_stack_trace.is_null()) {
            inserter->AddArg(context_->storage->InternString("free_stack"),
                           Variadic::String(formatted_free_stack_trace));
        }
    }
    // Note: Unterminated allocations (freed=false) don't have free_* arguments
    // and are created with Begin() instead of Scoped(), giving them kPendingDuration = -1
    // which makes them visually extend indefinitely without an endpoint in the UI
}

void HailoMemoryTracker::InitializeTotalsCounters(const std::vector<AllocationData>& unique_allocations) {
    if (unique_allocations.empty()) {
        return;
    }
    
    // Group allocations by tracker_name
    std::unordered_map<StringId, std::vector<AllocationData>> allocations_by_tracker;
    for (const auto& alloc : unique_allocations) {
        allocations_by_tracker[alloc.tracker_name].push_back(alloc);
    }
    
    // Process each tracker separately
    for (const auto& [tracker_name, tracker_allocations] : allocations_by_tracker) {
        // Build chronological event list: (timestamp, size_delta, memory_pool)
        std::vector<std::tuple<int64_t, int64_t, StringId>> counter_events;
        
        // Also collect unique memory pools for pre-creating tracks in sorted order
        std::set<std::string> unique_pools;
        
        for (const auto& alloc : tracker_allocations) {
            int64_t alloc_ts = static_cast<int64_t>(alloc.alloc_timestamp);
            StringId memory_pool = alloc.memory_pool.is_null() 
                                   ? context_->storage->InternString("") 
                                   : alloc.memory_pool;
            counter_events.push_back({alloc_ts, static_cast<int64_t>(alloc.size), memory_pool});
            
            // Collect pool name for track ordering
            base::StringView pool_str = memory_pool.is_null() 
                                        ? base::StringView("") 
                                        : context_->storage->GetString(memory_pool);
            if (pool_str.size() > 0) {
                unique_pools.insert(pool_str.ToStdString());
            }
            
            if (alloc.freed) {
                int64_t free_ts = static_cast<int64_t>(alloc.free_timestamp);
                counter_events.push_back({free_ts, -static_cast<int64_t>(alloc.size), memory_pool});
            }
        }
        std::sort(counter_events.begin(), counter_events.end(),
                  [](const auto& a, const auto& b) { return std::get<0>(a) < std::get<0>(b); });
        
        // Uppercase tracker_name for display
        base::StringView tracker_name_raw = context_->storage->GetString(tracker_name);
        std::string tracker_name_upper = tracker_name_raw.ToStdString();
        std::transform(tracker_name_upper.begin(), tracker_name_upper.end(), 
                       tracker_name_upper.begin(), ::toupper);
        base::StringView tracker_name_str(tracker_name_upper);
        
        // Create parent "Totals" track first
        TrackId totals_parent_track_id = context_->track_tracker->InternTrack(
            kHailoMemoryTrackerTotalsParentBlueprint,
            tracks::Dimensions(tracker_name_str)
        );
        
        auto* track_table = context_->storage->mutable_track_table();
        
        // Pre-create tracks in sorted order: individual pools first (alphabetically), then "Total" last
        // This ensures consistent track ordering in the UI
        std::unordered_map<std::string, TrackId> pool_track_ids;
        
        for (const auto& pool_name : unique_pools) {
            base::StringView pool_str(pool_name);
            TrackId pool_track_id = context_->track_tracker->InternTrack(
                kHailoMemoryTrackerTotalsBlueprint,
                tracks::Dimensions(tracker_name_str, pool_str)
            );
            auto pool_track_row = track_table->FindById(pool_track_id);
            if (pool_track_row) {
                pool_track_row->set_parent_id(totals_parent_track_id);
            }
            pool_track_ids[pool_name] = pool_track_id;
        }
        
        // Create "Total" track last so it appears at the bottom
        TrackId total_track_id = context_->track_tracker->InternTrack(
            kHailoMemoryTrackerTotalsBlueprint,
            tracks::Dimensions(tracker_name_str, base::StringView("∑ Total"))
        );
        auto total_track_row = track_table->FindById(total_track_id);
        if (total_track_row) {
            total_track_row->set_parent_id(totals_parent_track_id);
        }
        
        // Process events chronologically to build cumulative counters
        std::lock_guard<std::mutex> totals_lock(totals_mutex_);
        std::unordered_map<StringId, int64_t> pool_totals;  // Per-pool totals
        int64_t tracker_total = 0;  // Overall tracker total
        
        for (const auto& [ts, delta, memory_pool] : counter_events) {
            // Update per-pool total
            pool_totals[memory_pool] += delta;
            int64_t pool_total = pool_totals[memory_pool];
            
            // Update overall tracker total
            tracker_total += delta;
            
            // Get memory pool string for track dimension
            base::StringView memory_pool_str = memory_pool.is_null() 
                                               ? base::StringView("") 
                                               : context_->storage->GetString(memory_pool);
            
            // Get pre-created per-pool counter track
            TrackId pool_track_id;
            std::string pool_name_std = memory_pool_str.ToStdString();
            auto it = pool_track_ids.find(pool_name_std);
            if (it != pool_track_ids.end()) {
                pool_track_id = it->second;
            } else {
                // Fallback: create track if not found (shouldn't happen)
                pool_track_id = context_->track_tracker->InternTrack(
                    kHailoMemoryTrackerTotalsBlueprint,
                    tracks::Dimensions(tracker_name_str, memory_pool_str)
                );
            }
            
            context_->event_tracker->PushCounter(
                ts,
                static_cast<double>(pool_total),
                pool_track_id
            );
            
            // Push to overall total counter track (already created above)
            context_->event_tracker->PushCounter(
                ts,
                static_cast<double>(tracker_total),
                total_track_id
            );
        }
    }
}

void HailoMemoryTracker::RegisterExtension(MemoryTrackerExtension* extension) {
    if (!extension) {
        PERFETTO_LOG("HailoMemoryTracker: Attempted to register null extension");
        return;
    }
    std::lock_guard<std::mutex> lock(extensions_mutex_);
    extensions_.push_back(extension);
    PERFETTO_DLOG("HailoMemoryTracker: Registered extension parser");
}

bool HailoMemoryTracker::ParseMemoryTrackerEventFromFtrace(int64_t timestamp,
                                                           uint32_t tid,
                                                           base::StringView event_name,
                                                           const protos::pbzero::GenericFtraceEvent::Decoder& evt) {
    // Check if this is a memory tracker event
    if (!base::StartsWith(event_name.ToStdString(), "mem_tracker") && 
        event_name != "mem_tracker_event" && 
        event_name != "mem_tracker_hist_event" &&
        event_name != "cma_heap_stats") {
        return false;
    }
    
    // Check for hist events FIRST to avoid parsing them as regular events
    bool is_mem_tracker_hist = (event_name == "mem_tracker/mem_tracker_hist_event" ||
                                event_name == "mem_tracker_hist_event");
    
    // Check if this is mem_tracker_event (but NOT hist events)
    bool is_mem_tracker = !is_mem_tracker_hist &&
                          (event_name == "mem_tracker/mem_tracker_event" ||
                           event_name == "mem_tracker_event");
    
    // Extract fields for mem_tracker_event
    if (is_mem_tracker) {
        StringId tracker_name_id = kNullStringId;
        StringId memory_pool_id = kNullStringId;
        StringId device_name_id = kNullStringId;
        StringId event_type_id = kNullStringId;
        uint64_t address = 0;
        uint64_t size = 0;
        uint64_t allocation_uid = 0;
        StringId process_name_id = kNullStringId;
        int32_t tracepoint_pid = 0;
        bool has_tracepoint_pid = false;
        uint64_t tracepoint_timestamp = 0;
        bool has_tracepoint_timestamp = false;
        StringId stack_trace_id = kNullStringId;
        std::vector<uint64_t> stack_entries;
        uint32_t nr_stack_entries = 0;

        for (auto it = evt.field(); it; ++it) {
            protos::pbzero::GenericFtraceEvent::Field::Decoder fld(*it);
            base::StringView field_name = fld.name();
            std::string field_name_str = field_name.ToStdString();
            
            if (fld.has_int_value()) {
                if (field_name == "address") {
                    address = static_cast<uint64_t>(fld.int_value());
                } else if (field_name == "size") {
                    size = static_cast<uint64_t>(fld.int_value());
                } else if (field_name == "allocation_uid") {
                    allocation_uid = static_cast<uint64_t>(fld.int_value());
                } else if (field_name == "pid") {
                    tracepoint_pid = static_cast<int32_t>(fld.int_value());
                    has_tracepoint_pid = true;
                } else if (field_name == "nr_raw_stack_entries") {
                    nr_stack_entries = static_cast<uint32_t>(fld.int_value());
                }
            } else if (fld.has_uint_value()) {
                if (field_name == "address") {
                    address = fld.uint_value();
                } else if (field_name == "size") {
                    size = fld.uint_value();
                } else if (field_name == "allocation_uid") {
                    allocation_uid = fld.uint_value();
                } else if (field_name == "timestamp") {
                    tracepoint_timestamp = fld.uint_value();
                    has_tracepoint_timestamp = true;
                } else if (field_name == "nr_raw_stack_entries") {
                    nr_stack_entries = static_cast<uint32_t>(fld.uint_value());
                }
            } else if (fld.has_str_value()) {
                StringId str_value = context_->storage->InternString(fld.str_value());
                if (field_name == "tracker_name") {
                    tracker_name_id = str_value;
                } else if (field_name == "memory_pool") {
                    memory_pool_id = str_value;
                } else if (field_name == "device_name") {
                    device_name_id = str_value;
                } else if (field_name == "event_type") {
                    event_type_id = str_value;
                } else if (field_name == "process") {
                    process_name_id = str_value;
                } else if (field_name == "symbolized_stack_trace") {
                    stack_trace_id = str_value;
                } else if (field_name == "raw_stack_entries") {
                    // raw_stack_entries is sent as bytes (array of uint64_t)
                    ParseRawStackEntriesFromBytes(fld.str_value(), nr_stack_entries, stack_entries);
                }
            }
        }
        
        // Infer mode from which fields are populated:
        // - If symbolized_stack_trace is present and non-empty, use it (symbolized mode)
        // - Else if raw_stack_entries is present and non-empty, use it (raw mode)
        // - Otherwise, no stack trace (disabled)
        
        if (!stack_trace_id.is_null() && 
            context_->storage->GetString(stack_trace_id).ToStdString() != "") {
            // Symbolized mode: format the symbolized string (replace commas with newlines)
            stack_trace_id = FormatStackTraceString(context_->storage.get(), stack_trace_id);
        } else if (!stack_entries.empty()) {
            // Raw mode: format raw_stack_entries array
            // Use nr_stack_entries if available and valid, otherwise use all entries
            size_t count = (nr_stack_entries > 0 && nr_stack_entries < stack_entries.size()) 
                          ? nr_stack_entries : stack_entries.size();
            std::vector<uint64_t> limited_entries(stack_entries.begin(), stack_entries.begin() + count);
            stack_trace_id = FormatStackTraceArray(context_->storage.get(), limited_entries);
        }
        // Otherwise: stack_trace_id remains null (disabled)
        
        if (event_type_id == kNullStringId) {
            event_type_id = context_->storage->InternString("UNKNOWN");
        }
        
        ParseMemTrackerEvent(
            timestamp, tid, tracker_name_id, event_type_id, address, size, allocation_uid,
            process_name_id,
            has_tracepoint_pid ? std::make_optional(tracepoint_pid) : std::nullopt,
            has_tracepoint_timestamp ? std::make_optional(tracepoint_timestamp) : std::nullopt,
            stack_trace_id, memory_pool_id, device_name_id);
        return true;
    }
    
    // Extract fields for mem_tracker_hist_event
    if (is_mem_tracker_hist) {
        StringId hist_tracker_name_id = kNullStringId;
        uint64_t hist_address = 0;
        uint64_t hist_size = 0;
        uint64_t hist_allocation_uid = 0;
        StringId hist_alloc_process_id = kNullStringId;
        int32_t hist_alloc_pid = 0;
        uint64_t hist_alloc_timestamp = 0;
        StringId hist_alloc_stack_trace_id = kNullStringId;
        std::vector<uint64_t> hist_alloc_stack_entries;
        uint32_t hist_nr_alloc_stack_entries = 0;
        StringId hist_memory_pool_id = kNullStringId;
        StringId hist_device_name_id = kNullStringId;
        bool hist_freed = false;
        StringId hist_free_process_id = kNullStringId;
        int32_t hist_free_pid = 0;
        uint64_t hist_free_timestamp = 0;
        StringId hist_free_stack_trace_id = kNullStringId;
        std::vector<uint64_t> hist_free_stack_entries;
        uint32_t hist_nr_free_stack_entries = 0;
        
        for (auto it = evt.field(); it; ++it) {
            protos::pbzero::GenericFtraceEvent::Field::Decoder fld(*it);
            base::StringView field_name = fld.name();
            
            if (field_name == "freed") {
                if (fld.has_int_value()) {
                    hist_freed = (fld.int_value() != 0);
                } else if (fld.has_uint_value()) {
                    hist_freed = (fld.uint_value() != 0);
                }
                continue;
            }
            
            if (fld.has_int_value()) {
                if (field_name == "alloc_pid") {
                    hist_alloc_pid = static_cast<int32_t>(fld.int_value());
                } else if (field_name == "free_pid") {
                    hist_free_pid = static_cast<int32_t>(fld.int_value());
                } else if (field_name == "nr_alloc_raw_stack_entries") {
                    hist_nr_alloc_stack_entries = static_cast<uint32_t>(fld.int_value());
                } else if (field_name == "nr_free_raw_stack_entries") {
                    hist_nr_free_stack_entries = static_cast<uint32_t>(fld.int_value());
                }
            } else if (fld.has_uint_value()) {
                if (field_name == "address") {
                    hist_address = fld.uint_value();
                } else if (field_name == "size") {
                    hist_size = fld.uint_value();
                } else if (field_name == "allocation_uid") {
                    hist_allocation_uid = fld.uint_value();
                } else if (field_name == "alloc_timestamp") {
                    hist_alloc_timestamp = fld.uint_value();
                } else if (field_name == "free_timestamp") {
                    hist_free_timestamp = fld.uint_value();
                } else if (field_name == "nr_alloc_raw_stack_entries") {
                    hist_nr_alloc_stack_entries = static_cast<uint32_t>(fld.uint_value());
                } else if (field_name == "nr_free_raw_stack_entries") {
                    hist_nr_free_stack_entries = static_cast<uint32_t>(fld.uint_value());
                }
            } else if (fld.has_str_value()) {
                StringId value = context_->storage->InternString(fld.str_value());
                if (field_name == "tracker_name") {
                    hist_tracker_name_id = value;
                } else if (field_name == "alloc_process") {
                    hist_alloc_process_id = value;
                } else if (field_name == "alloc_symbolized_stack_trace") {
                    hist_alloc_stack_trace_id = value;
                } else if (field_name == "alloc_raw_stack_entries") {
                    // alloc_raw_stack_entries is sent as bytes (array of uint64_t)
                    ParseRawStackEntriesFromBytes(fld.str_value(), hist_nr_alloc_stack_entries, hist_alloc_stack_entries);
                } else if (field_name == "memory_pool") {
                    hist_memory_pool_id = value;
                } else if (field_name == "device_name") {
                    hist_device_name_id = value;
                } else if (field_name == "free_process") {
                    hist_free_process_id = value;
                } else if (field_name == "free_symbolized_stack_trace") {
                    hist_free_stack_trace_id = value;
                } else if (field_name == "free_raw_stack_entries") {
                    // free_raw_stack_entries is sent as bytes (array of uint64_t)
                    ParseRawStackEntriesFromBytes(fld.str_value(), hist_nr_free_stack_entries, hist_free_stack_entries);
                }
            }
        }
        
        // Infer mode from which fields are populated:
        // - If symbolized_stack_trace is present and non-empty, use it (symbolized mode)
        // - Else if raw_stack_entries is present and non-empty, use it (raw mode)
        // - Otherwise, no stack trace (disabled)
        
        // Format alloc stack trace
        if (!hist_alloc_stack_trace_id.is_null() && 
            context_->storage->GetString(hist_alloc_stack_trace_id).ToStdString() != "") {
            // Symbolized mode: format the symbolized string (replace commas with newlines)
            hist_alloc_stack_trace_id = FormatStackTraceString(context_->storage.get(), hist_alloc_stack_trace_id);
        } else if (!hist_alloc_stack_entries.empty()) {
            // Raw mode: format alloc_raw_stack_entries array
            // Use nr_alloc_stack_entries if available and valid, otherwise use all entries
            size_t count = (hist_nr_alloc_stack_entries > 0 && hist_nr_alloc_stack_entries < hist_alloc_stack_entries.size())
                          ? hist_nr_alloc_stack_entries : hist_alloc_stack_entries.size();
            std::vector<uint64_t> limited_entries(hist_alloc_stack_entries.begin(), hist_alloc_stack_entries.begin() + count);
            hist_alloc_stack_trace_id = FormatStackTraceArray(context_->storage.get(), limited_entries);
        }
        // Otherwise: hist_alloc_stack_trace_id remains null (disabled)
        
        // Format free stack trace
        if (!hist_free_stack_trace_id.is_null() && 
            context_->storage->GetString(hist_free_stack_trace_id).ToStdString() != "") {
            // Symbolized mode: format the symbolized string (replace commas with newlines)
            hist_free_stack_trace_id = FormatStackTraceString(context_->storage.get(), hist_free_stack_trace_id);
        } else if (!hist_free_stack_entries.empty()) {
            // Raw mode: format free_raw_stack_entries array
            // Use nr_free_stack_entries if available and valid, otherwise use all entries
            size_t count = (hist_nr_free_stack_entries > 0 && hist_nr_free_stack_entries < hist_free_stack_entries.size())
                          ? hist_nr_free_stack_entries : hist_free_stack_entries.size();
            std::vector<uint64_t> limited_entries(hist_free_stack_entries.begin(), hist_free_stack_entries.begin() + count);
            hist_free_stack_trace_id = FormatStackTraceArray(context_->storage.get(), limited_entries);
        }
        // Otherwise: hist_free_stack_trace_id remains null (disabled)
        
        ParseMemTrackerHistEvent(
            timestamp, tid, hist_tracker_name_id, hist_address, hist_size, hist_allocation_uid,
            hist_alloc_process_id, hist_alloc_pid, hist_alloc_timestamp,
            hist_alloc_stack_trace_id, hist_memory_pool_id, hist_device_name_id,
            hist_freed,
            hist_free_process_id, hist_free_pid, hist_free_timestamp,
            hist_free_stack_trace_id);
        return true;
    }
    
    // For other memory tracker events (e.g., cma_heap_stats), forward to generic handler
    ParseMemoryTrackerEvent(timestamp, event_name, evt);
    return true;
}

bool HailoMemoryTracker::ParseMemoryTrackerEventFromTypedFtrace(int64_t timestamp,
                                                                uint32_t tid,
                                                                base::StringView event_name,
                                                                protozero::ConstBytes blob,
                                                                const FtraceMessageDescriptor* descriptor,
                                                                StringId /* message_name_id */,
                                                                const StringId* field_name_ids) {
    // Check if this is a memory tracker event
    if (!base::StartsWith(event_name.ToStdString(), "mem_tracker") && 
        event_name != "mem_tracker_event" && 
        event_name != "mem_tracker_hist_event" &&
        event_name != "cma_heap_stats") {
        return false;
    }
    
    // Check for hist events FIRST to avoid parsing them as regular events
    bool is_mem_tracker_hist = (event_name == "mem_tracker_hist_event" ||
                                event_name == "mem_tracker/mem_tracker_hist_event");
    
    // Check if this is mem_tracker_event (but NOT hist events)
    bool is_mem_tracker = !is_mem_tracker_hist &&
                          (event_name == "mem_tracker_event" ||
                           event_name == "mem_tracker/mem_tracker_event");
    
    // Extract fields for mem_tracker_event
    if (is_mem_tracker) {
        StringId tracker_name_id = kNullStringId;
        StringId memory_pool_id = kNullStringId;
        StringId device_name_id = kNullStringId;
        StringId event_type_id = kNullStringId;
        uint64_t address = 0;
        uint64_t size = 0;
        uint64_t allocation_uid = 0;
        StringId process_name_id = kNullStringId;
        int32_t tracepoint_pid = 0;
        bool has_tracepoint_pid = false;
        uint64_t tracepoint_timestamp = 0;
        bool has_tracepoint_timestamp = false;
        StringId stack_trace_id = kNullStringId;
        
        // Create a new decoder to extract fields
        protozero::ProtoDecoder decoder(blob);
        for (auto fld = decoder.ReadField(); fld.valid(); fld = decoder.ReadField()) {
            uint32_t field_id = fld.id();
            if (PERFETTO_UNLIKELY(field_id >= kMaxFtraceEventFields)) {
                continue;
            }
            
            ProtoSchemaType type = descriptor->fields[field_id].type;
            StringId name_id = field_name_ids[field_id];
            base::StringView field_name = context_->storage->GetString(name_id);
            
            if (field_name == "address" || field_name == "size" || field_name == "allocation_uid") {
                if (type == ProtoSchemaType::kUint32 || type == ProtoSchemaType::kUint64 ||
                    type == ProtoSchemaType::kFixed32 || type == ProtoSchemaType::kFixed64) {
                    uint64_t val = fld.as_uint64();
                    if (field_name == "address") address = val;
                    else if (field_name == "size") size = val;
                    else if (field_name == "allocation_uid") allocation_uid = val;
                } else if (type == ProtoSchemaType::kInt32 || type == ProtoSchemaType::kInt64) {
                    uint64_t val = static_cast<uint64_t>(fld.as_int64());
                    if (field_name == "address") address = val;
                    else if (field_name == "size") size = val;
                    else if (field_name == "allocation_uid") allocation_uid = val;
                }
            } else if (field_name == "pid") {
                if (type == ProtoSchemaType::kInt32 || type == ProtoSchemaType::kInt64) {
                    tracepoint_pid = static_cast<int32_t>(fld.as_int64());
                    has_tracepoint_pid = true;
                }
            } else if (field_name == "timestamp") {
                if (type == ProtoSchemaType::kUint32 || type == ProtoSchemaType::kUint64 ||
                    type == ProtoSchemaType::kFixed32 || type == ProtoSchemaType::kFixed64) {
                    tracepoint_timestamp = fld.as_uint64();
                    has_tracepoint_timestamp = true;
                }
            } else if (field_name == "tracker_name" || field_name == "memory_pool" ||
                       field_name == "device_name" || field_name == "event_type" || 
                       field_name == "process" || field_name == "stack_trace") {
                if (type == ProtoSchemaType::kString || type == ProtoSchemaType::kBytes) {
                    StringId value = context_->storage->InternString(fld.as_string());
                    if (field_name == "tracker_name") tracker_name_id = value;
                    else if (field_name == "memory_pool") memory_pool_id = value;
                    else if (field_name == "device_name") device_name_id = value;
                    else if (field_name == "event_type") event_type_id = value;
                    else if (field_name == "process") process_name_id = value;
                    else if (field_name == "stack_trace") stack_trace_id = value;
                }
            }
        }
        
        if (event_type_id == kNullStringId) {
            event_type_id = context_->storage->InternString("UNKNOWN");
        }
        
        ParseMemTrackerEvent(
            timestamp, tid, tracker_name_id, event_type_id, address, size, allocation_uid,
            process_name_id,
            has_tracepoint_pid ? std::make_optional(tracepoint_pid) : std::nullopt,
            has_tracepoint_timestamp ? std::make_optional(tracepoint_timestamp) : std::nullopt,
            stack_trace_id, memory_pool_id, device_name_id);
        return true;
    }
    
    // Extract fields for mem_tracker_hist_event
    if (is_mem_tracker_hist) {
        StringId hist_tracker_name_id = kNullStringId;
        uint64_t hist_address = 0;
        uint64_t hist_size = 0;
        uint64_t hist_allocation_uid = 0;
        StringId hist_alloc_process_id = kNullStringId;
        int32_t hist_alloc_pid = 0;
        uint64_t hist_alloc_timestamp = 0;
        StringId hist_alloc_stack_trace_id = kNullStringId;
        StringId hist_memory_pool_id = kNullStringId;
        StringId hist_device_name_id = kNullStringId;
        bool hist_freed = false;
        StringId hist_free_process_id = kNullStringId;
        int32_t hist_free_pid = 0;
        uint64_t hist_free_timestamp = 0;
        StringId hist_free_stack_trace_id = kNullStringId;
        
        // Create a new decoder to extract fields
        protozero::ProtoDecoder decoder(blob);
        for (auto fld = decoder.ReadField(); fld.valid(); fld = decoder.ReadField()) {
            uint32_t field_id = fld.id();
            if (PERFETTO_UNLIKELY(field_id >= kMaxFtraceEventFields)) {
                continue;
            }
            
            ProtoSchemaType type = descriptor->fields[field_id].type;
            StringId name_id = field_name_ids[field_id];
            base::StringView field_name = context_->storage->GetString(name_id);
            
            if (field_name == "address" || field_name == "size" || field_name == "allocation_uid" ||
                field_name == "alloc_timestamp" || field_name == "free_timestamp") {
                if (type == ProtoSchemaType::kUint32 || type == ProtoSchemaType::kUint64 ||
                    type == ProtoSchemaType::kFixed32 || type == ProtoSchemaType::kFixed64) {
                    uint64_t val = fld.as_uint64();
                    if (field_name == "address") hist_address = val;
                    else if (field_name == "size") hist_size = val;
                    else if (field_name == "allocation_uid") hist_allocation_uid = val;
                    else if (field_name == "alloc_timestamp") hist_alloc_timestamp = val;
                    else if (field_name == "free_timestamp") hist_free_timestamp = val;
                }
            } else if (field_name == "alloc_pid" || field_name == "free_pid") {
                if (type == ProtoSchemaType::kInt32 || type == ProtoSchemaType::kInt64) {
                    int32_t val = static_cast<int32_t>(fld.as_int64());
                    if (field_name == "alloc_pid") hist_alloc_pid = val;
                    else if (field_name == "free_pid") hist_free_pid = val;
                }
            } else if (field_name == "freed") {
                if (type == ProtoSchemaType::kBool || type == ProtoSchemaType::kInt32 || type == ProtoSchemaType::kInt64) {
                    hist_freed = (fld.as_int64() != 0);
                }
            } else if (field_name == "tracker_name" || field_name == "alloc_process" ||
                       field_name == "alloc_stack_trace" || field_name == "memory_pool" ||
                       field_name == "device_name" || field_name == "free_process" || 
                       field_name == "free_stack_trace") {
                if (type == ProtoSchemaType::kString || type == ProtoSchemaType::kBytes) {
                    StringId value = context_->storage->InternString(fld.as_string());
                    if (field_name == "tracker_name") hist_tracker_name_id = value;
                    else if (field_name == "alloc_process") hist_alloc_process_id = value;
                    else if (field_name == "alloc_stack_trace") hist_alloc_stack_trace_id = value;
                    else if (field_name == "memory_pool") hist_memory_pool_id = value;
                    else if (field_name == "device_name") hist_device_name_id = value;
                    else if (field_name == "free_process") hist_free_process_id = value;
                    else if (field_name == "free_stack_trace") hist_free_stack_trace_id = value;
                }
            }
        }
        
        // Format stack traces if we have strings (symbolized mode)
        if (!hist_alloc_stack_trace_id.is_null()) {
            hist_alloc_stack_trace_id = FormatStackTraceString(context_->storage.get(), hist_alloc_stack_trace_id);
        }
        if (!hist_free_stack_trace_id.is_null()) {
            hist_free_stack_trace_id = FormatStackTraceString(context_->storage.get(), hist_free_stack_trace_id);
        }
        // Note: Array handling for typed ftrace may need additional work
        
        ParseMemTrackerHistEvent(
            timestamp, tid, hist_tracker_name_id, hist_address, hist_size, hist_allocation_uid,
            hist_alloc_process_id, hist_alloc_pid, hist_alloc_timestamp,
            hist_alloc_stack_trace_id, hist_memory_pool_id, hist_device_name_id,
            hist_freed,
            hist_free_process_id, hist_free_pid, hist_free_timestamp,
            hist_free_stack_trace_id);
        return true;
    }
    
    // For typed events, we can't easily convert to GenericFtraceEvent::Decoder
    // So we skip other memory tracker events for typed path (they should come through generic path)
    PERFETTO_DLOG("HailoMemoryTracker: Typed memory tracker events not yet supported: %.*s",
                  static_cast<int>(event_name.size()), event_name.data());
    return false;
}

void HailoMemoryTracker::ParseMemoryTrackerEvent(int64_t timestamp,
                                                  base::StringView event_name,
                                                  const protos::pbzero::GenericFtraceEvent::Decoder& evt) {
    // Extract all fields from the event
    std::unordered_map<std::string, StringId> string_fields;
    std::unordered_map<std::string, uint64_t> uint_fields;
    
    for (auto it = evt.field(); it; ++it) {
        protos::pbzero::GenericFtraceEvent::Field::Decoder fld(*it);
        base::StringView field_name = fld.name();
        
        if (fld.has_str_value()) {
            StringId str_value = context_->storage->InternString(fld.str_value());
            string_fields[field_name.ToStdString()] = str_value;
        } else if (fld.has_uint_value()) {
            uint_fields[field_name.ToStdString()] = fld.uint_value();
        } else if (fld.has_int_value()) {
            uint_fields[field_name.ToStdString()] = static_cast<uint64_t>(fld.int_value());
        }
    }
    
    // Forward to all registered extensions
    std::lock_guard<std::mutex> lock(extensions_mutex_);
    PERFETTO_DLOG("HailoMemoryTracker::ParseMemoryTrackerEvent: Forwarding to %zu extensions for event %.*s",
                 extensions_.size(), static_cast<int>(event_name.size()), event_name.data());
    for (MemoryTrackerExtension* extension : extensions_) {
        if (extension) {
            extension->ParseEvent(timestamp, event_name, string_fields, uint_fields);
        }
    }
}

} // namespace trace_processor
} // namespace perfetto

