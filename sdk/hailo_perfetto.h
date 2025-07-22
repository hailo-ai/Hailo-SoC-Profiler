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

}  // namespace perfetto

#endif  // SDK_HAILO_PERFETTO_H_