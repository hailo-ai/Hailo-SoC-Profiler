/*
 * Copyright (c) 2026 Hailo Technologies Ltd. All rights reserved.
 */

 #include "src/trace_processor/importers/ftrace/hailo_cma_tracker.h"

#include "perfetto/base/logging.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/common/tracks.h"
#include "src/trace_processor/importers/ftrace/hailo_memory_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/variadic.h"
#include <mutex>
#include <set>

namespace perfetto {
namespace trace_processor {

namespace {
// Blueprint for CMA heap stats parent counter track (creates "CMA Heap Stats" group)
static constexpr auto kCmaHeapStatsParentBlueprint =
    tracks::CounterBlueprint(
        "CmaHeapStats",
        tracks::StaticUnitBlueprint("pages"),
        tracks::DimensionBlueprints(
            tracks::StringDimensionBlueprint("tracker_name")  // Creates "CMA" group
        ),
        tracks::StaticNameBlueprint("CMA Heap Stats")
    );

// Blueprint for CMA heap stats events (markers with heap stats data)
static constexpr auto kCmaHeapStatsEventsBlueprint =
    tracks::SliceBlueprint(
        "CmaHeapStatsEvents",
        tracks::DimensionBlueprints(
            tracks::StringDimensionBlueprint("tracker_name"),  // Creates "CMA" group
            tracks::StringDimensionBlueprint("heap_name")      // Creates per-heap tracks
        ),
        tracks::FnNameBlueprint([](base::StringView /* tracker_name_dim */, base::StringView /* heap_name */) {
            // Format: "Events" (heap_name is shown in subgroup title)
            base::StackString<256> name("Events");
            return name;
        })
    );

// Blueprint for CMA heap stats counter track with tracker_name, heap_name, and metric dimensions
static constexpr auto kCmaHeapStatsBlueprint =
    tracks::CounterBlueprint(
        "CmaHeapStats",
        tracks::StaticUnitBlueprint("pages"),
        tracks::DimensionBlueprints(
            tracks::StringDimensionBlueprint("tracker_name"),  // Creates "CMA" group
            tracks::StringDimensionBlueprint("heap_name"),     // Creates per-heap tracks (e.g., "cma-linux,cma", "cma-hailo_media")
            tracks::StringDimensionBlueprint("metric")        // Creates per-metric tracks (e.g., "used", "free", "max_chunk")
        ),
        tracks::FnNameBlueprint([](base::StringView /* tracker_name_dim */, base::StringView /* heap_name */, base::StringView metric) {
            // Format: "metric" (heap_name is shown in subgroup title)
            base::StackString<256> name("%.*s",
                                       static_cast<int>(metric.size()), metric.data());
            return name;
        })
    );
} // namespace

HailoCmaTracker::HailoCmaTracker(TraceProcessorContext* context)
    : context_(context) {
}

HailoCmaTracker* HailoCmaTracker::GetOrCreate(TraceProcessorContext* context) {
    if (!context->hailo_cma_tracker) {
        PERFETTO_DLOG("HailoCmaTracker: Creating new instance and registering with HailoMemoryTracker");
        context->hailo_cma_tracker.reset(new HailoCmaTracker(context));
        // Register this tracker as an extension of HailoMemoryTracker
        HailoMemoryTracker::GetOrCreate(context)->RegisterExtension(
            static_cast<HailoCmaTracker*>(context->hailo_cma_tracker.get()));
        PERFETTO_DLOG("HailoCmaTracker: Successfully registered extension");
    }
    return static_cast<HailoCmaTracker*>(context->hailo_cma_tracker.get());
}

void HailoCmaTracker::ParseEvent(int64_t timestamp,
                                 base::StringView event_name,
                                 const std::unordered_map<std::string, StringId>& string_fields,
                                 const std::unordered_map<std::string, uint64_t>& uint_fields) {
    PERFETTO_DLOG("HailoCmaTracker::ParseEvent called with event_name=%.*s", 
                 static_cast<int>(event_name.size()), event_name.data());
    
    // Only handle cma_heap_stats events
    if (event_name != "cma_heap_stats" && event_name != "mem_tracker/cma_heap_stats") {
        return;
    }
    
    PERFETTO_DLOG("HailoCmaTracker: Processing cma_heap_stats event");
    
    // Extract fields for CMA heap stats
    auto heap_name_it = string_fields.find("heap_name");
    auto total_pages_it = uint_fields.find("total_pages");
    auto used_pages_it = uint_fields.find("used_pages");
    auto free_pages_it = uint_fields.find("free_pages");
    auto max_chunk_pages_it = uint_fields.find("max_chunk_pages");
    
    if (heap_name_it == string_fields.end() || heap_name_it->second.is_null()) {
        PERFETTO_DLOG("HailoCmaTracker: cma_heap_stats event missing heap_name field");
        return;
    }
    
    StringId heap_name = heap_name_it->second;
    uint64_t total_pages = (total_pages_it != uint_fields.end()) ? total_pages_it->second : 0;
    uint64_t used_pages = (used_pages_it != uint_fields.end()) ? used_pages_it->second : 0;
    uint64_t free_pages = (free_pages_it != uint_fields.end()) ? free_pages_it->second : 0;
    uint64_t max_chunk_pages = (max_chunk_pages_it != uint_fields.end()) ? max_chunk_pages_it->second : 0;
    
    ParseCmaHeapStats(timestamp, heap_name, total_pages, used_pages, free_pages, max_chunk_pages);
}

void HailoCmaTracker::ParseCmaHeapStats(int64_t timestamp,
                                   StringId heap_name,
                                   uint64_t total_pages,
                                   uint64_t used_pages,
                                   uint64_t free_pages,
                                   uint64_t max_chunk_pages) {
    if (heap_name.is_null()) {
        PERFETTO_DLOG("HailoCmaTracker: heap_name is null, skipping CMA heap stats");
        return;
    }
    
    // Use "CMA" as the tracker_name for grouping
    StringId tracker_name_id = context_->storage->InternString("CMA");
    base::StringView tracker_name_str = context_->storage->GetString(tracker_name_id);
    base::StringView heap_name_str = context_->storage->GetString(heap_name);
    
    // Create parent "CMA Heap Stats" track
    TrackId heap_stats_parent_track_id = context_->track_tracker->InternTrack(
        kCmaHeapStatsParentBlueprint,
        tracks::Dimensions(tracker_name_str)
    );
    
    // Push a dummy counter value to the parent track so it appears in _counter_track_summary
    // (parent tracks without counters don't appear in the summary)
    {
        std::lock_guard<std::mutex> lock(heap_stats_parent_initialized_mutex_);
        if (heap_stats_parent_initialized_.find(heap_stats_parent_track_id) == heap_stats_parent_initialized_.end()) {
            context_->event_tracker->PushCounter(
                timestamp,
                0.0, // Dummy value - just to make the track visible
                heap_stats_parent_track_id
            );
            heap_stats_parent_initialized_.insert(heap_stats_parent_track_id);
        }
    }
    
    // Create Events track for this heap (markers with heap stats data)
    TrackId events_track_id = context_->track_tracker->InternTrack(
        kCmaHeapStatsEventsBlueprint,
        tracks::Dimensions(tracker_name_str, heap_name_str)
    );
    
    // Set parent_id for Events track
    auto* track_table = context_->storage->mutable_track_table();
    auto events_track_row = track_table->FindById(events_track_id);
    if (events_track_row) {
        events_track_row->set_parent_id(heap_stats_parent_track_id);
    }
    
    // Create a marker event with heap stats as arguments
    StringId marker_name_id = context_->storage->InternString("Heap Stats Update");
    StringId slice_category = context_->storage->InternString("cma_heap_stats");
    
    (void)context_->slice_tracker->Scoped(
        timestamp,
        events_track_id,
        slice_category,
        marker_name_id,
        0, // Duration 0 for instant event
        [this, total_pages, used_pages, free_pages, max_chunk_pages](ArgsTracker::BoundInserter* inserter) {
            inserter->AddArg(context_->storage->InternString("total_pages"),
                           Variadic::UnsignedInteger(total_pages));
            inserter->AddArg(context_->storage->InternString("used_pages"),
                           Variadic::UnsignedInteger(used_pages));
            inserter->AddArg(context_->storage->InternString("free_pages"),
                           Variadic::UnsignedInteger(free_pages));
            inserter->AddArg(context_->storage->InternString("max_chunk_pages"),
                           Variadic::UnsignedInteger(max_chunk_pages));
        }
    );
    
    // Get track table for setting parent_id
    // Create counter tracks for each metric (used, free, max_chunk)
    StringId used_metric_id = context_->storage->InternString("used");
    StringId free_metric_id = context_->storage->InternString("free");
    StringId max_chunk_metric_id = context_->storage->InternString("max_chunk");
    
    base::StringView used_metric_str = context_->storage->GetString(used_metric_id);
    base::StringView free_metric_str = context_->storage->GetString(free_metric_id);
    base::StringView max_chunk_metric_str = context_->storage->GetString(max_chunk_metric_id);
    
    // Create tracks for each metric
    TrackId used_track_id = context_->track_tracker->InternTrack(
        kCmaHeapStatsBlueprint,
        tracks::Dimensions(tracker_name_str, heap_name_str, used_metric_str)
    );
    TrackId free_track_id = context_->track_tracker->InternTrack(
        kCmaHeapStatsBlueprint,
        tracks::Dimensions(tracker_name_str, heap_name_str, free_metric_str)
    );
    TrackId max_chunk_track_id = context_->track_tracker->InternTrack(
        kCmaHeapStatsBlueprint,
        tracks::Dimensions(tracker_name_str, heap_name_str, max_chunk_metric_str)
    );
    
    // Set parent_id for all metric tracks
    auto used_track_row = track_table->FindById(used_track_id);
    if (used_track_row) {
        used_track_row->set_parent_id(heap_stats_parent_track_id);
    }
    auto free_track_row = track_table->FindById(free_track_id);
    if (free_track_row) {
        free_track_row->set_parent_id(heap_stats_parent_track_id);
    }
    auto max_chunk_track_row = track_table->FindById(max_chunk_track_id);
    if (max_chunk_track_row) {
        max_chunk_track_row->set_parent_id(heap_stats_parent_track_id);
    }
    
    // Push counter values
    context_->event_tracker->PushCounter(
        timestamp,
        static_cast<double>(used_pages),
        used_track_id
    );
    context_->event_tracker->PushCounter(
        timestamp,
        static_cast<double>(free_pages),
        free_track_id
    );
    context_->event_tracker->PushCounter(
        timestamp,
        static_cast<double>(max_chunk_pages),
        max_chunk_track_id
    );
}

} // namespace trace_processor
} // namespace perfetto

