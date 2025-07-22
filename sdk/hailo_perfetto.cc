/*
 * Copyright (c) 2025 Hailo Technologies Ltd. All rights reserved.
 */

 // we use hailo_perfetto.cc as the source file for the SDK instead of perfetto.cc
 // and internally we include it this way
 // that way we can use unexported inlined methods like GetThreadName
#include "perfetto.cc"
#include "hailo_perfetto.h"

namespace perfetto {

// static
ThreadSubTrack ThreadSubTrack::Current(Track parent)
{
  return ThreadSubTrack(parent, ThreadTrack::Current().tid);
}

perfetto::protos::gen::TrackDescriptor ThreadSubTrack::Serialize() const
{
    auto desc = Track::Serialize();
    std::string thread_name = "";
    if (!base::GetThreadName(thread_name))
    {
        thread_name = "<unknown>";
    }
    desc.set_name(thread_name + " " + std::to_string(tid));
    return desc;
}

void ThreadSubTrack::Serialize(protos::pbzero::TrackDescriptor *desc) const
{
    auto bytes = Serialize().SerializeAsString();
    desc->AppendRawProtoBytes(bytes.data(), bytes.size());
}

// TODO: currently only the media library uses libperfetto
//       but once we have more components that use perfetto,
//       we want to use this file to initialize tracing globally
//       which is something that we currently do in the media library

}  // namespace perfetto
