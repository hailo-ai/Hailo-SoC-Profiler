/*
 * Copyright (c) 2025 Hailo Technologies Ltd. All rights reserved.
 */
#ifndef INCLUDE_PERFETTO_BASE_HAILO_H_
#define INCLUDE_PERFETTO_BASE_HAILO_H_

#include <stdint.h>

namespace perfetto::base {

enum class HailoSocType {
    HAILO15,
    HAILO15L
};

uint64_t GetScuTimerOffset();
uint32_t GetScuClockId();
HailoSocType GetHailoSocType();

}  // namespace perfetto::base

 #endif  // INCLUDE_PERFETTO_BASE_HAILO_H_
