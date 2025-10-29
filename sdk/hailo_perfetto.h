/*
 * Copyright (c) 2025 Hailo Technologies Ltd. All rights reserved.
 */

#ifndef SDK_HAILO_PERFETTO_H_
#define SDK_HAILO_PERFETTO_H_

#include "perfetto.h"

namespace perfetto {

// A track for a thread that is not a direct subtrack of a process track.
struct PERFETTO_EXPORT_COMPONENT ThreadSubTrack : public Track
{
  const base::PlatformThreadId tid;

  void Serialize(protos::pbzero::TrackDescriptor *) const;
  protos::gen::TrackDescriptor Serialize() const;
  static ThreadSubTrack Current(Track parent);

private:
  static constexpr uint64_t kThreadSubTrackMagic = 0xD1DDA68EEA27E4DAul;
  ThreadSubTrack(Track parent, base::PlatformThreadId tid_)
  : Track(kThreadSubTrackMagic ^ static_cast<uint64_t>(tid_), parent), tid(tid_) {}
};

template <typename TrackEventT>
class HailoTrackEventInitializer
{
public:
    template <typename... TrackTypes>
    HailoTrackEventInitializer(TrackTypes&&... track)
    {
        TrackEventT::Register();
        // I suspect we need this only because of a bug in Perfetto in the function WriteTrackDescriptorIfNeeded
        // that does not emit a track_descriptor if the track is not in the TrackRegistry.
        // I think the function should have dynamically added the tracks to the registry instead of
        // asking the library users to manually add the tracks by calling SetTrackDescriptor...
        (TrackEventT::SetTrackDescriptor(track, track.Serialize()), ...);
    }

    ~HailoTrackEventInitializer()
    {
        TrackEventT::Flush();
    }
};

}  // namespace perfetto


// params: namespace, and categories
#define HAILO_PERFETTO_DEFINE_CATEGORIES(ns, ...) \
    PERFETTO_DEFINE_CATEGORIES_IN_NAMESPACE(ns, __VA_ARGS__); \
    PERFETTO_USE_CATEGORIES_FROM_NAMESPACE(ns);


// params: namespace, and optionally custom tracks to initialize
#define HAILO_PERFETTO_INITIALIZER(ns, ...) \
    PERFETTO_TRACK_EVENT_STATIC_STORAGE_IN_NAMESPACE(ns); \
    namespace ns { static perfetto::HailoTrackEventInitializer<TrackEvent> hailo_track_event_initializer { __VA_ARGS__ }; };

#endif  // SDK_HAILO_PERFETTO_H_