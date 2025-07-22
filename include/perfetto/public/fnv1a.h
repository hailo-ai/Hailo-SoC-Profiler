/*
 * Copyright (C) 2023 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * This file has been modified by Hailo Technologies Ltd.
 */

#ifndef INCLUDE_PERFETTO_PUBLIC_FNV1A_H_
#define INCLUDE_PERFETTO_PUBLIC_FNV1A_H_

#include <stdint.h>
#include <stdlib.h>
#include <string_view>

#include "perfetto/public/compiler.h"

static inline constexpr uint64_t PerfettoFnv1a(std::string_view str) {
  const uint64_t kFnv1a64OffsetBasis = 0xcbf29ce484222325;
  const uint64_t kFnv1a64Prime = 0x100000001b3;

  uint64_t value = kFnv1a64OffsetBasis;

  for (char c : str) {
    value = (value ^ PERFETTO_STATIC_CAST(uint8_t, c)) * kFnv1a64Prime;
  }
  return value;
}

static inline uint64_t PerfettoFnv1a(const void* data, size_t size) {
  return PerfettoFnv1a(std::string_view(PERFETTO_STATIC_CAST(const char *, data), size));
}

#endif  // INCLUDE_PERFETTO_PUBLIC_FNV1A_H_
