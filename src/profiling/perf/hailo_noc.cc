/*
 * Copyright (c) 2025 Hailo Technologies Ltd. All rights reserved.
 */
#include <stdint.h>

#include "src/profiling/perf/event_config.h"
#include "src/profiling/perf/hailo_noc.h"

#include "protos/perfetto/common/builtin_clock.pbzero.h"
#include "protos/perfetto/common/perf_events.gen.h"
#include "protos/perfetto/trace/clock_snapshot.pbzero.h"
#include "protos/perfetto/trace/profiling/profile_packet.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

#include "perfetto/base/hailo.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/string_utils.h"

namespace perfetto::profiling {

using protos::gen::PerfEvents;

static constexpr uint32_t kDefaultSampleTimeUs = 100;
// Default values that mean not filtering by the respective field
static constexpr uint32_t kDefaultRouteIdBase = 0;
static constexpr uint32_t kDefaultRouteIdMask = 0;
static constexpr uint32_t kDefaultOpcode = 0xF;
static constexpr uint32_t kReadOpcode = 1;
static constexpr uint32_t kWriteOpcode = 2;
static constexpr uint64_t kDefaultAddressBase = 0;
static constexpr uint32_t kDefaultLength = 0xF;
static constexpr uint32_t kDefaultUrgency = 0;
static constexpr uint32_t kDefaultWindowSize = 0x3F;
static constexpr uint32_t kDefaultStatus = 3;
static constexpr std::string_view kDefaultMode = "filter";

struct noc_sample_h15 {
  uint32_t noc_counters[4];
  uint32_t dsm_rx_counter;
  uint32_t dsm_tx_counter;
  uint32_t csm_counter;
  uint64_t timestamp;
  bool triggered;
} PERFETTO_PACKED;

struct noc_sample_h15l {
  uint32_t noc_counters[5];
  uint32_t dsm_rx_counter;
  uint32_t dsm_tx_counter;
  uint32_t csm_counter;
  uint64_t timestamp;
  bool triggered;
} PERFETTO_PACKED;

struct NocChipSettings {
  uint32_t route_id_mask_filter;
  const std::unordered_map<std::string, uint32_t> filter_route_base_names;
};

static const struct NocChipSettings Hailo15NocSettings = {
    /* route_id_mask_filter = */ 0x1f8000,
    /* filter_route_base_names = */ {
        {"dsp_idma_ro_0", 0},
        {"dsp_idma_ro_1", 0x8000},
        {"dsp_idma_ro_2", 0x10000},
        {"dsp_idma_wo_0", 0x20000},
        {"dsp_idma_wo_1", 0x28000},
        {"dsp_idma_wo_2", 0x30000},
        {"dram_dma0_ddrbus", 0x40000},
        {"dram_dma0_ddrbus_lp", 0x48000},
        {"dram_dma1_ddrbus", 0x50000},
        {"dram_dma1_ddrbus_lp", 0x58000},
        {"dram_dma2_ddrbus", 0x60000},
        {"dram_dma2_ddrbus_lp", 0x68000},
        {"ap_cluster_ace", 0x70000},
        {"csi_rx0", 0x78000},
        {"csi_rx1", 0x80000},
        {"csi_tx0", 0x88000},
        {"dsi_tx0", 0x88000},  // alias
        {"debug_etr", 0x90000},
        {"dram_dma0_fastbus_ro", 0x98000},
        {"dram_dma0_fastbus_wo", 0xa0000},
        {"dram_dma1_fastbus_ro", 0xa8000},
        {"dram_dma1_fastbus_wo", 0xb0000},
        {"dram_dma2_fastbus_ro", 0xb8000},
        {"dram_dma2_fastbus_wo", 0xc0000},
        {"dram_dma3_ddrbus_ro", 0xc8000},
        {"dram_dma3_ddrbus_wo", 0xd0000},
        {"dram_dma_ddrbus_desc", 0xd8000},
        {"dsp_ddrbus", 0xe0000},
        {"ethernet", 0xe8000},
        {"gic_master", 0xf0000},
        {"h265", 0xf8000},
        {"isp_ddrbus0", 0x100000},
        {"isp_mcm", 0x100000},  // alias
        {"isp_ddrbus1", 0x108000},
        {"isp_sp2", 0x108000},  // alias
        {"isp_fastbus", 0x110000},
        {"isp_mp", 0x110000},  // alias
        {"main2fast", 0x118000},
        {"noc_firewall_service", 0x120000},
        {"noc_pcie_firewall_srvice", 0x128000},
        {"noc_service", 0x130000},
        {"pcie_aux", 0x138000},
        {"pcie_desc", 0x140000},
        {"pcie_main", 0x148000},
        {"sdio1", 0x150000},
        {"usb", 0x158000},
    }};

static const struct NocChipSettings Hailo15LNocSettings = {
    /* route_id_mask_filter = */ 0xfc0000,
    /* filter_route_base_names = */ {{"dram_dma0_ddrbus", 0x0},
                                     {"dram_dma0_ddrbus_lp", 0x40000},
                                     {"dram_dma1_ddrbus", 0x80000},
                                     {"dram_dma1_ddrbus_lp", 0xc0000},
                                     {"ap_cluster_ace", 0x100000},
                                     {"csi_rx0", 0x140000},
                                     {"csi_rx1", 0x180000},
                                     {"debug_etr", 0x1c0000},
                                     {"dram_dma0_fastbus_ro", 0x200000},
                                     {"dram_dma0_fastbus_wo", 0x240000},
                                     {"dram_dma1_fastbus_ro", 0x280000},
                                     {"dram_dma1_fastbus_wo", 0x2c0000},
                                     {"dram_dma2_ddrbus_ro", 0x300000},
                                     {"dram_dma2_ddrbus_wo", 0x340000},
                                     {"dram_dma_ddrbus_desc", 0x380000},
                                     {"dsi_tx0", 0x3c0000},
                                     {"dsp_ddrbus", 0x400000},
                                     {"dsp_idma", 0x440000},
                                     {"dwe_ro", 0x480000},
                                     {"dwe_wo", 0x4c0000},
                                     {"ethernet", 0x500000},
                                     {"gic_master", 0x540000},
                                     {"h265", 0x580000},
                                     {"isp_mcm", 0x5c0000},
                                     {"isp_mp", 0x600000},
                                     {"isp_sp2", 0x640000},
                                     {"main2fast", 0x680000},
                                     {"noc_firewall_service", 0x6c0000},
                                     {"noc_pcie_firewall_service", 0x700000},
                                     {"noc_service", 0x740000},
                                     {"pcie_aux", 0x780000},
                                     {"pcie_desc", 0x7c0000},
                                     {"pcie_main", 0x800000},
                                     {"sdio1", 0x840000},
                                     {"usb", 0x880000},
                                     {"xspi_master", 0x8c0000}}};

static const struct NocChipSettings& nocSettings = []() {
  switch (base::GetHailoSocType()) {
    case base::HailoSocType::HAILO15:
      return Hailo15NocSettings;
    case base::HailoSocType::HAILO15L:
      return Hailo15LNocSettings;
    default:
      PERFETTO_FATAL("Unknown Hailo SoC type");
  }
}();

struct NocCounterSettings {
  bool is_configurable;
  bool is_noc_customizable;
  std::string config_name;
  std::string display_name;
  bool is_supported;

