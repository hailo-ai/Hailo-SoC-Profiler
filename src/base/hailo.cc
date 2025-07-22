/*
 * Copyright (c) 2025 Hailo Technologies Ltd. All rights reserved.
 */
#include "perfetto/base/hailo.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/public/fnv1a.h"
#include <string>
#include <optional>
#include <mutex>

namespace perfetto::base {

constexpr uint32_t scu_clock_id = PERFETTO_STATIC_CAST(uint32_t, PerfettoFnv1a("ai.hailo.scu_clock")) | 0x80000000;

static uint64_t ReadScuTimerOffset() {
    auto offset = GetFileValue("/sys/devices/soc0/boot_info/scu_to_ap_timer_offset_ns");
    if (!offset.has_value()) {
        PERFETTO_FATAL("Failed to get SCU timer offset");
    }
    return offset.value();
}

uint64_t GetScuTimerOffset() {
    static const uint64_t scu_timer_offset = ReadScuTimerOffset();
    return scu_timer_offset;
}

uint32_t GetScuClockId() {
    return scu_clock_id;
}

static HailoSocType ReadHailoSocType() {
    std::string soc_type_str;
    if (!ReadFile("/sys/devices/soc0/machine", &soc_type_str)) {
        PERFETTO_FATAL("Failed to read Hailo SoC type");
    }

    if (soc_type_str == "Hailo-15\n") {
        return HailoSocType::HAILO15;
    } else if (soc_type_str == "Hailo-15l\n") {
        return HailoSocType::HAILO15L;
    } else {
        PERFETTO_FATAL("Unknown Hailo SoC type: %s", soc_type_str.c_str());
    }
}

HailoSocType GetHailoSocType() {
    static const HailoSocType soc_type = ReadHailoSocType();
    return soc_type;
}

}  // namespace perfetto::base
