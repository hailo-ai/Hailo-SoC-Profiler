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

class HailoPerfettoInitializer
{
  public:
    HailoPerfettoInitializer()
    {
        TracingInitArgs args;
        args.backends = kSystemBackend;
        Tracing::Initialize(args);
    }

    ~HailoPerfettoInitializer()
    {
        Tracing::Shutdown();
    }
};

// This static instance ensures the constructor runs during libperfetto load
static HailoPerfettoInitializer _hailo_perfetto_initializer;

}  // namespace perfetto