  NocCounterSettings(bool is_configurable,
                     bool is_noc_customizable,
                     const char* config_name,
                     const char* display_name,
                     std::vector<base::HailoSocType> supported_socs)
      : is_configurable(is_configurable),
        is_noc_customizable(is_noc_customizable),
        config_name(config_name),
        display_name(display_name) {
    is_supported = std::find(supported_socs.begin(), supported_socs.end(),
                             base::GetHailoSocType()) != supported_socs.end();
    ;
  }
};

// clang-format off
static const std::map<PerfEvents::HailoNocCounterType, const struct NocCounterSettings> kNocCounterSettings{
  {PerfEvents::HAILO_CSM_COUNTER,    {/*is_configurable=*/true,  /*is_noc_customizable=*/false, /*config_name=*/"csm",      /*display_name=*/"csm",           /*supported_socs=*/{base::HailoSocType::HAILO15, base::HailoSocType::HAILO15L}} },
  {PerfEvents::HAILO_DSM_RX_COUNTER, {/*is_configurable=*/true,  /*is_noc_customizable=*/false, /*config_name=*/"dsm_rx",   /*display_name=*/"dsm rx",        /*supported_socs=*/{base::HailoSocType::HAILO15, base::HailoSocType::HAILO15L}}},
  {PerfEvents::HAILO_DSM_TX_COUNTER, {/*is_configurable=*/true,  /*is_noc_customizable=*/false, /*config_name=*/"dsm_tx",   /*display_name=*/"dsm tx",        /*supported_socs=*/{base::HailoSocType::HAILO15, base::HailoSocType::HAILO15L}}},
  {PerfEvents::HAILO_NOC_COUNTER_0,  {/*is_configurable=*/true,  /*is_noc_customizable=*/true,  /*config_name=*/"counter0", /*display_name=*/"NoC counter 0", /*supported_socs=*/{base::HailoSocType::HAILO15, base::HailoSocType::HAILO15L}}},
  {PerfEvents::HAILO_NOC_COUNTER_1,  {/*is_configurable=*/true,  /*is_noc_customizable=*/true,  /*config_name=*/"counter1", /*display_name=*/"NoC counter 1", /*supported_socs=*/{base::HailoSocType::HAILO15, base::HailoSocType::HAILO15L}}},
  {PerfEvents::HAILO_NOC_COUNTER_2,  {/*is_configurable=*/true,  /*is_noc_customizable=*/true,  /*config_name=*/"counter2", /*display_name=*/"NoC counter 2", /*supported_socs=*/{base::HailoSocType::HAILO15, base::HailoSocType::HAILO15L}}},
  {PerfEvents::HAILO_NOC_COUNTER_3,  {/*is_configurable=*/true,  /*is_noc_customizable=*/true,  /*config_name=*/"counter3", /*display_name=*/"NoC counter 3", /*supported_socs=*/{base::HailoSocType::HAILO15, base::HailoSocType::HAILO15L}}},
  {PerfEvents::HAILO_NOC_COUNTER_4,  {/*is_configurable=*/false, /*is_noc_customizable=*/false, /*config_name=*/"",         /*display_name=*/"Total BW",      /*supported_socs=*/{base::HailoSocType::HAILO15L}}}
};
// clang-format on

std::optional<uint32_t> GetHailoNocPerfEventType() {
  return base::GetFileValue("/sys/devices/hailo_noc_pmu/type");
}

template <typename noc_sample_type>
static void EmitHailoNocCounterSample(
    perfetto::protos::pbzero::PerfSample* perf_sample,
    const noc_sample_type* sample,
    const struct PerfCounter& counter) {
  uint32_t value = 0;

  switch (counter.hailo_noc_counter_type) {
    case PerfEvents::HAILO_DSM_RX_COUNTER:
      value = sample->dsm_rx_counter;
      break;
    case PerfEvents::HAILO_DSM_TX_COUNTER:
      value = sample->dsm_tx_counter;
      break;
    case PerfEvents::HAILO_CSM_COUNTER:
      value = sample->csm_counter;
      break;
    case PerfEvents::HAILO_NOC_COUNTER_0:
      value = sample->noc_counters[0];
      break;
    case PerfEvents::HAILO_NOC_COUNTER_1:
      value = sample->noc_counters[1];
      break;
    case PerfEvents::HAILO_NOC_COUNTER_2:
      value = sample->noc_counters[2];
      break;
    case PerfEvents::HAILO_NOC_COUNTER_3:
      value = sample->noc_counters[3];
      break;
    case PerfEvents::HAILO_NOC_COUNTER_4:
      if constexpr (std::is_same_v<noc_sample_type, struct noc_sample_h15l>) {
        value = sample->noc_counters[4];
      } else {
        PERFETTO_FATAL("Counter type not supported for this chip");
      }
      break;
    case PerfEvents::UNKNOWN_HAILO_NOC_COUNTER:
      PERFETTO_FALLTHROUGH;
    default:
      PERFETTO_FATAL("Unknown Hailo NOC counter type");
      break;
  }
  perf_sample->add_follower_counts(value);
}

template <typename noc_sample_type>
static void EmitHailoNocAuxSampleForChip(TraceWriter* trace_writer,
                                         const EventConfig& event_config,
                                         const std::vector<char> aux) {
  int len = 0;
  const noc_sample_type* noc_sample =
      reinterpret_cast<const noc_sample_type*>(aux.data());

  PERFETTO_CHECK(aux.size() % sizeof(noc_sample_type) == 0);

  len = aux.size() / sizeof(noc_sample_type);

  for (int i = 0; i < len; i++, noc_sample++) {
    auto packet = trace_writer->NewTracePacket();
    packet->set_sequence_flags(
        protos::pbzero::TracePacket::SEQ_NEEDS_INCREMENTAL_STATE);
    packet->set_timestamp(noc_sample->timestamp);
    auto* perf_sample = packet->set_perf_sample();
    for (const auto& counter : event_config.follower_events()) {
      PERFETTO_CHECK(counter.event_type() ==
                     PerfCounter::Type::kHailoNocCounter);
      EmitHailoNocCounterSample(perf_sample, noc_sample, counter);
    }
  }
}

void EmitHailoNocAuxSample(TraceWriter* trace_writer,
                           const EventConfig& event_config,
                           const std::vector<char> aux) {
  switch (base::GetHailoSocType()) {
    case base::HailoSocType::HAILO15:
      return EmitHailoNocAuxSampleForChip<struct noc_sample_h15>(
          trace_writer, event_config, std::move(aux));
    case base::HailoSocType::HAILO15L:
      return EmitHailoNocAuxSampleForChip<struct noc_sample_h15l>(
          trace_writer, event_config, std::move(aux));
    default:
      PERFETTO_FATAL("Unknown Hailo SoC type");
  }
}

static uint32_t StringToRouteIdBase(const std::string& route_name) {
  auto it = nocSettings.filter_route_base_names.find(route_name);
  if (it != nocSettings.filter_route_base_names.end()) {
    return it->second;
  }

  PERFETTO_FATAL("Unknown route name: %s", route_name.c_str());
  return 0;  // This line will never be reached due to PERFETTO_FATAL.
}

// Extract parameters from HailoNocCounter message and create a PerfCounter
PerfCounter HailoNocCounterToPerfCounter(
    const std::string& name,
    const protos::gen::PerfEvents_HailoNocCounterFollowerEvent&
        hailo_noc_counter) {
  std::string counter_name = "";
  uint32_t route_id_base = kDefaultRouteIdBase;
  uint32_t route_id_mask = kDefaultRouteIdMask;
  uint32_t opcode = kDefaultOpcode;
  uint64_t address_base = kDefaultAddressBase;
  uint32_t length = kDefaultLength;
  uint32_t urgency = kDefaultUrgency;
  uint32_t window_size = kDefaultWindowSize;
  uint32_t status = kDefaultStatus;
  std::string mode = std::string(kDefaultMode);
  std::string access_type = "";
  PerfEvents::HailoNocCounterType type = hailo_noc_counter.type();
  bool is_default_counter = true;

  if (kNocCounterSettings.find(type) == kNocCounterSettings.end()) {
    PERFETTO_FATAL("Unknown or unsupported Hailo NOC counter type: %d", type);
  }

  const auto& counterSettings = kNocCounterSettings.at(type);
  if (!counterSettings.is_supported) {
    PERFETTO_FATAL("Counter type %s is not supported on this SoC",
                   counterSettings.display_name.c_str());
  }

  counter_name = counterSettings.display_name;
  std::string special_overrides = "";
  if (counterSettings.is_noc_customizable) {
    // Extract all parameter values directly from the protobuf
    if (hailo_noc_counter.has_route_id()) {
      if (hailo_noc_counter.route_id().has_route_id_base()) {
        is_default_counter = false;
        route_id_base = hailo_noc_counter.route_id().route_id_base();
        special_overrides +=
            "route_id_base=" + base::IntToHexString(route_id_base) + " ";
      }
      if (hailo_noc_counter.route_id().has_route_id_mask()) {
        is_default_counter = false;
        route_id_mask = hailo_noc_counter.route_id().route_id_mask();
        special_overrides +=
            "route_id_mask=" + base::IntToHexString(route_id_mask) + " ";
      }
    } else if (hailo_noc_counter.has_filter()) {
      is_default_counter = false;
      counter_name =
          hailo_noc_counter.filter();  // use the filter as the counter name
      route_id_base = StringToRouteIdBase(counter_name);
      route_id_mask = nocSettings.route_id_mask_filter;
    }

    if (hailo_noc_counter.has_opcode()) {
      is_default_counter = false;
      opcode = hailo_noc_counter.opcode();
      special_overrides += "opcode=" + base::IntToHexString(opcode) + " ";
    } else if (hailo_noc_counter.has_access_type()) {
      is_default_counter = false;
      access_type = hailo_noc_counter.access_type();
      if (access_type == "read") {
        opcode = kReadOpcode;
        special_overrides += "read ";
      } else if (access_type == "write") {
        opcode = kWriteOpcode;
        special_overrides += "write ";
      } else {
        PERFETTO_FATAL("Unknown access type: %s", access_type.c_str());
      }
    }

    if (hailo_noc_counter.has_address_base()) {
      is_default_counter = false;
      address_base = hailo_noc_counter.address_base();
      special_overrides +=
          "address_base=" + base::Uint64ToHexString(address_base) + " ";
    }

    if (hailo_noc_counter.has_length()) {
      is_default_counter = false;
      length = hailo_noc_counter.length();
      special_overrides += "length=" + base::IntToHexString(length) + " ";
    }

    if (hailo_noc_counter.has_urgency()) {
      is_default_counter = false;
      urgency = hailo_noc_counter.urgency();
      special_overrides += "urgency=" + base::IntToHexString(urgency) + " ";
    }

    if (hailo_noc_counter.has_window_size()) {
      is_default_counter = false;
      window_size = hailo_noc_counter.window_size();
      special_overrides +=
          "window_size=" + base::IntToHexString(window_size) + " ";
    }

    if (hailo_noc_counter.has_mode()) {
      is_default_counter = false;
      mode = hailo_noc_counter.mode();
      special_overrides += "mode=" + mode + " ";
    }

    if (hailo_noc_counter.has_status()) {
      is_default_counter = false;
      status = hailo_noc_counter.status();
      special_overrides += "status=" + base::IntToHexString(status) + " ";
    }

    if (!special_overrides.empty()) {
      counter_name += " [" + base::StripSuffix(special_overrides, " ") + "]";
    }

    if (is_default_counter) {
      counter_name = "Total BW";
    }
  }

  // If the name is not empty, use it as the counter name
  // This allows for overriding the default name generated from configuration
  if (!name.empty()) {
    counter_name = name;
  }

  // Create and return the PerfCounter object with all parameters
  return PerfCounter::HailoNocCounter(
      counter_name, type, route_id_base, route_id_mask, opcode, address_base,
      length, urgency, window_size, status, mode);
}

// Extract parameters from HailoNocEvent message and create a PerfCounter
PerfCounter HailoNocEventToPerfCounter(
    const PerfEvents::HailoNocEvent& noc_event) {
  // Default values
  uint32_t sample_time_us = kDefaultSampleTimeUs;
  PerfEvents::HailoNocRunningMode running_mode =
      PerfEvents::HAILO_NOC_RUNNING_MODE_GTIMER;
  uint32_t sample_count_limit = 0;

  // Extract values from the protobuf config
  if (noc_event.has_sample_time_us()) {
    sample_time_us = noc_event.sample_time_us();
  }

  if (noc_event.has_running_mode()) {
    running_mode = noc_event.running_mode();
  }

  if (noc_event.has_sample_count_limit()) {
    sample_count_limit = noc_event.sample_count_limit();
  }

  return PerfCounter::HailoNocEvent(sample_time_us, running_mode,
                                    sample_count_limit);
}

bool PreconfigureHailoNocEvent(const EventConfig& event_cfg) {
  static const std::string kBasePath{"/sys/devices/hailo_noc_pmu/"};
  bool success = true;
  for (const auto& [counter_type, settings] : kNocCounterSettings) {
    if (!settings.is_supported || !settings.is_configurable) {
      continue;
    }
    success &=
        base::SetFileValue(kBasePath + settings.config_name + "/enabled", 0);
  }

  for (auto follower_event : event_cfg.follower_events()) {
    PERFETTO_CHECK(follower_event.type == PerfCounter::Type::kHailoNocCounter);
    const auto& settings =
        kNocCounterSettings.at(follower_event.hailo_noc_counter_type);
    if (!settings.is_configurable) {
      continue;
    }

    std::string counter_path = kBasePath + settings.config_name;

    success &= base::SetFileValue(counter_path + "/enabled", 1);
    if (!settings.is_noc_customizable) {
      continue;
    }
    success &= base::SetFileValueHex(counter_path + "/address_base",
                                     follower_event.address_base);
    success &=
        base::SetFileValueHex(counter_path + "/length", follower_event.length);
    success &=
        base::SetFileContent(counter_path + "/mode", follower_event.noc_mode);
    success &=
        base::SetFileValueHex(counter_path + "/opcode", follower_event.opcode);
    success &= base::SetFileValueHex(counter_path + "/route_id_base",
                                     follower_event.route_id_base);
    success &= base::SetFileValueHex(counter_path + "/route_id_mask",
                                     follower_event.route_id_mask);
    success &=
        base::SetFileValueHex(counter_path + "/status", follower_event.status);
    success &= base::SetFileValueHex(counter_path + "/urgency",
                                     follower_event.urgency);
    success &= base::SetFileValueHex(counter_path + "/window_size",
                                     follower_event.window_size);
  }
  return success;
}

}  // namespace perfetto::profiling
