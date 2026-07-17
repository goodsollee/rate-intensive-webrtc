/*
 *  Copyright (c) 2024 The WebRTC project authors. All Rights Reserved.
 *
 *  RTP-SCTP Coordinator: V14 OWD-based BUR estimation.
 *  RTP BUR = (accumulated_delay - min_accumulated_delay) / L
 *  SCTP BUR = max(RTT - RTT_min) / L
 *  L = configurable normalization constant (BUR_L_MS env var, default 100ms).
 *
 *  Threading model:
 *    GCC thread: OnTwccUpdate, OnTwccFeedbackComplete → ComputeBurAndDecide
 *    Network thread: OnChunkSent, OnSackReceived, CheckTimeouts
 *    Shared: sctp_rtt_queue_ (mutex), rtt_min_us_ (atomic), pacing_rate_bps_ (atomic)
 */

#include "pc/rtp_sctp_coordinator.h"

#include "modules/pacing/pacing_controller.h"
#include "media/sctp/dcsctp_transport.h"
#include "rtc_base/logging.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <sys/stat.h>

namespace webrtc {

// Static singleton instance
RtpSctpCoordinator* RtpSctpCoordinator::active_instance_ = nullptr;
std::atomic<int64_t> RtpSctpCoordinator::rtp_target_floor_bps_{0};
std::atomic<int64_t> RtpSctpCoordinator::rtp_fse_override_bps_{0};
std::atomic<bool> RtpSctpCoordinator::fse_mode_active_{false};
std::atomic<bool> RtpSctpCoordinator::fse_v2_mode_active_{false};
std::atomic<double> RtpSctpCoordinator::pudica_rho_{2.0};
std::atomic<int64_t> RtpSctpCoordinator::pudica_rtp_target_bps_{0};
std::atomic<bool> RtpSctpCoordinator::pudica_mode_active_{false};

namespace {

double ReadEnvDouble(const char* name, double default_value) {
  const char* env = std::getenv(name);
  if (env) {
    char* end;
    double val = std::strtod(env, &end);
    if (end != env && *end == '\0') return val;
  }
  return default_value;
}

int64_t ReadEnvInt64(const char* name, int64_t default_value) {
  const char* env = std::getenv(name);
  if (env) {
    char* end;
    long long val = std::strtoll(env, &end, 10);
    if (end != env && *end == '\0') return static_cast<int64_t>(val);
  }
  return default_value;
}

bool ReadEnvBool(const char* name, bool default_value) {
  const char* env = std::getenv(name);
  if (env) {
    std::string s(env);
    return s == "1" || s == "true" || s == "yes";
  }
  return default_value;
}

SctpBurMode ParseSctpBurMode(const char* env_val) {
  if (!env_val) return SctpBurMode::kRttAdditive;
  std::string s(env_val);
  if (s == "rtt_min") return SctpBurMode::kRttMin;
  if (s == "rtt_additive" || s == "additive") return SctpBurMode::kRttAdditive;
  if (s == "rtt_min_hi" || s == "rtt_min_frame_hi") return SctpBurMode::kRttMinFrameHi;
  if (s == "rtt_min_all") return SctpBurMode::kRttMinAll;
  return SctpBurMode::kRttRef;
}

const char* SctpBurModeStr(SctpBurMode mode) {
  switch (mode) {
    case SctpBurMode::kRttRef: return "rtt_ref";
    case SctpBurMode::kRttMin: return "rtt_min";
    case SctpBurMode::kRttAdditive: return "rtt_additive";
    case SctpBurMode::kRttMinFrameHi: return "rtt_min_hi";
    case SctpBurMode::kRttMinAll: return "rtt_min_all";
  }
  return "rtt_ref";
}

BurCombineMode ParseCombineMode(const char* env_val) {
  if (!env_val) return BurCombineMode::kSctp;
  std::string s(env_val);
  if (s == "rtp") return BurCombineMode::kRtp;
  if (s == "max") return BurCombineMode::kMax;
  if (s == "avg") return BurCombineMode::kWeightedAvg;
  if (s == "additive" || s == "add") return BurCombineMode::kAdditive;
  if (s == "fallback" || s == "fb") return BurCombineMode::kFallback;
  if (s == "decay") return BurCombineMode::kDecay;
  return BurCombineMode::kSctp;
}

const char* CombineModeStr(BurCombineMode mode) {
  switch (mode) {
    case BurCombineMode::kSctp: return "sctp";
    case BurCombineMode::kRtp: return "rtp";
    case BurCombineMode::kMax: return "max";
    case BurCombineMode::kWeightedAvg: return "avg";
    case BurCombineMode::kAdditive: return "additive";
    case BurCombineMode::kFallback: return "fallback";
    case BurCombineMode::kDecay: return "decay";
  }
  return "sctp";
}

CoordinatorFeatures ReadFeatures() {
  CoordinatorFeatures f;
  f.twcc_bur = ReadEnvBool("BUR_TWCC", false);
  f.sctp_bur = ReadEnvBool("BUR_SCTP", true);
  f.sctp_bur_mode = ParseSctpBurMode(std::getenv("BUR_SCTP_MODE"));
  f.combine = ParseCombineMode(std::getenv("BUR_COMBINE"));
  f.rtp_recv_rate = ReadEnvBool("BUR_RTP_RECV", true);
  f.total_recv_rate = ReadEnvBool("BUR_TOTAL_RECV", true);
  f.combine_weight = ReadEnvDouble("BUR_COMBINE_WEIGHT", 0.5);
  f.L_ms = ReadEnvDouble("BUR_L_MS", 33.0);
  f.alloc_dynamic = ReadEnvBool("BUR_ALLOC_DYNAMIC", true);
  f.unified_rtp_rate_ctrl = ReadEnvBool("UNIFIED_RTP_RATE_CTRL", false);
  f.max_rtp_share = ReadEnvDouble("UNIFIED_MAX_RTP_SHARE", 0.5);
  // Pudica
  f.pudica_probing = ReadEnvBool("PUDICA_PROBING", false);
  f.sctp_pacing_bypass = ReadEnvBool("SCTP_PACING_BYPASS", false);
  f.measure_only = ReadEnvBool("BUR_MEASURE_ONLY", false);
  f.pudica_num_probes = static_cast<int>(ReadEnvInt64("PUDICA_NUM_PROBES", 4));
  f.pudica_gamma_rho = ReadEnvDouble("PUDICA_GAMMA_RHO", 1.25);
  // SCTP frame-burst probing
  f.sctp_burst_probe = ReadEnvBool("SCTP_BURST_PROBE", false);
  f.sctp_burst_mult = ReadEnvDouble("SCTP_BURST_MULT", 2.0);
  f.sctp_burst_us = ReadEnvInt64("SCTP_BURST_MS", 5) * 1000;  // ms → μs
  // SCTP jitter probing
  f.sctp_jitter = ReadEnvDouble("SCTP_JITTER", 0.0);
  return f;
}

}  // namespace

// ============================================================
// CoordinatorConfig
// ============================================================

CoordinatorConfig CoordinatorConfig::FromEnvironment() {
  CoordinatorConfig cfg;

  const char* mode_env = std::getenv("COORDINATOR_MODE");
  if (mode_env) {
    std::string mode_str(mode_env);
    if (mode_str == "agentrtc") {
      cfg.mode = CoordinatorMode::kAgentRtc;
    } else if (mode_str == "fse") {
      cfg.mode = CoordinatorMode::kFse;
    } else if (mode_str == "pudica") {
      cfg.mode = CoordinatorMode::kPudica;
    } else if (mode_str == "fse_v2") {
      cfg.mode = CoordinatorMode::kFseV2;
    }
  }

  const char* rate_env = std::getenv("PACING_RATE_MBPS");
  if (rate_env) {
    int64_t rate_mbps = std::atoll(rate_env);
    if (rate_mbps > 0) cfg.static_rate_bps = rate_mbps * 1'000'000;
  }

  cfg.alpha            = ReadEnvDouble("BUR_ALPHA", 0.85);
  cfg.bur_threshold    = ReadEnvDouble("BUR_THRESHOLD", 0.5);
  cfg.spike_threshold  = ReadEnvDouble("BUR_SPIKE_THRESHOLD", 2.0);
  cfg.max_mi_multiplier = ReadEnvDouble("BUR_MAX_MI_MULTIPLIER", 1.2);
  cfg.spike_reduction  = ReadEnvDouble("BUR_SPIKE_REDUCTION", 0.85);
  cfg.interval_ms      = ReadEnvInt64("BUR_INTERVAL_MS", 33);
  // Auto-derive feedback timeout from spike_threshold × L_ms.
  // Rationale: BUR = excess_delay / L_ms. If a single interval has
  // BUR > spike_threshold, excess_delay > spike_threshold × L_ms.
  // So missing feedback for that duration = same severity as one spike tick.
  // Explicit BUR_FEEDBACK_TIMEOUT_MS / BUR_TIMEOUT_MS env overrides this.
  double l_ms = ReadEnvDouble("BUR_L_MS", 33.0);
  int64_t auto_timeout_ms = static_cast<int64_t>(cfg.spike_threshold * l_ms);
  cfg.timeout_ms       = ReadEnvInt64("BUR_FEEDBACK_TIMEOUT_MS",
                           ReadEnvInt64("BUR_TIMEOUT_MS", auto_timeout_ms));
  cfg.rate_decision_interval_ms = ReadEnvInt64("BUR_RATE_DECISION_INTERVAL_MS", 33);
  cfg.draining_target  = ReadEnvDouble("BUR_DRAINING_TARGET", 0.85);

  cfg.gamma_md                  = ReadEnvDouble("BUR_GAMMA_MD", 0.20);
  cfg.gamma_mi                  = ReadEnvDouble("BUR_GAMMA_MI", 0.12);
  cfg.fairness_upper_threshold  = ReadEnvDouble("BUR_FAIRNESS_UPPER", 1.0);
  cfg.draining_bur_threshold    = ReadEnvDouble("BUR_DRAINING_THRESHOLD", 2.0);
  cfg.recv_cap_margin           = ReadEnvDouble("BUR_RECV_CAP_MARGIN", 1.20);
  cfg.gamma_spike               = ReadEnvDouble("BUR_GAMMA_SPIKE", 0.30);
  cfg.raw_corr_max_step         = ReadEnvDouble("BUR_RAW_CORR_MAX_STEP", 0.03);
  cfg.alpha_target              = ReadEnvDouble("BUR_ALPHA_TARGET", 0.9);
  cfg.max_step_fraction         = ReadEnvDouble("BUR_MAX_STEP_FRACTION", 0.08);
  cfg.ewma_alpha                = ReadEnvDouble("BUR_EWMA_ALPHA", 0.30);
  cfg.ai_recv_alpha             = ReadEnvDouble("BUR_AI_RECV_ALPHA", 0.0);
  cfg.raw_corr_factor           = ReadEnvDouble("BUR_RAW_CORR_FACTOR", 0.85);
  cfg.skip_draining             = ReadEnvInt64("BUR_SKIP_DRAINING", 0) != 0;

  cfg.initial_rate_bps = ReadEnvInt64("BUR_R_INIT_KBPS", 10000) * 1000;
  cfg.max_rate_bps     = ReadEnvInt64("BUR_MAX_RATE_MBPS", 1000) * 1'000'000;
  cfg.min_rate_bps     = ReadEnvInt64("BUR_MIN_RATE_KBPS", 1000) * 1000;
  cfg.rtp_max_rate_bps = ReadEnvInt64("RTP_MAX_RATE_KBPS", 0) * 1000;

  // FSEv2 params
  cfg.fse_p_rtp = ReadEnvDouble("FSE_P_RTP", 2.0);
  cfg.fse_p_sctp = ReadEnvDouble("FSE_P_SCTP", 1.0);
  cfg.fse_dr_rtp_kbps = ReadEnvInt64("FSE_DR_RTP_KBPS", 10000);

  return cfg;
}

// ============================================================
// Constructor / Destructor
// ============================================================

RtpSctpCoordinator::RtpSctpCoordinator(rtc::Thread* network_thread,
                                        const CoordinatorConfig& config)
    : config_(config), features_(ReadFeatures()) {
  if (config_.static_rate_bps > 0) {
    pacing_rate_bps_.store(config_.static_rate_bps, std::memory_order_relaxed);
    sctp_allocated_bps_.store(config_.static_rate_bps, std::memory_order_relaxed);
  } else if (config_.initial_rate_bps > 0) {
    pacing_rate_bps_.store(config_.initial_rate_bps, std::memory_order_relaxed);
    sctp_allocated_bps_.store(config_.initial_rate_bps, std::memory_order_relaxed);
  }

  active_instance_ = this;

  const char* mode_str =
      (config_.mode == CoordinatorMode::kAgentRtc) ? "agentrtc" :
      (config_.mode == CoordinatorMode::kFse) ? "fse" :
      (config_.mode == CoordinatorMode::kPudica) ? "pudica" : "disabled";
  RTC_LOG(LS_INFO) << "[BUR-COORD] V14 Created: mode=" << mode_str
                   << " twcc_bur=" << features_.twcc_bur
                   << " sctp_bur=" << features_.sctp_bur
                   << " combine=" << CombineModeStr(features_.combine)
                   << " L_ms=" << features_.L_ms
                   << " rtp_recv=" << features_.rtp_recv_rate
                   << " total_recv=" << features_.total_recv_rate
                   << " alpha=" << config_.alpha
                   << " threshold=" << config_.bur_threshold
                   << " spike=" << config_.spike_threshold
                   << " timeout=" << config_.timeout_ms << "ms"
                   << " rate_decision=" << config_.rate_decision_interval_ms << "ms"
                   << " draining_target=" << config_.draining_target
                   << " gamma_md=" << config_.gamma_md
                   << " gamma_mi=" << config_.gamma_mi
                   << " alpha_target=" << config_.alpha_target
                   << " max_step=" << config_.max_step_fraction
                   << " drain_bur_th=" << config_.draining_bur_threshold
                   << " ewma_alpha=" << config_.ewma_alpha
                   << " recv_cap_margin=" << config_.recv_cap_margin
                   << " gamma_spike=" << config_.gamma_spike
                   << " raw_corr_max=" << config_.raw_corr_max_step
                   << " init_rate=" << (pacing_rate_bps_.load() / 1e6) << "Mbps"
                   << " rtp_max=" << (config_.rtp_max_rate_bps / 1e6) << "Mbps"
                   << " alloc_dynamic=" << features_.alloc_dynamic
                   << " unified_rtp_rate_ctrl=" << features_.unified_rtp_rate_ctrl;

  // FSEv2 initialization — GCC runs freely until SCTP starts sending
  if (config_.mode == CoordinatorMode::kFse) {
    fse_mode_active_.store(true, std::memory_order_relaxed);
    fse_dr_rtp_bps_ = config_.fse_dr_rtp_kbps * 1000.0;
    // No override at startup: GCC discovers capacity independently.
    // FSE coordination activates only when SCTP begins sending.
    fse_s_cr_ = 0.0;
    fse_r_rtp_ = 0.0;
    fse_r_sctp_ = 0.0;
    rtp_fse_override_bps_.store(0, std::memory_order_relaxed);
    RTC_LOG(LS_INFO) << "[FSEv2] Initialized: P_rtp=" << config_.fse_p_rtp
                     << " P_sctp=" << config_.fse_p_sctp
                     << " DR_rtp=" << (fse_dr_rtp_bps_ / 1e6) << "Mbps"
                     << " GCC runs freely until SCTP starts sending";
  }

  // FSEv2 lightweight: NC-mode SCTP CC + 1Hz fixed RTP override (test isolation).
  // No SCTP cwnd tracking, no rate redistribution — just hold RTP at FSE_DR_RTP_KBPS.
  if (config_.mode == CoordinatorMode::kFseV2) {
    fse_v2_mode_active_.store(true, std::memory_order_relaxed);
    fse_dr_rtp_bps_ = config_.fse_dr_rtp_kbps * 1000.0;
    rtp_fse_override_bps_.store(static_cast<int64_t>(fse_dr_rtp_bps_),
                                std::memory_order_relaxed);
    RTC_LOG(LS_INFO) << "[FSEv2-LITE] Initialized: DR_rtp="
                     << (fse_dr_rtp_bps_ / 1e6) << "Mbps"
                     << " (NC SCTP CC + 1Hz override store)";
  }

  // Pudica: force RTP-only BUR measurement
  if (config_.mode == CoordinatorMode::kPudica) {
    features_.twcc_bur = true;
    features_.sctp_bur = false;
    features_.combine = BurCombineMode::kRtp;
    pudica_rho_.store(2.0, std::memory_order_relaxed);  // initial ρ = 2.0
    // Apollo: Pudica controls the RTP-video target directly (BUR-driven).
    // Start at 0 so GCC warm-starts until the first frame BUR arrives.
    pudica_mode_active_.store(true, std::memory_order_relaxed);
    pudica_rtp_target_bps_.store(0, std::memory_order_relaxed);
    // Activate probe injection in PacingController
    if (features_.pudica_probing) {
      PacingController::SetPudicaProbing(true, features_.pudica_num_probes);
    }
    RTC_LOG(LS_INFO) << "[PUDICA] Initialized: L_ms=" << features_.L_ms
                     << " probing=" << features_.pudica_probing
                     << " num_probes=" << features_.pudica_num_probes
                     << " gamma_rho=" << features_.pudica_gamma_rho;
  }

  // Initialize MAFS flow coordinator if enabled
  mafs_config_ = MafsConfig::FromEnvironment();
  if (mafs_config_.enabled) {
    coordinator::MultiAgentFlowCoordinator::Config mafs_coord_config;
    mafs_coord_config.update_interval_ms = mafs_config_.update_interval_ms;
    mafs_coord_config.enable_logging = true;
    mafs_coord_config.telemetry_config.sample_interval_ms = mafs_config_.telemetry_sample_ms;
    mafs_coord_config.telemetry_config.ewma_window_ms = mafs_config_.telemetry_window_ms;
    mafs_coord_config.telemetry_config.ewma_alpha = mafs_config_.telemetry_alpha;
    mafs_coord_config.calculator_config.beta = mafs_config_.beta;
    mafs_coord_config.calculator_config.min_rate_bps = mafs_config_.min_rate_bps;
    mafs_coord_config.calculator_config.default_P_hat_ms = mafs_config_.default_P_hat_ms;
    mafs_coord_config.calculator_config.default_sigma_P_ms = mafs_config_.default_sigma_P_ms;
    mafs_coord_config.calculator_config.force_zero_P_hat = mafs_config_.force_zero_P_hat;
    mafs_coord_config.calculator_config.force_equal_priority = mafs_config_.force_equal_priority;
    mafs_coord_config.mapper_config.priority_max = mafs_config_.priority_max;
    mafs_coord_config.mapper_config.priority_min = mafs_config_.priority_min;
    mafs_coord_config.mapper_config.priority_spacing =
        mafs_config_.priority_spacing;
    mafs_coord_config.enable_logging = mafs_config_.verbose_logging;
    mafs_coord_config.auto_register_flows = true;

    flow_coordinator_ = std::make_unique<coordinator::MultiAgentFlowCoordinator>(mafs_coord_config);
    RTC_LOG(LS_INFO) << "[BUR-COORD] MAFS enabled: " << mafs_config_.ToString();
  }
}

RtpSctpCoordinator::~RtpSctpCoordinator() {
  if (active_instance_ == this) {
    active_instance_ = nullptr;
    rtp_fse_override_bps_.store(0, std::memory_order_relaxed);
    fse_mode_active_.store(false, std::memory_order_relaxed);
  }
  if (csv_file_.is_open()) csv_file_.close();
  RTC_LOG(LS_INFO) << "[BUR-COORD] Destroyed.";
}

void RtpSctpCoordinator::SetDcSctpTransport(DcSctpTransport* transport) {
  transport_ = transport;
  RTC_LOG(LS_INFO) << "[BUR-COORD] DcSctpTransport connected";
}

// ============================================================
// CSV Logging (V14: excess_rtp_ms, excess_sctp_ms columns)
// ============================================================

void RtpSctpCoordinator::InitCsvLogging() {
  const char* result_dir = std::getenv("UNIFIED_CSV_DIR");
  fprintf(stderr, "[BUR-COORD] InitCsvLogging: UNIFIED_CSV_DIR=%s mode=%d\n",
          result_dir ? result_dir : "(null)",
          static_cast<int>(config_.mode));
  if (result_dir && std::strlen(result_dir) > 0) {
    std::string csv_path = std::string(result_dir) + "/unified_metrics.csv";
    csv_file_.open(csv_path, std::ios::out | std::ios::trunc);
    if (csv_file_.is_open()) {
      chmod(csv_path.c_str(), 0666);
      csv_file_ << "time_ms,sctp_kbps,receiving_rate_kbps,pacing_rate_mbps,"
                   "bur,smoothed_bur,rtt_min_ms,rtt_max_ms,L_ms,"
                   "available_bw_kbps,cwnd_bytes,srtt_ms,peer_rwnd_bytes,unacked_bytes,"
                   "mode,rate_delta_mbps,recv_rate_mbps,consecutive_high,"
                   "bur_rtp,bur_sctp,bur_combined,"
                   "excess_rtp_ms,excess_sctp_ms,"
                   "rtp_recv_kbps,total_recv_kbps,combine_mode,"
                   "unified_rate_mbps,rtp_allocated_mbps,"
                   "sctp_allocated_mbps,alloc_phase,rtp_floor_mbps,"
                   "link_utilization,rtt_ref_ms\n";
      csv_initialized_ = true;
      RTC_LOG(LS_INFO) << "[BUR-COORD] CSV: " << csv_path;
    }
  }
}

void RtpSctpCoordinator::LogToCsv(int64_t now_us) {
  int64_t now_ms = now_us / 1000;
  if (!csv_initialized_) {
    InitCsvLogging();
    csv_start_time_ms_ = now_ms;
  }
  if (!csv_file_.is_open()) return;

  int64_t logged_pacing_rate_mbps = 0;
  if (config_.mode == CoordinatorMode::kAgentRtc ||
      config_.mode == CoordinatorMode::kPudica) {
    logged_pacing_rate_mbps = pacing_rate_bps_.load() / 1'000'000;
  }
  if (unified_metrics_.receiving_rate_kbps <= 0) {
    unified_metrics_.receiving_rate_kbps = GetDirectSctpRecvRateBps() / 1000;
  }
  if (unified_metrics_.total_recv_kbps <= 0) {
    unified_metrics_.rtp_recv_kbps = GetRtpRecvRateBps() / 1000;
    unified_metrics_.total_recv_kbps = GetTotalRecvRateBps() / 1000;
  }

  int64_t rel = now_ms - csv_start_time_ms_;
  csv_file_ << rel << ","
            << unified_metrics_.sctp_kbps << ","
            << unified_metrics_.receiving_rate_kbps << ","
            << logged_pacing_rate_mbps << ","
            << unified_metrics_.current_bur << ","
            << unified_metrics_.smoothed_bur << ","
            << unified_metrics_.rtt_min_ms << ","
            << unified_metrics_.rtt_max_ms << ","
            << unified_metrics_.L_ms << ","
            << unified_metrics_.available_bw_kbps << ","
            << unified_metrics_.cwnd_bytes << ","
            << unified_metrics_.srtt_ms << ","
            << unified_metrics_.peer_rwnd_bytes << ","
            << unified_metrics_.unacked_bytes << ","
            << unified_metrics_.mode << ","
            << unified_metrics_.rate_delta_mbps << ","
            << unified_metrics_.recv_rate_mbps << ","
            << unified_metrics_.consecutive_high << ","
            << unified_metrics_.bur_rtp << ","
            << unified_metrics_.bur_sctp << ","
            << unified_metrics_.bur_combined << ","
            << unified_metrics_.excess_rtp_ms << ","
            << unified_metrics_.excess_sctp_ms << ","
            << unified_metrics_.rtp_recv_kbps << ","
            << unified_metrics_.total_recv_kbps << ","
            << unified_metrics_.combine_mode << ","
            << unified_metrics_.unified_rate_mbps << ","
            << unified_metrics_.rtp_allocated_mbps << ","
            << unified_metrics_.sctp_allocated_mbps << ","
            << unified_metrics_.alloc_phase << ","
            << (rtp_target_floor_bps_.load() / 1'000'000.0) << ","
            << (unified_metrics_.available_bw_kbps > 0
                ? static_cast<double>(unified_metrics_.total_recv_kbps) /
                  unified_metrics_.available_bw_kbps
                : 0.0) << ","
            << (rtt_ref_us_.load(std::memory_order_relaxed) / 1000.0) << "\n";
  csv_file_.flush();
}

// ============================================================
// Simple setters
// ============================================================

void RtpSctpCoordinator::SetCwnd(int64_t cwnd_bytes) {
  unified_metrics_.cwnd_bytes = cwnd_bytes;
}
void RtpSctpCoordinator::SetSrtt(int64_t srtt_ms) {
  unified_metrics_.srtt_ms = srtt_ms;
}
void RtpSctpCoordinator::SetPeerRwnd(int64_t peer_rwnd_bytes) {
  unified_metrics_.peer_rwnd_bytes = peer_rwnd_bytes;
}
void RtpSctpCoordinator::SetUnackedBytes(int64_t unacked_bytes) {
  unified_metrics_.unacked_bytes = unacked_bytes;
}
void RtpSctpCoordinator::SetAvailableBandwidth(int64_t bw_kbps) {
  unified_metrics_.available_bw_kbps = bw_kbps;
}

int64_t RtpSctpCoordinator::GetRtpTargetFloor() {
  return rtp_target_floor_bps_.load(std::memory_order_relaxed);
}

int64_t RtpSctpCoordinator::GetRtpFseOverride() {
  return rtp_fse_override_bps_.load(std::memory_order_relaxed);
}

bool RtpSctpCoordinator::IsFseMode() {
  return fse_mode_active_.load(std::memory_order_relaxed);
}

bool RtpSctpCoordinator::IsFseV2Mode() {
  return fse_v2_mode_active_.load(std::memory_order_relaxed);
}

void RtpSctpCoordinator::OnGccRateUpdated(int64_t cc_r_bps) {
  if (!active_instance_) return;
  auto* self = active_instance_;
  if (self->config_.mode != CoordinatorMode::kFse) return;

  auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  self->FseOnRtpUpdate(static_cast<double>(cc_r_bps), now_us);
}

int64_t RtpSctpCoordinator::GetPacingRate() const {
  int64_t alloc = sctp_allocated_bps_.load(std::memory_order_relaxed);
  int64_t rate = alloc > 0 ? alloc : pacing_rate_bps_.load(std::memory_order_relaxed);

  // SCTP frame-burst probing: burst at frame boundary, drain otherwise
  // Keeps average rate constant: burst_frac × mult + (1-burst_frac) × drain = 1.0
  if (features_.sctp_burst_probe && rate > 0) {
    int64_t burst_start = frame_burst_start_us_.load(std::memory_order_relaxed);
    if (burst_start > 0) {
      auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count();
      int64_t elapsed = now_us - burst_start;
      int64_t frame_us = static_cast<int64_t>(features_.L_ms * 1000.0);

      if (elapsed < features_.sctp_burst_us) {
        // Burst phase: multiply rate
        rate = static_cast<int64_t>(rate * features_.sctp_burst_mult);
      } else if (elapsed < frame_us) {
        // Drain phase: reduce to keep average constant
        double burst_frac = static_cast<double>(features_.sctp_burst_us) / frame_us;
        double drain_factor = (1.0 - burst_frac * features_.sctp_burst_mult)
                            / (1.0 - burst_frac);
        if (drain_factor < 0.1) drain_factor = 0.1;
        rate = static_cast<int64_t>(rate * drain_factor);
      }
    }
  }

  // SCTP jitter probing: apply per-frame random multiplier
  if (features_.sctp_jitter > 0.0) {
    double mult = frame_jitter_mult_.load(std::memory_order_relaxed);
    rate = static_cast<int64_t>(rate * mult);
  }

  return rate;
}

// ============================================================
// Rate Allocation: split unified rate → RTP floor + SCTP pacing
//
// UNIFIED_RTP_RATE_CTRL=1 (adaptive share model):
//   rtp_floor = min(adaptive_share × unified, rtp_max)
//   sctp      = unified - rtp_floor
//
//   adaptive_share scales with bandwidth headroom:
//     High BW (unified ≥ 2×rtp_max) → max_share (default 0.5)
//     Low BW  (unified → 0)         → min_share (0.2)
//     Between: linear interpolation
//
//   Result: RTP gets rtp_max when BW is ample,
//           shrinks proportionally when BW is tight.
//
// UNIFIED_RTP_RATE_CTRL=0 (legacy):
//   No GCC floor. SCTP pacing = unified - rtp_recv×1.2
//
// WAIT: no SCTP traffic → no allocation needed
// ============================================================

void RtpSctpCoordinator::AllocateUnifiedRate() {
  int64_t unified = pacing_rate_bps_.load(std::memory_order_relaxed);
  int64_t rtp_recv = GetRtpRecvRateBps();
  int64_t total_recv = GetTotalRecvRateBps();
  int64_t rtp_max = config_.rtp_max_rate_bps;

  int64_t sctp_rate;
  int64_t rtp_alloc;
  int64_t floor = 0;
  const char* phase;

  bool active = (rtp_recv > 0 &&
                 total_recv > static_cast<int64_t>(1.2 * rtp_recv));

  if (!active) {
    // No SCTP traffic — GCC handles RTP, SCTP gets full pacing
    sctp_rate = unified;
    rtp_alloc = rtp_recv;
    phase = "WAIT";

  } else if (features_.unified_rtp_rate_ctrl && rtp_max > 0) {
    // Adaptive share: RTP share scales with bandwidth headroom
    //   bw_ratio ≥ 2  → share = max_share (0.5)  → floor ≤ rtp_max
    //   bw_ratio = 0  → share = 0.2               → floor = 20% of unified
    constexpr double kMinShare = 0.2;
    double bw_ratio = static_cast<double>(unified) / rtp_max;
    double t = std::min(bw_ratio / 2.0, 1.0);
    double share = kMinShare + (features_.max_rtp_share - kMinShare) * t;

    floor = std::min(static_cast<int64_t>(unified * share), rtp_max);
    sctp_rate = unified - floor;
    rtp_alloc = floor;
    phase = (floor >= rtp_max) ? "SPLIT" : "SPLIT-S";

  } else {
    // Legacy: no GCC floor, SCTP gets unified minus RTP headroom
    int64_t rtp_reserve = 0;
    if (features_.alloc_dynamic && rtp_recv > 0) {
      rtp_reserve = static_cast<int64_t>(rtp_recv * 1.2);
      if (rtp_max > 0) rtp_reserve = std::min(rtp_reserve, rtp_max);
    } else {
      rtp_reserve = rtp_max;
    }

    sctp_rate = unified - rtp_reserve;
    rtp_alloc = rtp_reserve;
    phase = "SPLIT";
  }

  sctp_rate = std::max(config_.min_rate_bps,
                       std::min(config_.max_rate_bps, sctp_rate));
  // SCTP_PACING_BYPASS: bursty SCTP pacing for BUR measurement experiments.
  // Pattern controlled by SCTP_BURST_PATTERN env var:
  //   "random"  — uniform [0, 4x] per call (default)
  //   "micro"   — 5ms ON at 4x, 5ms OFF (10ms cycle, micro-burst)
  //   "macro"   — 50ms ON at 2x, 50ms OFF (100ms cycle)
  //   "sawtooth"— linear ramp 0→4x over 33ms then reset
  if (features_.sctp_pacing_bypass) {
    static thread_local uint32_t rng_state = 12345;
    static thread_local int64_t burst_start_us = 0;
    int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (burst_start_us == 0) burst_start_us = now_us;
    int64_t elapsed_us = now_us - burst_start_us;

    const char* pattern = std::getenv("SCTP_BURST_PATTERN");
    std::string pat = pattern ? pattern : "random";

    double mult = 1.0;
    if (pat == "micro") {
      // 10ms cycle: 5ms ON at 4x, 5ms OFF
      int64_t phase_us = elapsed_us % 10000;
      mult = (phase_us < 5000) ? 4.0 : 0.0;
    } else if (pat == "macro") {
      // 100ms cycle: 50ms ON at 2x, 50ms OFF
      int64_t phase_us = elapsed_us % 100000;
      mult = (phase_us < 50000) ? 2.0 : 0.0;
    } else if (pat == "sawtooth") {
      // 33ms cycle: linear ramp 0→4x
      int64_t phase_us = elapsed_us % 33000;
      mult = 4.0 * static_cast<double>(phase_us) / 33000.0;
    } else {
      // "random": uniform [0, 4x]
      rng_state = rng_state * 1103515245 + 12345;
      mult = static_cast<double>((rng_state >> 8) % 4001) / 1000.0;
    }
    sctp_rate = static_cast<int64_t>(sctp_rate * mult);
  }
  sctp_allocated_bps_.store(sctp_rate, std::memory_order_relaxed);

  unified_metrics_.unified_rate_mbps = unified / 1'000'000.0;
  unified_metrics_.rtp_allocated_mbps = rtp_alloc / 1'000'000.0;
  unified_metrics_.sctp_allocated_mbps = sctp_rate / 1'000'000.0;
  unified_metrics_.alloc_phase = phase;

  rtp_target_floor_bps_.store(floor, std::memory_order_relaxed);

  if (active) {
    RTC_LOG(LS_INFO) << "[BUR-COORD] ALLOC " << phase
                     << " unified=" << (unified / 1e6)
                     << " rtp=" << (rtp_alloc / 1e6)
                     << " sctp=" << (sctp_rate / 1e6)
                     << " rtp_recv=" << (rtp_recv / 1e6)
                     << " rtp_max=" << (rtp_max / 1e6)
                     << " floor=" << (floor / 1e6)
                     << "Mbps";
  }
}

// ============================================================
// Receiving rate functions
// ============================================================

int64_t RtpSctpCoordinator::GetAckRateBps() const {
  if (ack_samples_.empty()) return 0;
  int64_t ack_window = GetAckWindowMs();
  auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  if ((now_ms - ack_samples_.back().timestamp_ms) > ack_window * 2) {
    return 0;
  }
  int64_t total = ack_samples_total_bytes_;
  int64_t window = ack_window;
  if (ack_samples_.size() >= 3) {
    int64_t span_ms = ack_samples_.back().timestamp_ms -
                      ack_samples_.front().timestamp_ms;
    if (span_ms >= 10) {
      window = std::min(span_ms, ack_window);
    }
  }
  return (total * 8 * 1000) / window;
}

int64_t RtpSctpCoordinator::GetRtpRecvRateBps() const {
  if (rtp_ack_samples_.empty()) return 0;
  int64_t window_ms = rtp_ack_samples_.back().timestamp_ms -
                      rtp_ack_samples_.front().timestamp_ms;
  if (window_ms <= 0) return 0;
  return rtp_samples_total_bytes_ * 8 * 1000 / window_ms;
}

int64_t RtpSctpCoordinator::GetDirectSctpRecvRateBps() const {
  if (sctp_recv_samples_.empty()) return 0;
  int64_t window_ms = sctp_recv_samples_.back().timestamp_ms -
                      sctp_recv_samples_.front().timestamp_ms;
  if (window_ms <= 0) return 0;
  int64_t total_bytes = 0;
  for (const auto& s : sctp_recv_samples_) total_bytes += s.bytes;
  return total_bytes * 8 * 1000 / window_ms;
}

int64_t RtpSctpCoordinator::GetTotalRecvRateBps() const {
  int64_t sctp = GetAckRateBps();
  if (sctp <= 0) {
    sctp = GetDirectSctpRecvRateBps();
  }
  int64_t rtp = features_.total_recv_rate ? GetRtpRecvRateBps() : 0;
  return sctp + rtp;
}

// ============================================================
// BUR Combine [F4]
// ============================================================

double RtpSctpCoordinator::CombineBur(double bur_rtp, double bur_sctp) {
  switch (features_.combine) {
    case BurCombineMode::kSctp:
      return bur_sctp;
    case BurCombineMode::kRtp:
      return bur_rtp;
    case BurCombineMode::kMax:
      return std::max(bur_rtp, bur_sctp);
    case BurCombineMode::kWeightedAvg:
      return features_.combine_weight * bur_rtp +
             (1.0 - features_.combine_weight) * bur_sctp;
    case BurCombineMode::kAdditive:
      return bur_rtp + bur_sctp;
    case BurCombineMode::kFallback:
      return bur_sctp;  // SACK present when called; fallback to bur_rtp handled in skip logic
    case BurCombineMode::kDecay:
      return bur_sctp;  // SACK present when called; decay handled in skip logic
  }
  return bur_sctp;
}

// ============================================================
// OnTwccUpdate — static callback from TrendlineEstimator
// V14: receives accumulated_delay (absolute OWD queue depth proxy)
// Called per TWCC group from GCC thread (~5 times per 100ms)
// ============================================================

void RtpSctpCoordinator::OnTwccUpdate(double accumulated_delay_ms,
                                       int64_t arrival_time_ms) {
  if (!active_instance_) return;
  auto* self = active_instance_;

  // Frame OWD BUR replaces accumulated_delay for all agentrtc/pudica modes.
  if (self->config_.mode == CoordinatorMode::kPudica ||
      self->config_.mode == CoordinatorMode::kAgentRtc) {
    if (!self->acc_delay_initialized_) {
      self->acc_delay_initialized_ = true;
    }
    return;
  }

  // Update minimum (baseline for RTP BUR)
  if (!self->acc_delay_initialized_) {
    self->min_acc_delay_ms_ = accumulated_delay_ms;
    self->acc_delay_initialized_ = true;
  } else if (accumulated_delay_ms < self->min_acc_delay_ms_) {
    self->min_acc_delay_ms_ = accumulated_delay_ms;
  }

  // Push excess to queue (drained at ComputeBurAndDecide)
  double excess = accumulated_delay_ms - self->min_acc_delay_ms_;
  if (excess < 0.0) excess = 0.0;
  {
    std::lock_guard<std::mutex> lock(self->rtp_queue_mutex_);
    self->rtp_excess_queue_.push_back(excess);
  }
}

// ============================================================
// OnTwccFeedbackComplete — static callback from DelayBasedBwe
// Called per TWCC feedback message (~50-100ms interval)
// THIS IS THE INTERVAL COMPLETION TRIGGER
// ============================================================

void RtpSctpCoordinator::OnTwccFeedbackComplete(int64_t rtp_bytes_acked,
                                                  int64_t feedback_time_ms,
                                                  int64_t max_data_rate_bps) {
  if (!active_instance_) return;
  auto* self = active_instance_;

  // Passive RTP receive-rate tracking for disabled/conservative/raw modes.
  if (self->config_.mode != CoordinatorMode::kAgentRtc &&
      self->config_.mode != CoordinatorMode::kFse &&
      self->config_.mode != CoordinatorMode::kPudica) {
    if (self->features_.rtp_recv_rate && rtp_bytes_acked > 0) {
      self->rtp_samples_total_bytes_ += rtp_bytes_acked;
      self->rtp_ack_samples_.push_back({feedback_time_ms, rtp_bytes_acked});
      while (!self->rtp_ack_samples_.empty() &&
             (feedback_time_ms - self->rtp_ack_samples_.front().timestamp_ms) > kAckWindowMs) {
        self->rtp_samples_total_bytes_ -= self->rtp_ack_samples_.front().bytes;
        self->rtp_ack_samples_.pop_front();
      }
    }
    return;
  }

  // FSE mode: only track RTP recv rate, no BUR computation
  if (self->config_.mode == CoordinatorMode::kFse) {
    if (self->features_.rtp_recv_rate && rtp_bytes_acked > 0) {
      self->rtp_samples_total_bytes_ += rtp_bytes_acked;
      self->rtp_ack_samples_.push_back({feedback_time_ms, rtp_bytes_acked});
      while (!self->rtp_ack_samples_.empty() &&
             (feedback_time_ms - self->rtp_ack_samples_.front().timestamp_ms) > kAckWindowMs) {
        self->rtp_samples_total_bytes_ -= self->rtp_ack_samples_.front().bytes;
        self->rtp_ack_samples_.pop_front();
      }
    }
    return;
  }

  // Dynamic rtp_max_rate_bps from GCC max_data_rate (only when alloc_dynamic)
  if (self->features_.alloc_dynamic && max_data_rate_bps > 0 &&
      max_data_rate_bps != self->config_.rtp_max_rate_bps) {
    RTC_LOG(LS_INFO) << "[BUR-COORD] rtp_max updated: "
                     << (self->config_.rtp_max_rate_bps / 1e6) << " -> "
                     << (max_data_rate_bps / 1e6) << " Mbps (from GCC)";
    self->config_.rtp_max_rate_bps = max_data_rate_bps;
  }

  // RTP recv rate tracking [F5]
  if (self->features_.rtp_recv_rate && rtp_bytes_acked > 0) {
    self->rtp_samples_total_bytes_ += rtp_bytes_acked;
    self->rtp_ack_samples_.push_back({feedback_time_ms, rtp_bytes_acked});
    while (!self->rtp_ack_samples_.empty() &&
           (feedback_time_ms - self->rtp_ack_samples_.front().timestamp_ms) > kAckWindowMs) {
      self->rtp_samples_total_bytes_ -= self->rtp_ack_samples_.front().bytes;
      self->rtp_ack_samples_.pop_front();
    }
  }

  // Compute BUR and make rate decision
  self->ComputeBurAndDecide(feedback_time_ms);
}

// ============================================================
// ComputeBurAndDecide — V14 core: OWD-based BUR + rate control
// Called from GCC thread (via OnTwccFeedbackComplete) or
// from network thread (SACK fallback when no TWCC)
// ============================================================

void RtpSctpCoordinator::ComputeBurAndDecide(int64_t now_ms) {
  int64_t now_us = now_ms * 1000;
  const double L_ms = features_.L_ms;

  // === Drain RTP excess queue and compute RTP BUR (avg) ===
  double excess_rtp_ms = 0.0;
  double bur_rtp = 0.0;
  // Always drain RTP BUR queue (frame OWD is computed for all agentrtc modes)
  if (acc_delay_initialized_) {
    std::deque<double> rtp_samples;
    {
      std::lock_guard<std::mutex> lock(rtp_queue_mutex_);
      rtp_samples.swap(rtp_excess_queue_);
    }
    if (!rtp_samples.empty()) {
      double sum = 0.0;
      for (double e : rtp_samples) sum += e;
      excess_rtp_ms = sum / static_cast<double>(rtp_samples.size());
      if (excess_rtp_ms < 0.0) excess_rtp_ms = 0.0;
      bur_rtp = excess_rtp_ms / L_ms;
    }
  }

  // === Drain SCTP BUR (mode-dependent) ===
  double excess_sctp_ms = 0.0;
  double bur_sctp = 0.0;
  int drained_sack_count = 0;
  if (features_.sctp_bur) {
    if (features_.sctp_bur_mode == SctpBurMode::kRttMinAll) {
      // kRttMinAll: time-weighted queue ratio using ALL SACKs.
      // Σ min(excess_i, gap_i) / Σ gap_i — scale-invariant regardless of SACK count.
      // SCTP-only (not combined with bur_rtp).
      std::deque<SctpRttSample> samples;
      {
        std::lock_guard<std::mutex> lock(sctp_queue_mutex_);
        samples.swap(sctp_rtt_queue_);
      }
      int64_t rtt_min = rtt_min_us_.load(std::memory_order_relaxed);
      drained_sack_count = static_cast<int>(samples.size());
      if (rtt_min > 0 && !samples.empty()) {
        double sum_capped = 0.0;
        double sum_gap = 0.0;
        for (const auto& s : samples) {
          double excess = static_cast<double>(s.rtt_us - rtt_min);
          if (excess < 0.0) excess = 0.0;
          double gap = static_cast<double>(s.gap_us);
          if (gap < 1.0) gap = 1.0;
          sum_capped += std::min(excess, gap);
          sum_gap += gap;
        }
        if (sum_gap > 0) {
          bur_sctp = sum_capped / sum_gap;
          excess_sctp_ms = bur_sctp * L_ms;
        }
      }
    } else if (features_.sctp_bur_mode == SctpBurMode::kRttAdditive ||
        features_.sctp_bur_mode == SctpBurMode::kRttMinFrameHi) {
      // kRttAdditive / kRttMinFrameHi: SCTP correction computed at frame boundary
      // (OnPudicaPacketFeedback) and pushed to sctp_excess_queue_. Just drain it here.
      std::deque<double> sctp_samples;
      {
        std::lock_guard<std::mutex> lock(rtp_queue_mutex_);
        sctp_samples.swap(sctp_excess_queue_);
      }
      if (!sctp_samples.empty()) {
        double sum = 0.0;
        for (double e : sctp_samples) sum += e;
        excess_sctp_ms = sum / static_cast<double>(sctp_samples.size());
        if (excess_sctp_ms < 0.0) excess_sctp_ms = 0.0;
        bur_sctp = excess_sctp_ms / L_ms;
        drained_sack_count = static_cast<int>(sctp_samples.size());
      }
    } else {
      // kRttRef / kRttMin: drain sctp_rtt_queue_ and compute here
      std::deque<SctpRttSample> samples;
      {
        std::lock_guard<std::mutex> lock(sctp_queue_mutex_);
        samples.swap(sctp_rtt_queue_);
      }

      int64_t rtt_min = rtt_min_us_.load(std::memory_order_relaxed);
      drained_sack_count = static_cast<int>(samples.size());

      if (rtt_min > 0 && !samples.empty()) {
        if (features_.sctp_bur_mode == SctpBurMode::kRttMin) {
          // kRttMin: median(RTT - RTT_min) / L — absolute queue depth
          std::vector<double> queue_samples;
          queue_samples.reserve(samples.size());
          for (const auto& s : samples) {
            double q = static_cast<double>(s.rtt_us - rtt_min);
            if (q < 0.0) q = 0.0;
            queue_samples.push_back(q);
          }
          std::sort(queue_samples.begin(), queue_samples.end());
          size_t mid = queue_samples.size() / 2;
          double median_us = (queue_samples.size() % 2 == 0)
              ? (queue_samples[mid - 1] + queue_samples[mid]) / 2.0
              : queue_samples[mid];
          excess_sctp_ms = median_us / 1000.0;
          bur_sctp = excess_sctp_ms / L_ms;
        } else {
          // kRttRef: avg(RTT - RTT_ref) / L — original per-frame delta
          double sum_delta_us = 0.0;
          for (const auto& s : samples) {
            int64_t ref = (s.rtt_ref_us > 0) ? s.rtt_ref_us : rtt_min;
            double raw_excess = static_cast<double>(s.rtt_us - ref);
            if (raw_excess < 0.0) raw_excess = 0.0;
            sum_delta_us += raw_excess;
          }
          excess_sctp_ms = (sum_delta_us / static_cast<double>(samples.size())) / 1000.0;
          bur_sctp = excess_sctp_ms / L_ms;
        }
      }
    }
  }

  // === Combine BUR [F4] ===
  double bur;
  if (features_.sctp_bur_mode == SctpBurMode::kRttMinAll) {
    // SCTP-only: no RTP component
    bur = bur_sctp;
  } else if (features_.sctp_bur_mode == SctpBurMode::kRttAdditive ||
      features_.sctp_bur_mode == SctpBurMode::kRttMinFrameHi) {
    // Pudica-style: frame BUR + SCTP correction (always additive)
    bur = bur_rtp + bur_sctp;
  } else {
    bur = CombineBur(bur_rtp, bur_sctp);
  }

  // Debug: periodic pacing rate + BUR report with RTT_ref diagnostics
  static int64_t last_debug_ms = 0;
  if (now_ms - last_debug_ms >= 1000) {
    fprintf(stderr, "[BUR-DEBUG] t=%lld bur_rtp=%.3f bur_sctp=%.3f bur=%.3f "
            "pacing=%.1fMbps rtt_ref=%.1fms rtt_min=%.1fms sacks=%d\n",
            (long long)(now_ms % 100000), bur_rtp, bur_sctp, bur,
            pacing_rate_bps_.load() / 1e6,
            rtt_ref_us_.load(std::memory_order_relaxed) / 1000.0,
            rtt_min_us_.load(std::memory_order_relaxed) / 1000.0,
            drained_sack_count);
    last_debug_ms = now_ms;
  }

  // Store breakdown for logging
  last_bur_rtp_ = bur_rtp;
  last_bur_sctp_ = bur_sctp;
  last_bur_combined_ = bur;
  last_bur_ = bur;
  last_excess_rtp_ms_ = excess_rtp_ms;
  last_excess_sctp_ms_ = excess_sctp_ms;

  // EWMA smoothed BUR
  double pre_ewma_smoothed = smoothed_bur_;
  if (!ewma_initialized_) {
    smoothed_bur_ = bur;
    ewma_initialized_ = true;
  } else {
    smoothed_bur_ = config_.ewma_alpha * bur +
                    (1.0 - config_.ewma_alpha) * smoothed_bur_;
  }

  // Track completion time
  last_completion_us_ = now_us;

  // Reset SACK counter (atomic — network thread increments, we reset)
  atomic_sack_count_.store(0, std::memory_order_relaxed);

  // Compute sent throughput for CSV
  if (unified_metrics_.last_log_time_ms > 0) {
    int64_t dt = now_ms - unified_metrics_.last_log_time_ms;
    if (dt > 0) {
      if (unified_metrics_.sctp_bytes_accumulated > 0) {
        unified_metrics_.sctp_kbps =
            (unified_metrics_.sctp_bytes_accumulated * 8) / dt;
      } else {
        unified_metrics_.sctp_kbps = 0;
      }
    }
  }

  // === SCTP idle restart (RFC 2581 §4.1 style) ===
  // If no SCTP data sent this interval, decay pacing rate by half per RTT,
  // down to initial_rate. Prevents stale high rate after idle periods.
  // On SCTP resume, transmission starts at the decayed rate.
  //
  // Guard: only count as "idle" when there are no in-flight bytes. Under
  // RACK, the sender can briefly stop pushing *new* bytes while it waits
  // on a retransmit timer / reorder window, but unacked_bytes > 0 means
  // SCTP is still actively trying to drain the network. Decaying pacing
  // there crashes BUR/pacing to floor and permanently stalls HAFS+RACK.
  if (unified_metrics_.sctp_bytes_accumulated == 0 &&
      unified_metrics_.unacked_bytes == 0) {
    int64_t rtt_us = rtt_min_us_.load(std::memory_order_relaxed);
    int64_t rtt_ms = (rtt_us > 0) ? rtt_us / 1000 : 100;  // fallback 100ms
    if (rtt_ms < 10) rtt_ms = 10;

    // Track idle duration
    if (idle_start_us_ == 0) {
      idle_start_us_ = now_us;
    }
    int64_t idle_ms = (now_us - idle_start_us_) / 1000;

    if (idle_ms > rtt_ms) {
      // Decay: halve rate for each RTT of idle time elapsed since last decay
      int shifts = static_cast<int>(idle_ms / rtt_ms);
      if (shifts > 0) {
        int64_t old_rate = pacing_rate_bps_.load(std::memory_order_relaxed);
        int64_t new_rate = old_rate >> std::min(shifts, 10);  // cap at 10 halvings
        new_rate = std::max(new_rate, config_.initial_rate_bps);
        pacing_rate_bps_.store(new_rate, std::memory_order_relaxed);
      }
    }

    // Clear stale metrics
    unified_metrics_.sctp_kbps = 0;
    smoothed_bur_ *= 0.5;
    receiving_rate_bps_ = 0;
    unified_metrics_.receiving_rate_kbps = 0;
    unified_metrics_.mode = "SCTP-IDLE";
    unified_metrics_.current_bur = smoothed_bur_;
    unified_metrics_.smoothed_bur = smoothed_bur_;
    unified_metrics_.pacing_rate_mbps = pacing_rate_bps_.load() / 1'000'000;
    AllocateUnifiedRate();
    LogToCsv(now_us);
    unified_metrics_.sctp_bytes_accumulated = 0;
    unified_metrics_.last_log_time_ms = now_ms;
    return;  // No MI, no rate decision — just decay
  }
  idle_start_us_ = 0;  // SCTP active → reset idle tracker

  // Update CSV metrics
  unified_metrics_.current_bur = bur;
  unified_metrics_.smoothed_bur = smoothed_bur_;
  unified_metrics_.rtt_min_ms =
      (rtt_min_us_.load(std::memory_order_relaxed) > 0)
          ? rtt_min_us_.load(std::memory_order_relaxed) / 1000
          : 0;
  unified_metrics_.L_ms = static_cast<int64_t>(L_ms);
  unified_metrics_.pacing_rate_mbps = pacing_rate_bps_.load() / 1'000'000;
  unified_metrics_.bur_rtp = bur_rtp;
  unified_metrics_.bur_sctp = bur_sctp;
  unified_metrics_.bur_combined = bur;
  unified_metrics_.excess_rtp_ms = excess_rtp_ms;
  unified_metrics_.excess_sctp_ms = excess_sctp_ms;
  unified_metrics_.combine_mode = CombineModeStr(features_.combine);

  // RTP/Total recv rate for CSV
  int64_t rtp_recv = GetRtpRecvRateBps();
  unified_metrics_.rtp_recv_kbps = rtp_recv / 1000;
  unified_metrics_.total_recv_kbps = GetTotalRecvRateBps() / 1000;

  // === Skip rate decision if no SCTP data ===
  // All modes except kRtp need SCTP SACK feedback for meaningful BUR.
  // Without SACKs: bur_sctp=0, which creates a loophole (kMax/kWeightedAvg
  // would see low BUR → PROBE). Skip SCTP pacing decision and let GCC
  // handle RTP independently.
  // kFallback: use bur_rtp for rate decision instead of skipping.
  bool needs_sctp_data = (features_.combine != BurCombineMode::kRtp);
  if (needs_sctp_data && drained_sack_count == 0) {
    if (features_.combine == BurCombineMode::kFallback && bur_rtp > 0) {
      // Fallback: use bur_rtp for rate decision
      bur = bur_rtp;
      last_bur_combined_ = bur;
      last_bur_ = bur;
      smoothed_bur_ = config_.ewma_alpha * bur +
                      (1.0 - config_.ewma_alpha) * smoothed_bur_;
      unified_metrics_.current_bur = bur;
      unified_metrics_.smoothed_bur = smoothed_bur_;
      unified_metrics_.bur_combined = bur;
      unified_metrics_.combine_mode = "fallback-rtp";
      // Continue to rate decision below (don't return)
    } else if (features_.combine == BurCombineMode::kDecay) {
      // Decay: halve rate every 200ms toward init_rate when no SACK
      if (now_us - last_decay_us_ >= 200'000) {
        int64_t current = pacing_rate_bps_.load();
        int64_t target = config_.initial_rate_bps;
        if (current > target) {
          int64_t new_rate = std::max(target, current / 2);
          pacing_rate_bps_.store(new_rate, std::memory_order_relaxed);
          unified_metrics_.mode = "DECAY";
          unified_metrics_.rate_delta_mbps = (new_rate - current) / 1'000'000.0;
          unified_metrics_.pacing_rate_mbps = new_rate / 1'000'000;
          RTC_LOG(LS_INFO) << "[BUR-COORD] DECAY: no SACK, rate "
                           << (current / 1e6) << " -> " << (new_rate / 1e6)
                           << " Mbps (target=" << (target / 1e6) << ")";
        }
        last_decay_us_ = now_us;
      }
      unified_metrics_.combine_mode = "decay-nosack";
      AllocateUnifiedRate();
      LogToCsv(now_us);
      unified_metrics_.sctp_bytes_accumulated = 0;
      unified_metrics_.last_log_time_ms = now_ms;
      return;
    } else if (features_.combine == BurCombineMode::kAdditive && bur_rtp > 0) {
      // Additive with no SACKs: fallback to RTP BUR only.
      // Prevents blind MI ramp during flow set gaps.
      bur = bur_rtp;
      last_bur_combined_ = bur;
      last_bur_ = bur;
      smoothed_bur_ = config_.ewma_alpha * bur +
                      (1.0 - config_.ewma_alpha) * smoothed_bur_;
      unified_metrics_.current_bur = bur;
      unified_metrics_.smoothed_bur = smoothed_bur_;
      unified_metrics_.bur_combined = bur;
      unified_metrics_.combine_mode = "additive-rtp-only";
      // Continue to rate decision below
    } else if (bur_rtp > 0) {
      // No SACKs but RTP frame OWD available — use RTP BUR as fallback.
      bur = bur_rtp;
      last_bur_combined_ = bur;
      last_bur_ = bur;
      smoothed_bur_ = config_.ewma_alpha * bur +
                      (1.0 - config_.ewma_alpha) * smoothed_bur_;
    } else {
      // No SACKs AND no RTP BUR — truly idle. Hold rate.
      smoothed_bur_ = (1.0 - config_.ewma_alpha * 0.5) * pre_ewma_smoothed;
      unified_metrics_.current_bur = smoothed_bur_;
      unified_metrics_.smoothed_bur = smoothed_bur_;
      unified_metrics_.bur_combined = smoothed_bur_;
      unified_metrics_.combine_mode = "nosack-hold";
      AllocateUnifiedRate();
      LogToCsv(now_us);
      unified_metrics_.sctp_bytes_accumulated = 0;
      unified_metrics_.last_log_time_ms = now_ms;
      return;  // Skip rate decision — truly idle
    }
  }

  // === Feedback-matched rate control ===
  int64_t current_rate = pacing_rate_bps_.load();
  bool is_matched = (interval_pacing_rate_ == current_rate);
  int64_t rtt_min_val = rtt_min_us_.load(std::memory_order_relaxed);
  int64_t feedback_timeout_us = (rtt_min_val > 0 ? rtt_min_val : 100'000) + 100'000;
  bool wait_expired = feedback_pending_ &&
                      (now_us - rate_change_us_) > feedback_timeout_us;

  bool should_decide = false;
  if (!feedback_pending_) {
    int64_t since_last = now_us - last_rate_decision_us_;
    bool is_urgent = (bur > config_.bur_threshold) ||
                     draining_active_ ||
                     (consecutive_high_bur_ >= 2);
    should_decide = (since_last >= config_.rate_decision_interval_ms * 1000)
                    || (last_rate_decision_us_ == 0)
                    || is_urgent;
  } else if (is_matched) {
    should_decide = true;
    feedback_pending_ = false;
    RTC_LOG(LS_VERBOSE) << "[BUR-COORD] Matched feedback: rate="
                        << (current_rate / 1e6) << "Mbps BUR=" << bur;
  } else if (wait_expired) {
    should_decide = true;
    feedback_pending_ = false;
    RTC_LOG(LS_INFO) << "[BUR-COORD] Feedback timeout: waited "
                     << ((now_us - rate_change_us_) / 1000) << "ms"
                     << " interval_rate=" << (interval_pacing_rate_ / 1e6)
                     << " current_rate=" << (current_rate / 1e6);
  }

  if (should_decide) {
    if (features_.measure_only) {
      // Measure-only: fix pacing at max_rate (don't adjust based on BUR)
      pacing_rate_bps_.store(config_.max_rate_bps, std::memory_order_relaxed);
    } else {
      AdjustPacingRate(bur, "TWCC", now_us);
    }
    last_rate_decision_us_ = now_us;

    // Periodic MAFS priority update (piggyback on rate decision cycle)
    MaybeUpdateMafsPriorities(now_us / 1000);
  }

  // Update interval state for next cycle
  interval_start_us_ = now_us;
  interval_pacing_rate_ = pacing_rate_bps_.load();

  // Allocate unified rate → SCTP pacing
  AllocateUnifiedRate();

  // Log
  LogToCsv(now_us);

  // Reset accumulators
  unified_metrics_.sctp_bytes_accumulated = 0;
  unified_metrics_.last_log_time_ms = now_ms;

  RTC_LOG(LS_VERBOSE) << "[BUR-COORD] BUR computed:"
                      << " bur_rtp=" << bur_rtp
                      << " bur_sctp=" << bur_sctp
                      << " bur=" << bur
                      << " excess_rtp=" << excess_rtp_ms << "ms"
                      << " excess_sctp=" << excess_sctp_ms << "ms"
                      << " L=" << L_ms << "ms"
                      << " sacks=" << drained_sack_count
                      << " rate=" << (pacing_rate_bps_.load() / 1e6) << "Mbps";
}

// ============================================================
// OnChunkSent — called when pacer sends a DATA chunk
// V14: just track bytes for throughput CSV
// ============================================================

void RtpSctpCoordinator::OnChunkSent(uint32_t tsn, int64_t bytes,
                                      int64_t now_us) {
  if (config_.mode != CoordinatorMode::kAgentRtc &&
      config_.mode != CoordinatorMode::kFse &&
      config_.mode != CoordinatorMode::kPudica) return;
  if (tsn == 0) return;

  unified_metrics_.sctp_bytes_accumulated += bytes;
}

// ============================================================
// OnSackReceived — push RTT to queue, update recv rate
// Network thread only. No direct rate decision.
// SACK fallback: if TWCC isn't triggering, complete after threshold.
// ============================================================

void RtpSctpCoordinator::OnSackReceived(uint32_t cumulative_tsn_ack,
                                         int64_t rtt_us,
                                         int64_t bytes_acked,
                                         bool has_packet_loss,
                                         int64_t now_us) {
  // Disabled mode: passive logging only
  if (config_.mode != CoordinatorMode::kAgentRtc &&
      config_.mode != CoordinatorMode::kFse &&
      config_.mode != CoordinatorMode::kPudica) {
    int64_t now_ms = now_us / 1000;
    if (bytes_acked > 0) {
      ack_samples_total_bytes_ += bytes_acked;
      ack_samples_.push_back({now_ms, static_cast<int64_t>(bytes_acked)});
      while (!ack_samples_.empty() &&
             (now_ms - ack_samples_.front().timestamp_ms) > GetAckWindowMs()) {
        ack_samples_total_bytes_ -= ack_samples_.front().bytes;
        ack_samples_.pop_front();
      }
      int64_t ack_rate = GetAckRateBps();
      if (ack_rate > 0) {
        receiving_rate_bps_ = ack_rate;
        unified_metrics_.receiving_rate_kbps = ack_rate / 1000;
      }
    }
    if (rtt_us > 0) {
      int64_t cur_min = rtt_min_us_.load(std::memory_order_relaxed);
      if (cur_min < 0 || rtt_us < cur_min) {
        rtt_min_us_.store(rtt_us, std::memory_order_relaxed);
      }
      unified_metrics_.rtt_min_ms =
          (rtt_min_us_.load(std::memory_order_relaxed) > 0)
              ? rtt_min_us_.load(std::memory_order_relaxed) / 1000
              : 0;
      unified_metrics_.rtt_max_ms = rtt_us / 1000;
    }
    unified_metrics_.rtp_recv_kbps = GetRtpRecvRateBps() / 1000;
    unified_metrics_.total_recv_kbps = GetTotalRecvRateBps() / 1000;
    if (now_ms - passive_log_last_ms_ >= 100) {
      passive_log_last_ms_ = now_ms;
      unified_metrics_.mode = "DISABLED";
      LogToCsv(now_us);
    }
    return;
  }

  int64_t now_ms = now_us / 1000;

  // RTT minimum tracking: 10s sliding window (BBR-style).
  // Without windowing, rtt_min is a lifetime global min that goes stale
  // after path changes (WiFi AP switch, jitter). Stale rtt_min makes
  // every normal RTT look like congestion → BUR permanently elevated →
  // rate collapse with no recovery (see BUR_RECOVERY_ANALYSIS.md).
  // Override window size: RTT_MIN_WINDOW_S=0 reverts to lifetime min.
  if (rtt_us > 0) {
    static const int64_t kWindowUs = []() {
      const char* e = std::getenv("RTT_MIN_WINDOW_S");
      return e ? static_cast<int64_t>(std::atof(e) * 1e6)
               : 10'000'000;  // default 10s
    }();
    if (kWindowUs > 0) {
      std::lock_guard<std::mutex> lock(sctp_queue_mutex_);
      // Monotonic deque: back-remove any entry >= new value (they can never
      // be the window min while the new entry is alive), then push new entry.
      while (!rtt_min_window_.empty() &&
             rtt_min_window_.back().rtt_us >= rtt_us) {
        rtt_min_window_.pop_back();
      }
      rtt_min_window_.push_back({now_us, rtt_us});
      // Expire front entries outside the time window.
      while (!rtt_min_window_.empty() &&
             now_us - rtt_min_window_.front().time_us > kWindowUs) {
        rtt_min_window_.pop_front();
      }
      // Front is always the minimum — O(1).
      if (!rtt_min_window_.empty()) {
        rtt_min_us_.store(rtt_min_window_.front().rtt_us,
                          std::memory_order_relaxed);
      }
    } else {
      int64_t cur_min = rtt_min_us_.load(std::memory_order_relaxed);
      if (cur_min < 0 || rtt_us < cur_min) {
        rtt_min_us_.store(rtt_us, std::memory_order_relaxed);
      }
    }
    unified_metrics_.rtt_max_ms = rtt_us / 1000;
  }

  // FSE mode: track CWND and RTT, trigger redistribution
  if (config_.mode == CoordinatorMode::kFse) {
    // Update SCTP ACK sliding window
    if (bytes_acked > 0) {
      ack_samples_total_bytes_ += bytes_acked;
      ack_samples_.push_back({now_ms, static_cast<int64_t>(bytes_acked)});
      while (!ack_samples_.empty() &&
             (now_ms - ack_samples_.front().timestamp_ms) > GetAckWindowMs()) {
        ack_samples_total_bytes_ -= ack_samples_.front().bytes;
        ack_samples_.pop_front();
      }
      int64_t ack_rate = GetAckRateBps();
      if (ack_rate > 0) {
        receiving_rate_bps_ = ack_rate;
        unified_metrics_.receiving_rate_kbps = ack_rate / 1000;
      }
    }
    // Trigger FSE redistribution with CWND
    int64_t cwnd = unified_metrics_.cwnd_bytes;
    if (cwnd > 0 && rtt_us > 0) {
      FseOnSctpUpdate(static_cast<double>(cwnd), rtt_us, now_us);
    }
    return;
  }

  // FSEv2 lightweight: NC SCTP path + 1Hz fixed override store.
  // Falls through to NC processing below — only adds a periodic override store.
  if (config_.mode == CoordinatorMode::kFseV2) {
    constexpr int64_t kFseV2IntervalUs = 1'000'000;  // 1 Hz
    if (now_us - fse_v2_last_override_us_ >= kFseV2IntervalUs) {
      rtp_fse_override_bps_.store(static_cast<int64_t>(fse_dr_rtp_bps_),
                                  std::memory_order_relaxed);
      fse_v2_last_override_us_ = now_us;
    }
    // Do NOT return — let SCTP CC use NC's full SACK path (NewReno freely).
  }

  // Push RTT sample to queue for GCC thread to drain (paper Eq.5: δ_i)
  if (rtt_us > 0) {
    int64_t gap_us = (last_sack_time_us_ > 0) ? (now_us - last_sack_time_us_) : 0;
    last_sack_time_us_ = now_us;
    last_raw_rtt_us_.store(rtt_us, std::memory_order_relaxed);
    int64_t cur_ref = rtt_ref_us_.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(sctp_queue_mutex_);
    sctp_rtt_queue_.push_back({now_us, rtt_us, gap_us, cur_ref});
    // SACK history: store (send_time, rtt) for RTT_ref timestamp matching
    int64_t send_time_us = now_us - rtt_us;  // approximate when the acked packet was sent
    sack_history_.push_back({send_time_us, rtt_us});
    while (!sack_history_.empty() &&
           (now_us - sack_history_.front().send_time_us - sack_history_.front().rtt_us) > kSackHistoryWindowUs) {
      sack_history_.pop_front();
    }
  }

  // Update SCTP ACK sliding window for receiving rate
  if (bytes_acked > 0) {
    ack_samples_total_bytes_ += bytes_acked;
    ack_samples_.push_back({now_ms, static_cast<int64_t>(bytes_acked)});
    while (!ack_samples_.empty() &&
           (now_ms - ack_samples_.front().timestamp_ms) > GetAckWindowMs()) {
      ack_samples_total_bytes_ -= ack_samples_.front().bytes;
      ack_samples_.pop_front();
    }
  }

  // Update receiving rate
  int64_t ack_rate = GetAckRateBps();
  if (ack_rate > 0) {
    receiving_rate_bps_ = ack_rate;
    unified_metrics_.receiving_rate_kbps = ack_rate / 1000;

    // Update max_delivery_rate (BBR btlbw): sliding window peak.
    // Monotonic decreasing deque — O(1) amortized, no rescan ever.
    static const int64_t kBtlBwWindowUs = []() {
      const char* e = std::getenv("BUR_BTLBW_WINDOW_S");
      return e ? static_cast<int64_t>(std::atof(e) * 1e6)
               : 10'000'000LL;
    }();
    int64_t total_recv = ack_rate + GetRtpRecvRateBps();
    int64_t now_us2 = now_ms * 1000;

    // Maintain decreasing invariant: pop back while back <= new value.
    while (!max_delivery_window_.empty() &&
           max_delivery_window_.back().second <= total_recv) {
      max_delivery_window_.pop_back();
    }
    max_delivery_window_.push_back({now_us2, total_recv});

    // Expire old samples from front.
    while (!max_delivery_window_.empty() &&
           now_us2 - max_delivery_window_.front().first > kBtlBwWindowUs) {
      max_delivery_window_.pop_front();
    }

    // Front is always the max.
    int64_t new_max = max_delivery_window_.empty()
                      ? 0 : max_delivery_window_.front().second;
    max_delivery_rate_bps_.store(new_max, std::memory_order_relaxed);
  }

  // Track SACK count for fallback completion trigger
  int sack_count = atomic_sack_count_.fetch_add(1, std::memory_order_relaxed) + 1;

  // RTT-CONGEST removed: rely solely on BUR-based detection (DRAINING / RAW-CORR)
  // which uses config_.spike_threshold as the single tunable threshold.
  int64_t rtt_min = rtt_min_us_.load(std::memory_order_relaxed);

  // Normal SACK with reasonable RTT → congestion recovery
  if (congestion_count_ > 0 || timeout_was_draining_) {
    // Only recover if RTT is reasonable (< 2× RTT_min)
    bool rtt_ok = (rtt_min <= 0 || rtt_us <= 0 || rtt_us < rtt_min * 2);
    if (rtt_ok) {
      if (timeout_was_draining_) {
        int64_t current = pacing_rate_bps_.load();
        int64_t recv = receiving_rate_bps_;
        if (recv > 0 && current < recv) {
          pacing_rate_bps_.store(recv, std::memory_order_relaxed);
          RTC_LOG(LS_INFO) << "[BUR-COORD] TIMEOUT-RECOVER: rate restored "
                           << (current / 1e6) << " -> " << (recv / 1e6) << " Mbps";
        }
      }
      congestion_count_ = 0;
      spike_applied_ = false;
      timeout_was_draining_ = false;
    }
  }

  // === SACK fallback completion ===
  // If TWCC feedback isn't arriving (or hasn't started yet), complete
  // the interval from the network thread based on SACK count + time.
  bool twcc_active = (last_completion_us_ > 0) &&
                     (now_us - last_completion_us_) < 200'000;  // 200ms
  if (!twcc_active && sack_count >= 2) {
    int64_t age_us = (interval_start_us_ > 0)
                         ? (now_us - interval_start_us_)
                         : config_.rate_decision_interval_ms * 1000;
    if (age_us >= config_.rate_decision_interval_ms * 1000) {
      ComputeBurAndDecide(now_ms);
    }
  }
}

// ============================================================
// CheckTimeouts — called from DrainPacingQueue periodically
// Feedback timeout: detect no-SACK condition using last_sack_time_us_.
// Unlike last_completion_us_ (refreshed by TWCC), last_sack_time_us_ is
// only updated when an actual SCTP SACK arrives. This prevents the bug
// where TWCC keeps ComputeBurAndDecide running → BUR decays to 0 → PROBE
// increases rate during WiFi congestion.
// ============================================================

void RtpSctpCoordinator::CheckTimeouts(int64_t now_us) {
  if (config_.mode != CoordinatorMode::kAgentRtc &&
      config_.mode != CoordinatorMode::kPudica) return;

  // Don't timeout before first SACK (startup)
  if (last_sack_time_us_ == 0) {
    return;
  }

  // SACK arriving normally — no timeout
  int64_t feedback_timeout_us = config_.timeout_ms * 1000;
  if ((now_us - last_sack_time_us_) < feedback_timeout_us) {
    return;
  }

  // Don't timeout during idle periods (no SCTP data flowing).
  // Use wider window than ACK rate staleness (kAckWindowMs × 2 = 400ms)
  // to prevent false timeout in the gap between SACK stopping and
  // ACK rate decaying to zero.
  if (GetAckRateBps() <= 0) {
    return;
  }
  // Additional idle guard: if SACK stopped recently (within 2× ACK window),
  // ACK rate hasn't decayed yet — this is idle transition, not congestion.
  int64_t sack_age_us = now_us - last_sack_time_us_;
  if (sack_age_us < (kAckWindowMs * 2 + 100) * 1000) {
    // SACK stopped 300-500ms ago, ACK rate still positive → idle transition
    // Wait for ACK rate to decay before judging congestion.
    return;
  }

  // Feedback timeout: no SACK for > timeout_ms
  int64_t age_ms = (now_us - last_sack_time_us_) / 1000;
  RTC_LOG(LS_WARNING) << "[BUR-COORD] FEEDBACK-TIMEOUT: no SACK for "
                      << age_ms << "ms"
                      << " (deadline=" << config_.timeout_ms << "ms)";
  OnCongestionTimeout(now_us);
  last_sack_time_us_ = now_us;  // Reset to avoid repeated timeouts per interval
}

// ============================================================
// OnCongestionTimeout — rate reduction on feedback timeout
// ============================================================

void RtpSctpCoordinator::OnCongestionTimeout(int64_t now_us) {
  congestion_count_++;
  feedback_pending_ = false;

  int64_t old_rate = pacing_rate_bps_.load();
  int64_t recv_rate = features_.total_recv_rate ? GetTotalRecvRateBps()
                                                : receiving_rate_bps_;
  if (recv_rate <= 0) recv_rate = old_rate;

  int64_t new_rate = old_rate;
  const char* mode;

  if (congestion_count_ <= 2) {
    // Spike phase: apply rate reduction ONCE per congestion episode.
    // Without this guard, repeated timeouts would cut rate multiple times
    // during spike phase before transitioning to draining.
    if (!spike_applied_) {
      new_rate = static_cast<int64_t>(old_rate * config_.spike_reduction);
      spike_applied_ = true;
      mode = "TIMEOUT-SPIKE";
    } else {
      mode = "TIMEOUT-SPIKE-HOLD";
    }
  } else {
    // Draining phase: actively drain excess buffer
    timeout_was_draining_ = true;
    // Use sending rate (old_rate) instead of recv_rate for consistency.
    // drain_rate is implicit (α < 1 already reduces from sending rate).
    new_rate = static_cast<int64_t>(
        config_.spike_reduction * old_rate);
    mode = "TIMEOUT-DRAIN";
  }

  new_rate = std::max(config_.min_rate_bps,
                      std::min(config_.max_rate_bps, new_rate));
  pacing_rate_bps_.store(new_rate, std::memory_order_relaxed);

  unified_metrics_.mode = mode;
  unified_metrics_.rate_delta_mbps = (new_rate - old_rate) / 1'000'000.0;
  unified_metrics_.recv_rate_mbps = recv_rate / 1'000'000.0;
  unified_metrics_.consecutive_high = consecutive_high_bur_;

  last_bur_ = 1.5;
  unified_metrics_.current_bur = 1.5;
  unified_metrics_.pacing_rate_mbps = new_rate / 1'000'000;
  AllocateUnifiedRate();
  LogToCsv(now_us);

  RTC_LOG(LS_INFO) << "[BUR-COORD] " << mode
                   << " count=" << congestion_count_
                   << " recv=" << (recv_rate / 1e6) << "Mbps"
                   << " pacing: " << (old_rate / 1e6)
                   << " -> " << (new_rate / 1e6) << " Mbps";
}

// ============================================================
// AdjustPacingRate — BUR-based rate control (unchanged from V12)
// ============================================================

void RtpSctpCoordinator::AdjustPacingRate(double bur, const char* trigger,
                                           int64_t now_us) {
  // Skip rate adjustment entirely when no SCTP data is flowing.
  // During idle gaps between flow sets, only RTP remains. Stale SCTP
  // RTT data can cause spurious BUR spikes that crash the pacing rate.
  // Preserve the current rate for fast warm start on the next set.
  if (GetAckRateBps() <= 0) {
    smoothed_bur_ *= 0.5;
    consecutive_high_bur_ = 0;
    draining_active_ = false;
    return;
  }

  const double kBurThreshold = config_.bur_threshold;
  const double kDrainingBurThreshold = config_.draining_bur_threshold;
  const double kMaxMiMult = config_.max_mi_multiplier;
  const double kDrainingTarget = config_.draining_target;
  constexpr double kEpsilon = 0.01;

  int64_t old_rate = pacing_rate_bps_.load();
  int64_t recv_rate_raw = features_.total_recv_rate ? GetTotalRecvRateBps()
                                                    : receiving_rate_bps_;
  int64_t recv_rate = (recv_rate_raw <= 0) ? old_rate : recv_rate_raw;

  // Shared-queue guard: if pacing ~= recv, we're NOT over-pacing.
  // High BUR is then from competing flows (shared standing queue), not us.
  // Reducing our rate doesn't drain the queue (TCP keeps it full) — it just
  // gives up throughput for no benefit. Disable reduction modes in this case.
  // Active only when BUR_SHARED_GUARD_RATIO > 0 (default 0 disabled for safety).
  static const double kSharedGuardRatio = []() {
    const char* e = std::getenv("BUR_SHARED_GUARD_RATIO");
    return e ? std::atof(e) : 0.0;
  }();
  bool shared_queue_guard = false;
  bool guard_probe_fired = false;
  if (kSharedGuardRatio > 0.0 && recv_rate_raw > 0) {
    double pacing_to_recv =
        static_cast<double>(old_rate) / static_cast<double>(recv_rate_raw);
    // pacing/recv ≤ ratio means we're in balance (not over-pacing)
    shared_queue_guard = (pacing_to_recv <= kSharedGuardRatio);
  }

  double r_raw = bur;
  double r_tilde = smoothed_bur_;
  int64_t new_rate = old_rate;
  const char* mode = "STABLE";
  bool recovery_applied = false;

  // === SUSTAINED-CUT disabled ===
  // Previous behavior: if smoothed BUR stays > 0.7 for 20+ intervals, halve rate.
  // Disabled to observe pure MI/AI/RAW-CORR/DRAINING behavior without safety net.
  // if (r_tilde > kBurThreshold * 1.4) {
  //   sustained_high_count_++;
  //   if (sustained_high_count_ >= 20) {
  //     int64_t halved = old_rate / 2;
  //     halved = std::max(config_.min_rate_bps, halved);
  //     new_rate = halved;
  //     mode = "SUSTAINED-CUT";
  //     sustained_high_count_ = 0;
  //     pacing_rate_bps_.store(new_rate, std::memory_order_relaxed);
  //     unified_metrics_.mode = mode;
  //     unified_metrics_.rate_delta_mbps = (new_rate - old_rate) / 1'000'000.0;
  //     unified_metrics_.pacing_rate_mbps = new_rate / 1'000'000;
  //     unified_metrics_.consecutive_high = consecutive_high_bur_;
  //     AllocateUnifiedRate();
  //     LogToCsv(now_us);
  //     RTC_LOG(LS_WARNING) << "[BUR-COORD] SUSTAINED-CUT: smoothed_bur="
  //                         << r_tilde << " for 20+ intervals, rate "
  //                         << (old_rate / 1e6) << " -> " << (new_rate / 1e6) << " Mbps";
  //     return;
  //   }
  // } else {
  //   sustained_high_count_ = 0;
  // }

  // === Pudica-style active queue draining (NSDI'24 §4.3, Eq.11) ===
  // Entry: 3 consecutive frames with BUR > 1.0 (queue is building)
  // Draining: B_new = α × recv_rate - draining_rate
  //   draining_rate = queue_volume / 200ms (drain queue within 200ms)
  //   queue_volume = (RTT - RTT_min) × recv_rate (BDP-based estimate)
  // Recovery: BUR < 1.0 → one-step recovery to recv_rate
  if (r_raw > config_.spike_threshold) {
    consecutive_high_bur_++;
  } else {
    consecutive_high_bur_ = 0;
    if (draining_active_) {
      draining_active_ = false;
      // One-step recovery (Pudica §4.3): α × recv_rate
      // recovery_applied = true → skip rate control THIS interval only
      // Next interval: recovery_applied resets (local var) → MI/AI-MD runs normally
      if (recv_rate > 0) {
        new_rate = static_cast<int64_t>(kDrainingTarget * recv_rate);
        mode = "DRAIN-RECOVERY";
        recovery_applied = true;
        RTC_LOG(LS_INFO) << "[BUR-COORD] DRAIN-RECOVERY: rate="
                         << (new_rate / 1e6) << "Mbps (0.85×recv)";
      }
    }
  }

  // === RATE CONTROL PHASES ===
  // TRANS-HOLD removed entirely. Idle gaps are handled by:
  //   1. Early return above (GetAckRateBps() <= 0 → no rate change)
  //   2. recv_cap skip (recv_rate < old_rate × 0.5 → cap not applied)
  // These two together preserve rate through idle and prevent crash on
  // resume without freezing rate or creating unrecoverable hold traps.
  bool transition_hold = false;
  if (!recovery_applied) {
    // Shared-queue guard: only activate at BUR > 1.0 (true overflow). At
    // BUR 0.5-1.0 (normal saturation in standalone), AI-MD handles correctly
    // and guard would over-probe above capacity. Guard is only valuable when
    // we're SO over that AI-MD would crash pacing below fair share —
    // i.e., standing queue from competing flows.
    if (shared_queue_guard && r_raw > config_.spike_threshold) {
      int64_t probe_from_recv = static_cast<int64_t>(recv_rate * 1.4);
      int64_t probe_from_old = static_cast<int64_t>(old_rate * 1.4);
      new_rate = std::max(probe_from_recv, probe_from_old);
      mode = "SHARED-PROBE";
      guard_probe_fired = true;
      consecutive_high_bur_ = 0;
      draining_active_ = false;
    } else if (!config_.skip_draining && (draining_active_ || consecutive_high_bur_ >= 3)) {
      if (!draining_active_) {
        draining_active_ = true;
        // Snapshot sending rate (pacing) at DRAINING entry.
        // Was: recv_rate (200ms-lagged, can be artificially low from queue).
        // Now: old_rate (current pacing) — what we actually *want* to drain from.
        draining_recv_snapshot_ = old_rate;
        RTC_LOG(LS_INFO) << "[BUR-COORD] Entering DRAINING ("
                         << consecutive_high_bur_ << " consecutive BUR > 1.0)"
                         << " send_snapshot=" << (draining_recv_snapshot_/1e6) << "Mbps";
      }
      // Pudica Eq.11: B_new = α × send_snapshot - drain_rate
      // Using sending-rate snapshot (not recv) since the queue we want to
      // drain is caused by OUR sending, not by delivery lag.
      double queue_delay_ms = smoothed_bur_ * features_.L_ms;
      int64_t queue_bytes = static_cast<int64_t>(
          queue_delay_ms * draining_recv_snapshot_ / 8000.0);
      int64_t drain_rate_bps = (queue_bytes > 0)
          ? static_cast<int64_t>(queue_bytes * 8.0 / 0.200)  // drain in 200ms
          : 0;
      new_rate = static_cast<int64_t>(
          kDrainingTarget * draining_recv_snapshot_) - drain_rate_bps;
      new_rate = std::max(config_.min_rate_bps, new_rate);
      mode = "DRAINING";

    } else if (r_raw > config_.spike_threshold) {
      // Rate-based reduction from current pacing rate.
      // Using old_rate (not recv_rate) avoids the 200ms-lagged recv
      // causing a 50%+ crash on every spike.
      new_rate = static_cast<int64_t>(old_rate * config_.raw_corr_factor);
      mode = "RAW-CORR";

      if (recv_rate > 0 && smoothed_bur_ > 0.7) {
        capacity_est_bps_ = recv_rate;
        low_bur_count_ = 0;
      }

    } else if (r_tilde >= config_.alpha_target) {
      // TCP-style AI in narrow band near capacity (BUR 0.85~1.0).
      if (config_.ai_recv_alpha > 0) {
        int64_t recv_max = max_delivery_rate_bps_.load(std::memory_order_relaxed);
        if (recv_max > 0) {
          new_rate = old_rate + static_cast<int64_t>(config_.ai_recv_alpha * recv_max);
        } else {
          new_rate = old_rate;
        }
        mode = "AI-R";
      } else {
        constexpr double kMSS = 1200.0;
        int64_t rtt_min_val = rtt_min_us_.load(std::memory_order_relaxed);
        double rtt_s = (rtt_min_val > 0) ? rtt_min_val / 1e6 : 0.030;
        double tick_s = features_.L_ms / 1000.0;
        // BUR_AI_MULTIPLIER: multiplies AI step. default 3.0 (empirically best on fixed300).
        static const double kAiMultiplier = []() {
          const char* e = std::getenv("BUR_AI_MULTIPLIER");
          return e ? std::atof(e) : 3.0;
        }();
        double ai_step_bps = (kMSS * 8.0) / rtt_s;
        double ai_step_per_tick = ai_step_bps * (tick_s / rtt_s) * kAiMultiplier;
        new_rate = old_rate + static_cast<int64_t>(ai_step_per_tick);
        mode = "AI";
      }

    } else if (r_tilde > kEpsilon) {
      // MI: multiplicative increase up to BUR = alpha_target (0.85).
      // Base is recv_rate (actual delivered) rather than old_rate
      // (advertised pacing). When pacing overshoots delivery, old_rate ×
      // frac grows independently of what the link is actually carrying;
      // tying the base to recv_rate anchors pacing to observed capacity.
      //   new_rate = recv × (1 + max_step_frac × scale)
      // Fallback to old_rate when recv_rate is unavailable (first few
      // ticks before TWCC feedback stabilizes).
      double max_step_frac = config_.max_step_fraction;
      double scale = std::max(0.25, 1.0 - (r_tilde / config_.alpha_target) * 0.75);
      if (r_tilde < 0.1) {
        max_step_frac = std::max(max_step_frac, 0.20);
      }
      double base = (recv_rate > 0) ? static_cast<double>(recv_rate)
                                    : static_cast<double>(old_rate);
      base = std::max(base, 1e5);
      new_rate = static_cast<int64_t>(base * (1.0 + max_step_frac * scale));
      mode = "MI";

    } else {
      // BUR ≈ 0: aggressive multiplicative probe on old_rate (not recv).
      // Probe is specifically for discovering unused capacity when delivery
      // signal is absent / tiny — anchoring to old_rate lets pacing keep
      // growing even if recv is momentarily 0.
      new_rate = static_cast<int64_t>(old_rate * kMaxMiMult);
      mode = "PROBE";
    }
  }  // end else if (!recovery_applied)

  // Capacity estimate decay: reset when BUR stays low for extended period
  if (capacity_est_bps_ > 0 && r_tilde < kBurThreshold * 0.3) {
    if (++low_bur_count_ >= 20) {
      RTC_LOG(LS_INFO) << "[BUR-COORD] capacity_est reset (low BUR for "
                       << low_bur_count_ << " samples): "
                       << (capacity_est_bps_ / 1e6) << "M -> 0";
      capacity_est_bps_ = 0;
      low_bur_count_ = 0;
    }
  } else {
    low_bur_count_ = 0;
  }

  // No-feedback cap: without recv_rate, cap at capacity_est or initial_rate
  // (no evidence of link capacity → stay conservative)
  if (recv_rate <= 0 && capacity_est_bps_ <= 0) {
    new_rate = std::min(new_rate, config_.initial_rate_bps);
  } else if (recv_rate <= 0 && capacity_est_bps_ > 0) {
    new_rate = std::min(new_rate, capacity_est_bps_);
  }

  // recv_cap removed. BUR feedback is the sole rate limiter across all
  // tiers. The old recv_cap (recv × 1.2-1.5) created a self-limiting
  // loop: pacing cut → throughput ↓ → recv ↓ → recv_cap ↓ → pacing
  // stuck. With BUR as the only signal, overshoot → BUR rises → MI/AI
  // naturally reduces rate. No external cap needed.

  new_rate = std::max(config_.min_rate_bps,
                      std::min(config_.max_rate_bps, new_rate));
  pacing_rate_bps_.store(new_rate, std::memory_order_relaxed);

  unified_metrics_.mode = mode;
  unified_metrics_.rate_delta_mbps = (new_rate - old_rate) / 1'000'000.0;
  unified_metrics_.recv_rate_mbps = recv_rate / 1'000'000.0;
  unified_metrics_.consecutive_high = consecutive_high_bur_;

  if (old_rate != new_rate) {
    feedback_pending_ = true;
    rate_change_us_ = now_us;
    rate_after_change_ = new_rate;

    RTC_LOG(LS_INFO) << "[BUR-COORD] " << mode
                     << " R_raw=" << r_raw
                     << " R_tilde=" << r_tilde
                     << " bur_rtp=" << last_bur_rtp_
                     << " bur_sctp=" << last_bur_sctp_
                     << " excess_rtp=" << last_excess_rtp_ms_ << "ms"
                     << " excess_sctp=" << last_excess_sctp_ms_ << "ms"
                     << " consec=" << consecutive_high_bur_
                     << " recv=" << (recv_rate / 1e6) << "Mbps"
                     << " pacing: " << (old_rate / 1e6)
                     << " -> " << (new_rate / 1e6) << " Mbps"
                     << " [" << trigger << "]";
  }
}

// ============================================================
// ComputeFairnessAimdRate — VCP-style BUR-proportional AI-MD
// ============================================================

int64_t RtpSctpCoordinator::ComputeFairnessAimdRate(
    int64_t pacing_rate_bps, double smoothed_bur) {
  const double gamma = config_.gamma_md;
  const double alpha_target = config_.alpha_target;
  double max_step_frac = config_.max_step_fraction;

  // P = old pacing rate (not recv_rate). Using recv_rate caused a
  // downward spiral: recv lags pacing by 200ms → AI-MD entry resets
  // rate to recv (lower) → throughput drops → recv drops → next AI-MD
  // entry resets even lower. On a stable 300 Mbps link this drove
  // pacing from 420 → 170 Mbps. Using old_rate means AI-MD adjusts
  // from current rate — no jump-down on entry.
  double P = std::max(static_cast<double>(pacing_rate_bps), 1e5);

  // Step reduction near capacity: prevent overshoot when rate >= 90% of capacity
  if (capacity_est_bps_ > 0) {
    double cap_ratio = P / static_cast<double>(capacity_est_bps_);
    if (cap_ratio >= 0.9) {
      max_step_frac *= 0.50;
    }
  }

  double A = gamma * (alpha_target - smoothed_bur) * P;
  double max_step = max_step_frac * P;
  double capped_A = std::max(-max_step, std::min(max_step, A));

  int64_t new_rate = static_cast<int64_t>(P + capped_A);

  RTC_LOG(LS_INFO) << "[BUR-COORD] AI-MD VCP:"
                   << " R_tilde=" << smoothed_bur
                   << " alpha_target=" << alpha_target
                   << " deviation=" << (alpha_target - smoothed_bur)
                   << " A=" << (capped_A / 1e6) << "Mbps"
                   << " recv_base=" << (P / 1e6) << "Mbps"
                   << " -> " << (new_rate / 1e6) << "Mbps";

  return new_rate;
}

// ============================================================
// FSEv2 Algorithm 6: Coupled CC for RTP + SCTP
// ============================================================

void RtpSctpCoordinator::FseOnRtpUpdate(double cc_r_bps, int64_t now_us) {
  // Store GCC's natural CC_R — this is the RTP flow's capacity estimate
  // for the FSEv2 S_CR computation (Algorithm 6).
  fse_gcc_estimate_bps_ = cc_r_bps;

  // Check if SCTP is actively sending (recent SACK within threshold).
  // When SCTP is idle, GCC runs freely — no FSE coordination needed.
  constexpr int64_t kSctpIdleThresholdUs = 500'000;  // 500ms
  bool sctp_active = (fse_last_sctp_update_us_ > 0 &&
                      (now_us - fse_last_sctp_update_us_) < kSctpIdleThresholdUs);

  if (sctp_active) {
    // SCTP active: both flows contribute to S_CR, redistribute proportionally
    fse_s_cr_ = cc_r_bps + fse_sctp_cc_r_bps_;
    fse_s_cr_ = std::max(0.0, fse_s_cr_);
    FseRedistribute(now_us);
  } else {
    // SCTP idle: clear override — GCC runs freely
    rtp_fse_override_bps_.store(0, std::memory_order_relaxed);
  }
}

void RtpSctpCoordinator::FseOnSctpUpdate(double cc_cwnd_bytes,
                                          int64_t rtt_us, int64_t now_us) {
  fse_last_sctp_update_us_ = now_us;  // Mark SCTP active

  // Update RTTbase
  if (fse_rtt_base_us_ < 0 || rtt_us < fse_rtt_base_us_) {
    fse_rtt_base_us_ = rtt_us;
  }

  // Convert CWND to rate: CC_R = CWND * 8 / RTT_base (per FSEv2 Algorithm 6).
  // Using RTT_base (minimum RTT) removes queuing delay from the calculation.
  double rtt_base_s = static_cast<double>(fse_rtt_base_us_) / 1e6;
  if (rtt_base_s <= 0) return;
  double sctp_cc_r_bps = (cc_cwnd_bytes * 8.0) / rtt_base_s;
  fse_sctp_cc_r_bps_ = sctp_cc_r_bps;

  // S_CR = GCC_CC_R (RTP) + SCTP_CC_R  (FSEv2 Algorithm 6)
  // Both CC algorithms run independently; FSE combines their estimates.
  fse_s_cr_ = fse_gcc_estimate_bps_ + sctp_cc_r_bps;
  fse_s_cr_ = std::max(0.0, fse_s_cr_);

  FseRedistribute(now_us);
}

void RtpSctpCoordinator::FseRedistribute(int64_t now_us) {
  double p_rtp = config_.fse_p_rtp;
  double p_sctp = config_.fse_p_sctp;
  double s_p = p_rtp + p_sctp;
  if (s_p <= 0 || fse_s_cr_ <= 0) return;

  double rtp_alloc = 0.0;
  double sctp_alloc = 0.0;

  // Algorithm 6: RTP-priority allocation.
  // RTP gets min(DR, S_CR) first; SCTP gets the remainder.
  if (fse_dr_rtp_bps_ > 0) {
    rtp_alloc = std::min(fse_dr_rtp_bps_, fse_s_cr_);
    sctp_alloc = fse_s_cr_ - rtp_alloc;
  } else {
    // No DR configured — fall back to proportional split
    rtp_alloc = fse_s_cr_ * p_rtp / s_p;
    sctp_alloc = fse_s_cr_ * p_sctp / s_p;
  }

  fse_r_rtp_ = std::max(0.0, rtp_alloc);
  fse_r_sctp_ = std::max(0.0, sctp_alloc);

  // Apply RTP: override GCC output to FSE_R
  rtp_fse_override_bps_.store(static_cast<int64_t>(fse_r_rtp_),
                              std::memory_order_relaxed);

  // FSE does NOT clamp SCTP's CWND — SCTP CC runs freely (same as NC).
  // FSE only observes CWND/RTT to estimate capacity (S_CR) and redistributes
  // the RTP share via GCC override. SCTP competes for bandwidth independently.

  // Update atomic rates for pacing/metrics.
  // In FSE mode, pacing is disabled — SCTP CC controls its own rate via CWND.
  pacing_rate_bps_.store(config_.max_rate_bps, std::memory_order_relaxed);
  sctp_allocated_bps_.store(static_cast<int64_t>(fse_r_sctp_),
                            std::memory_order_relaxed);

  // CSV logging — throttle to ~30Hz to avoid I/O overload
  // (FSE callbacks fire on every SCTP SACK, which can be 10K+/sec)
  constexpr int64_t kFseLogIntervalUs = 33'000;  // 33ms
  if (now_us - fse_last_log_us_ >= kFseLogIntervalUs) {
    fse_last_log_us_ = now_us;
    unified_metrics_.rtp_recv_kbps = GetRtpRecvRateBps() / 1000;
    unified_metrics_.total_recv_kbps = GetTotalRecvRateBps() / 1000;
    unified_metrics_.mode = "FSE";
    unified_metrics_.unified_rate_mbps = fse_s_cr_ / 1e6;
    unified_metrics_.rtp_allocated_mbps = fse_r_rtp_ / 1e6;
    unified_metrics_.sctp_allocated_mbps = fse_r_sctp_ / 1e6;
    unified_metrics_.alloc_phase = "FSE";
    unified_metrics_.pacing_rate_mbps = 0;
    LogToCsv(now_us);
  }

  RTC_LOG(LS_VERBOSE) << "[FSEv2] S_CR=" << (fse_s_cr_ / 1e6)
                      << " (GCC=" << (fse_gcc_estimate_bps_ / 1e6)
                      << "+SCTP=" << (fse_sctp_cc_r_bps_ / 1e6) << ")"
                      << " R_rtp=" << (fse_r_rtp_ / 1e6)
                      << " R_sctp=" << (fse_r_sctp_ / 1e6) << "Mbps";
}

// ===== MAFS Flow Scheduling Methods =====

uint32_t RtpSctpCoordinator::RegisterFlow(int stream_id,
                                           const std::string& label,
                                           size_t total_bytes) {
  if (!flow_coordinator_) return 0;
  int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  return flow_coordinator_->RegisterFlow(stream_id, label, total_bytes, now_ms);
}

void RtpSctpCoordinator::OnFlowDataSent(int stream_id, size_t bytes) {
  if (!flow_coordinator_) return;
  int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  flow_coordinator_->OnDataSent(stream_id, bytes, now_ms);
  // Periodically update priorities (update_interval_ms throttling inside UpdatePriorities)
  MaybeUpdateMafsPriorities(now_ms);
}

void RtpSctpCoordinator::OnFlowDataReceived(int stream_id, size_t bytes) {
  if (!flow_coordinator_) return;
  int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  flow_coordinator_->OnDataReceived(stream_id, bytes, now_ms);
}

void RtpSctpCoordinator::OnSctpDataReceived(size_t bytes) {
  int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  sctp_recv_samples_.push_back(
      {now_ms, static_cast<int64_t>(bytes)});
  while (!sctp_recv_samples_.empty() &&
         (now_ms - sctp_recv_samples_.front().timestamp_ms) > GetAckWindowMs()) {
    sctp_recv_samples_.pop_front();
  }
}

bool RtpSctpCoordinator::SetComputeTime(uint32_t flow_id,
                                          double P_hat_ms,
                                          double sigma_P_ms) {
  if (!flow_coordinator_) return false;
  int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  return flow_coordinator_->SetComputeTime(flow_id, P_hat_ms, sigma_P_ms, now_ms);
}

void RtpSctpCoordinator::MarkFlowComplete(uint32_t flow_id) {
  if (!flow_coordinator_) return;
  flow_coordinator_->MarkFlowComplete(flow_id);
  int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  MaybeUpdateMafsPriorities(now_ms);
}

void RtpSctpCoordinator::ForceUpdateMafsPriorities() {
  if (!flow_coordinator_ || !transport_) return;
  int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();

  flow_coordinator_->SetDownlinkRate(
      static_cast<double>(pacing_rate_bps_.load(std::memory_order_relaxed)));

  auto priority_map = flow_coordinator_->ForceUpdatePriorities(now_ms);
  if (priority_map.empty()) return;

  for (const auto& [stream_id, priority] : priority_map) {
    transport_->SetStreamPriority(stream_id, priority);
    fprintf(stderr, "[MAFS] Initial priority: stream=%d priority=%u\n",
            stream_id, priority);
  }
}

void RtpSctpCoordinator::MaybeUpdateMafsPriorities(int64_t now_ms) {
  if (!flow_coordinator_ || !transport_) return;

  // Feed current pacing rate as downlink rate
  flow_coordinator_->SetDownlinkRate(
      static_cast<double>(pacing_rate_bps_.load(std::memory_order_relaxed)));

  auto priority_map = flow_coordinator_->UpdatePriorities(now_ms);
  if (priority_map.empty()) return;

  // Apply priorities to DcSctpTransport
  for (const auto& [stream_id, priority] : priority_map) {
    transport_->SetStreamPriority(stream_id, priority);
    fprintf(stderr, "[MAFS] SetPriority: stream=%d priority=%u\n",
            stream_id, priority);
  }

  fprintf(stderr, "[MAFS] Priorities applied for %zu flows\n",
          priority_map.size());
}

// =============================================================================
// Pudica BUR Measurement (NSDI'24)
// =============================================================================

void RtpSctpCoordinator::OnPudicaPacketFeedback(
    int64_t transport_seq, int64_t send_time_us, int64_t recv_time_us,
    bool is_probe, bool is_frame_last) {
  if (!active_instance_) return;
  auto* self = active_instance_;
  // Frame OWD BUR: works for Pudica and ALL agentrtc modes.
  // Always compute per-frame OWD so it's available as fallback during idle gaps.
  if (self->config_.mode != CoordinatorMode::kPudica &&
      self->config_.mode != CoordinatorMode::kAgentRtc) {
    return;
  }

  double owd_us = static_cast<double>(recv_time_us - send_time_us);

  // Debug: periodic report
  static int64_t pudica_pkt_count = 0;
  static int64_t pudica_frame_count = 0;
  static int64_t pudica_probe_count = 0;
  pudica_pkt_count++;
  if (is_frame_last) pudica_frame_count++;
  if (is_probe) pudica_probe_count++;
  if (pudica_pkt_count % 500 == 0) {
    fprintf(stderr, "[PUDICA-PKT] pkts=%lld frames=%lld probes=%lld "
            "owd=%.3fms d_min=%.3fms rho=%.2f\n",
            (long long)pudica_pkt_count, (long long)pudica_frame_count,
            (long long)pudica_probe_count, owd_us / 1000.0,
            self->pudica_d_min_us_ / 1000.0, pudica_rho_.load());
  }

  // Update D_min (minimum packet OWD over 10-second window, in μs)
  if (owd_us > 0) {
    self->pudica_owd_window_.push_back({recv_time_us, owd_us});
    while (!self->pudica_owd_window_.empty() &&
           (recv_time_us - self->pudica_owd_window_.front().first) >
               kPudicaDminWindowUs) {
      self->pudica_owd_window_.pop_front();
    }
    self->pudica_d_min_us_ = owd_us;
    for (const auto& [t, d] : self->pudica_owd_window_) {
      if (d < self->pudica_d_min_us_) self->pudica_d_min_us_ = d;
    }
  }

  if (is_probe) {
    double h_us = (self->pudica_last_frame_recv_us_ > 0)
                      ? static_cast<double>(recv_time_us -
                                            self->pudica_last_frame_recv_us_)
                      : owd_us;
    self->pudica_probe_results_.push_back({owd_us, h_us});
  } else {
    // Media packet: track per-frame OWD
    // IMPORTANT: is_frame_last means THIS packet is the first of a NEW frame.
    // Compute BUR for the previous frame BEFORE adding this packet.
    if (is_frame_last && self->pudica_frame_.frame_packets > 0) {
      double bur = self->PudicaComputeFrameBur(recv_time_us);
      if (bur >= 0.0) {
        // Apollo: drive the RTP-video target from this frame's BUR (Pudica
        // rate controller). No-op unless coordinator_mode=pudica.
        self->PudicaUpdateRtpTarget(bur, recv_time_us);
        // Push pure frame BUR to rtp_excess_queue_ (always)
        {
          std::lock_guard<std::mutex> lock(self->rtp_queue_mutex_);
          self->rtp_excess_queue_.push_back(bur * self->features_.L_ms);
        }

        // SCTP correction from agnostic-period SACKs (Pudica Eq.4-5 adaptation)
        if (self->features_.sctp_bur &&
            (self->features_.sctp_bur_mode == SctpBurMode::kRttAdditive ||
             self->features_.sctp_bur_mode == SctpBurMode::kRttMinFrameHi)) {
          double sctp_correction_ms = 0.0;
          int64_t rtt_min = self->rtt_min_us_.load(std::memory_order_relaxed);
          int64_t rtt_ref = self->rtt_ref_us_.load(std::memory_order_relaxed);
          if (rtt_min > 0) {
            double L_us = self->features_.L_ms * 1000.0;
            // Drain ALL queued SACKs (accumulated since last frame boundary)
            std::deque<SctpRttSample> sack_batch;
            {
              std::lock_guard<std::mutex> lock(self->sctp_queue_mutex_);
              sack_batch.swap(self->sctp_rtt_queue_);
            }
            int N_sack = static_cast<int>(sack_batch.size());
            if (N_sack > 0) {
              // Baseline selection:
              // kRttAdditive: RTT_ref (relative, measures queue change since frame)
              // kRttMinFrameHi: RTT_min (absolute, measures total queue depth)
              bool use_min_hi = (self->features_.sctp_bur_mode ==
                                 SctpBurMode::kRttMinFrameHi);
              int64_t baseline = use_min_hi ? rtt_min
                                            : ((rtt_ref > 0) ? rtt_ref : rtt_min);

              // SCTP_SUBSAMPLE: optionally pick N evenly spaced SACKs (0=all)
              static const int kSubsample = []() {
                const char* e = std::getenv("SCTP_SUBSAMPLE");
                return e ? std::atoi(e) : 0;
              }();

              // Select SACKs: subsample or use all
              std::deque<SctpRttSample>* batch_ptr = &sack_batch;
              std::deque<SctpRttSample> subsampled;
              if (kSubsample > 0 && kSubsample < N_sack) {
                for (int k = 0; k < kSubsample; k++) {
                  int idx = k * (N_sack - 1) / (kSubsample - 1);
                  subsampled.push_back(sack_batch[idx]);
                }
                batch_ptr = &subsampled;
              }
              int N_used = static_cast<int>(batch_ptr->size());

              // Aggregation method selector (SACK_AGG env var):
              //   mean (default)    : mean(excess) / L
              //   max               : max(excess) / L
              //   p90               : 90th percentile excess / L
              //   last_k            : mean of last K=16 SACKs
              //   total_mean_rtt    : (mean(RTT) - RTT_min) / L - bur_rtp  (override)
              //   total_max_rtt     : (max(RTT) - RTT_min) / L - bur_rtp   (override)
              //   diff              : (mean(RTT_n) - mean(RTT_n-1)) / L  flow-private
              //   ewma              : (mean(RTT) - slow_ewma(mean_RTT)) / L flow-private
              static const char* agg = []() {
                const char* e = std::getenv("SACK_AGG");
                return e ? e : "mean";
              }();
              // EWMA alpha for slow baseline (small = slow tracking)
              static const double kEwmaAlpha = []() {
                const char* e = std::getenv("SACK_EWMA_ALPHA");
                return e ? std::atof(e) : 0.02;
              }();

              // Sort excesses for percentile
              std::vector<double> excesses;
              excesses.reserve(N_used);
              int64_t max_rtt = 0, sum_rtt = 0;
              int64_t min_rtt = INT64_MAX;
              for (const auto& s : *batch_ptr) {
                double excess_us = static_cast<double>(s.rtt_us - baseline);
                if (excess_us < 0.0) excess_us = 0.0;
                excesses.push_back(excess_us);
                max_rtt = std::max(max_rtt, s.rtt_us);
                min_rtt = std::min(min_rtt, s.rtt_us);
                sum_rtt += s.rtt_us;
              }

              double chosen_excess_us = 0.0;
              std::string agg_mode(agg);
              if (agg_mode == "util") {
                // True bandwidth utilization ratio: pacing / max_recent_delivery.
                // max_delivery is the BBR-style 5s peak — link-capacity proxy.
                // bur_sctp = pacing/max_delivery - 1 represents over-pacing fraction.
                // Existing controller treats bur > 0.5 as backoff signal, so this
                // gives a meaningful overshoot signal.
                int64_t pacing_bps = self->pacing_rate_bps_.load(std::memory_order_relaxed);
                int64_t max_dr = self->max_delivery_rate_bps_.load(std::memory_order_relaxed);
                double op = 0.0;
                if (max_dr > 0 && pacing_bps > max_dr) {
                  op = static_cast<double>(pacing_bps) / max_dr - 1.0;
                }
                chosen_excess_us = op * L_us;
              } else if (agg_mode == "dr") {
                // Delivery-rate matching: bur_sctp = max(0, (pacing - recv) / pacing).
                int64_t pacing_bps = self->pacing_rate_bps_.load(std::memory_order_relaxed);
                int64_t recv_bps = self->GetTotalRecvRateBps();
                double dr_excess = 0.0;
                if (pacing_bps > 0 && recv_bps > 0) {
                  double diff = static_cast<double>(pacing_bps - recv_bps);
                  if (diff > 0) {
                    dr_excess = diff / static_cast<double>(pacing_bps);
                  }
                }
                chosen_excess_us = dr_excess * L_us;
              } else if (agg_mode == "max") {
                chosen_excess_us = *std::max_element(excesses.begin(), excesses.end());
              } else if (agg_mode == "p90") {
                std::sort(excesses.begin(), excesses.end());
                size_t idx = static_cast<size_t>(excesses.size() * 0.9);
                if (idx >= excesses.size()) idx = excesses.size() - 1;
                chosen_excess_us = excesses[idx];
              } else if (agg_mode == "last_k") {
                int k = std::min(16, N_used);
                double sum = 0;
                for (int i = N_used - k; i < N_used; i++) sum += excesses[i];
                chosen_excess_us = sum / k;
              } else if (agg_mode == "total_mean_rtt") {
                // Override: total BUR from mean(RTT) - RTT_min directly
                double avg_rtt = static_cast<double>(sum_rtt) / N_used;
                double queue_us = avg_rtt - static_cast<double>(rtt_min);
                if (queue_us < 0) queue_us = 0;
                double total_bur = queue_us / L_us;
                // Subtract bur_rtp to get sctp correction (since combined = bur_rtp + bur_sctp)
                double sctp_bur = total_bur - bur;
                if (sctp_bur < 0) sctp_bur = 0;
                chosen_excess_us = sctp_bur * L_us;
              } else if (agg_mode == "total_max_rtt") {
                double queue_us = static_cast<double>(max_rtt - rtt_min);
                if (queue_us < 0) queue_us = 0;
                double total_bur = queue_us / L_us;
                double sctp_bur = total_bur - bur;
                if (sctp_bur < 0) sctp_bur = 0;
                chosen_excess_us = sctp_bur * L_us;
              } else if (agg_mode == "anchor") {
                // Frame-boundary RTT anchors. Use mean of LAST K SACKs as
                // "RTT at end of this frame interval". Compare to previous
                // frame's anchor → queue change during this frame interval.
                // Divide by 2 to get one-way queue (RTT is round-trip).
                // Standing queue cancels because both anchors include it.
                int k = std::min(8, N_used);
                int64_t sum_late = 0;
                int start_i = N_used - k;
                int idx = 0;
                for (const auto& s : *batch_ptr) {
                  if (idx >= start_i) sum_late += s.rtt_us;
                  idx++;
                }
                double cur_anchor = static_cast<double>(sum_late) / k;
                double prev = self->prev_frame_mean_rtt_us_;
                double delta_us = (prev > 0) ? (cur_anchor - prev) / 2.0 : 0.0;
                if (delta_us < 0.0) delta_us = 0.0;
                chosen_excess_us = delta_us;
                self->prev_frame_mean_rtt_us_ = cur_anchor;
              } else if (agg_mode == "diff") {
                // Flow-private: differential of mean RTT vs previous frame.
                double cur_mean = static_cast<double>(sum_rtt) / N_used;
                double prev = self->prev_frame_mean_rtt_us_;
                double delta_us = (prev > 0) ? (cur_mean - prev) : 0.0;
                if (delta_us < 0.0) delta_us = 0.0;
                chosen_excess_us = delta_us;
                self->prev_frame_mean_rtt_us_ = cur_mean;
              } else if (agg_mode == "ewma") {
                // Flow-private: slow EWMA tracks standing queue baseline.
                // Excess above baseline = transient queue from current frame.
                double cur_mean = static_cast<double>(sum_rtt) / N_used;
                double prev_ewma = self->slow_ewma_mean_rtt_us_;
                if (prev_ewma < 0) {
                  self->slow_ewma_mean_rtt_us_ = cur_mean;
                  chosen_excess_us = 0.0;
                } else {
                  double delta_us = cur_mean - prev_ewma;
                  if (delta_us < 0.0) delta_us = 0.0;
                  chosen_excess_us = delta_us;
                  self->slow_ewma_mean_rtt_us_ =
                      (1.0 - kEwmaAlpha) * prev_ewma + kEwmaAlpha * cur_mean;
                }
              } else if (agg_mode == "sum_gap") {
                // Σ min(excess, gap) / Σ gap — time-weighted queue ratio
                double sum_capped = 0.0;
                double sg = 0.0;
                for (const auto& s : *batch_ptr) {
                  double excess_val = static_cast<double>(s.rtt_us - baseline);
                  if (excess_val < 0.0) excess_val = 0.0;
                  double gap = static_cast<double>(s.gap_us);
                  if (gap < 1.0) gap = 1.0;
                  sum_capped += std::min(excess_val, gap);
                  sg += gap;
                }
                // Result in same units as other agg modes: excess_us that gives bur = ratio
                chosen_excess_us = (sg > 0) ? (sum_capped / sg) * L_us : 0.0;
              } else {
                // mean (default)
                double sum = 0;
                for (double e : excesses) sum += e;
                chosen_excess_us = sum / N_used;
              }
              sctp_correction_ms = chosen_excess_us / 1000.0;

              // Debug: log per-frame stats (1Hz)
              static int64_t last_dbg = 0;
              if (recv_time_us - last_dbg > 1'000'000) {
                int64_t avg_rtt = sum_rtt / N_used;
                double true_queue = static_cast<double>(avg_rtt - rtt_min) / 1000.0;
                fprintf(stderr, "[AGG_DBG] mode=%s N=%d "
                        "rtt[min=%lld,avg=%lld,max=%lld] rtt_min_abs=%lld "
                        "true_queue=%.1fms corr=%.2fms bur_sctp=%.3f total_bur=%.3f\n",
                        agg_mode.c_str(), N_used,
                        (long long)min_rtt, (long long)avg_rtt, (long long)max_rtt,
                        (long long)rtt_min, true_queue,
                        sctp_correction_ms,
                        sctp_correction_ms / self->features_.L_ms,
                        bur + sctp_correction_ms / self->features_.L_ms);
                last_dbg = recv_time_us;
              }
            }
          }
          std::lock_guard<std::mutex> lock(self->rtp_queue_mutex_);
          self->sctp_excess_queue_.push_back(sctp_correction_ms);
        }
      }

      // Update adaptive pacing: ρ = γ_ρ / min(R, 1) (use base frame BUR, not corrected)
      double r_clamped = std::max(0.01, std::min(bur, 1.0));
      double rho = self->features_.pudica_gamma_rho / r_clamped;
      rho = std::max(1.0, std::min(rho, 10.0));
      pudica_rho_.store(rho, std::memory_order_relaxed);
      PacingController::SetPudicaRho(rho);

      // Set RTT_ref synthetically from frame measurement.
      // RTT_ref = RTT_min + (D_frame - D_min)
      //         = "RTT of an SCTP packet that traversed the same queue as the frame"
      // This is conceptually the true reference: the SCTP correction then measures
      // ONLY the queue accumulated AFTER the frame ended (inter-frame increment).
      {
        int64_t rtt_min = self->rtt_min_us_.load(std::memory_order_relaxed);
        if (rtt_min > 0 && self->pudica_d_min_us_ > 0 &&
            self->pudica_frame_.last_recv_us > 0) {
          double D_us = static_cast<double>(self->pudica_frame_.last_recv_us -
                                            self->pudica_frame_.first_send_us);
          double frame_queue_us = D_us - self->pudica_d_min_us_;
          if (frame_queue_us < 0.0) frame_queue_us = 0.0;
          int64_t synthetic_rtt_ref = rtt_min + static_cast<int64_t>(frame_queue_us);
          self->rtt_ref_us_.store(synthetic_rtt_ref, std::memory_order_relaxed);
        }
      }

      // Trigger SCTP burst probe at frame boundary (sender-side clock)
      if (self->features_.sctp_burst_probe) {
        auto now_sender = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        self->frame_burst_start_us_.store(now_sender, std::memory_order_relaxed);
      }

      // SCTP jitter: random ±jitter multiplier per frame
      if (self->features_.sctp_jitter > 0.0) {
        static thread_local uint32_t jrng = 54321;
        jrng = jrng * 1103515245 + 12345;
        double u = static_cast<double>((jrng >> 8) & 0xFFFF) / 65535.0;  // [0, 1]
        double mult = 1.0 + self->features_.sctp_jitter * (2.0 * u - 1.0);
        self->frame_jitter_mult_.store(mult, std::memory_order_relaxed);
      }

      // Reset for next frame — save frame boundary references first
      self->pudica_last_frame_recv_us_ = self->pudica_frame_.last_recv_us;
      self->pudica_last_frame_send_us_ = self->pudica_frame_.last_send_us;
      self->pudica_frame_ = {};
      self->pudica_probe_results_.clear();
    }

    // Now add current packet to the (new) frame
    if (self->pudica_frame_.first_send_us < 0 ||
        send_time_us < self->pudica_frame_.first_send_us) {
      self->pudica_frame_.first_send_us = send_time_us;
    }
    if (send_time_us > self->pudica_frame_.last_send_us) {
      self->pudica_frame_.last_send_us = send_time_us;
    }
    self->pudica_frame_.last_recv_us = recv_time_us;
    self->pudica_frame_.frame_packets++;
  }
}

double RtpSctpCoordinator::PudicaComputeFrameBur(int64_t now_us) {
  if (pudica_frame_.first_send_us < 0 || pudica_frame_.last_recv_us < 0 ||
      pudica_d_min_us_ < 0) {
    return -1.0;
  }

  double L_us = features_.L_ms * 1000.0;  // convert L from ms to μs
  if (L_us <= 0) return -1.0;

  // D = frame OWD = last_recv - first_send (μs)
  double D_us = static_cast<double>(pudica_frame_.last_recv_us -
                                     pudica_frame_.first_send_us);

  // R = (D - D_min) / L (Pudica Eq.1) — all in μs, result dimensionless
  double base_R = std::max(0.0, (D_us - pudica_d_min_us_) / L_us);
  double R = base_R;

  // Probe correction: R_corrected = R + Σ T_i / L (Pudica Eq.5)
  int probe_count = static_cast<int>(pudica_probe_results_.size());
  double probe_correction_us = 0.0;
  if (!pudica_probe_results_.empty()) {
    double rho = pudica_rho_.load(std::memory_order_relaxed);
    double T_packet_us = (1.0 - 1.0 / rho) * L_us /
                         (features_.pudica_num_probes + 1);
    if (T_packet_us < 100.0) T_packet_us = 100.0;  // min 0.1ms

    for (const auto& probe : pudica_probe_results_) {
      double probe_excess = probe.owd_us - pudica_d_min_us_;
      if (probe_excess < 0.0) probe_excess = 0.0;
      // T_i = min(D_i - D_min, H_i, T_packet) (Pudica Eq.4)
      double T_i = std::min({probe_excess, probe.h_us, T_packet_us});
      if (T_i < 0.0) T_i = 0.0;
      probe_correction_us += T_i;
    }
    R += probe_correction_us / L_us;
  }

  // Store for smoothed BUR (window in μs)
  int64_t rate_bps = pacing_rate_bps_.load(std::memory_order_relaxed);
  pudica_bur_history_.push_back({now_us, R, static_cast<double>(rate_bps)});
  while (!pudica_bur_history_.empty() &&
         (now_us - pudica_bur_history_.front().time_us) > kPudicaBurWindowUs) {
    pudica_bur_history_.pop_front();
  }

  // Pudica debug CSV (output in ms for readability, with decimal precision)
  double rho_val = pudica_rho_.load(std::memory_order_relaxed);
  if (!pudica_csv_initialized_) {
    const char* dir = std::getenv("UNIFIED_CSV_DIR");
    if (dir && std::strlen(dir) > 0) {
      std::string path = std::string(dir) + "/pudica_debug.csv";
      pudica_csv_file_.open(path, std::ios::out | std::ios::trunc);
      if (pudica_csv_file_.is_open()) {
        chmod(path.c_str(), 0666);
        pudica_csv_file_ << "time_ms,frame_owd_ms,d_min_ms,base_bur,"
                            "probe_count,probe_correction_ms,corrected_bur,"
                            "rho,frame_packets\n";
      }
      pudica_csv_start_us_ = now_us;
      pudica_csv_initialized_ = true;
    }
  }
  if (pudica_csv_file_.is_open()) {
    pudica_csv_file_ << std::fixed << std::setprecision(3)
                     << ((now_us - pudica_csv_start_us_) / 1000.0) << ","
                     << (D_us / 1000.0) << "," << (pudica_d_min_us_ / 1000.0) << ","
                     << std::setprecision(6) << base_R << "," << probe_count << ","
                     << std::setprecision(3) << (probe_correction_us / 1000.0) << ","
                     << std::setprecision(6) << R << ","
                     << std::setprecision(2) << rho_val << ","
                     << pudica_frame_.frame_packets << "\n";
  }

  return R;
}

double RtpSctpCoordinator::PudicaSmoothedBur(int64_t now_us) {
  // Weighted average BUR over 200ms (Pudica Eq.6)
  if (pudica_bur_history_.empty()) return 0.0;

  double current_rate = static_cast<double>(
      pacing_rate_bps_.load(std::memory_order_relaxed));
  if (current_rate <= 0) current_rate = 1.0;

  double sum_bur = 0.0;
  double sum_weight = 0.0;
  for (const auto& s : pudica_bur_history_) {
    double age_us = static_cast<double>(now_us - s.time_us);
    double weight = std::max(0.0, 1.0 - age_us / static_cast<double>(kPudicaBurWindowUs));
    double rate_ratio = (s.bitrate_bps > 0) ? current_rate / s.bitrate_bps : 1.0;
    sum_bur += weight * s.bur * rate_ratio;
    sum_weight += weight;
  }

  return (sum_weight > 0) ? sum_bur / sum_weight : 0.0;
}

double RtpSctpCoordinator::GetPudicaPacingMultiplier() {
  return pudica_rho_.load(std::memory_order_relaxed);
}

bool RtpSctpCoordinator::IsPudicaProbingEnabled() {
  if (!active_instance_) return false;
  return active_instance_->config_.mode == CoordinatorMode::kPudica &&
         active_instance_->features_.pudica_probing;
}

// ===== Pudica RTP-video rate controller (Apollo) =====
bool RtpSctpCoordinator::IsPudicaMode() {
  return pudica_mode_active_.load(std::memory_order_relaxed);
}

int64_t RtpSctpCoordinator::GetPudicaRtpOverride() {
  return pudica_rtp_target_bps_.load(std::memory_order_relaxed);
}

// Per-frame Pudica rate rule (NSDI'24): the arrived frame's BUR (= queuing
// delay / frame interval) decides the next RTP-video target relative to the
// measured receiving_rate (delivered RTP bitrate).
//   BUR < 1  → headroom: one-step restore to receiving_rate × γ_up (fast up,
//              unlike GCC's slow AIMD ramp; the signature Pudica behavior).
//   BUR ≈ 1  → hold at receiving_rate.
//   BUR > 1  → queue building: drain below receiving_rate (§4.3 Eq.11-style).
// The result is published via GetPudicaRtpOverride() and applied by
// DelayBasedBwe as the delay-based target. NOTE: still subject to the
// loss-based cap downstream (see "Neutralize loss-based BWE" task) — that
// matters only once burst loss occurs; the loss-light up-step case is exact.
void RtpSctpCoordinator::PudicaUpdateRtpTarget(double frame_bur, int64_t now_us) {
  if (config_.mode != CoordinatorMode::kPudica) return;

  // Measured RTP receiving rate (delivered) — the restore anchor.
  int64_t recv_rate = GetRtpRecvRateBps();
  if (recv_rate <= 0) recv_rate = GetTotalRecvRateBps();
  if (recv_rate <= 0) return;  // No delivery signal yet → leave GCC in charge.

  int64_t old_target = pudica_rtp_target_bps_.load(std::memory_order_relaxed);
  if (old_target <= 0) old_target = recv_rate;

  // Env-tunable thresholds; defaults pivot on the paper's BUR = 1.
  static const double kGammaUp = []() {
    // Restore aggressiveness: target = recv_rate x gamma when BUR<1. 1.05 was
    // too gentle to break out of a low-quality equilibrium and probe into
    // available headroom; 1.3 climbs ~30%/feedback to the link limit in ~1s,
    // then BUR>1 triggers drain (the intended aggressive Pudica sawtooth).
    const char* e = std::getenv("PUDICA_GAMMA_UP"); return e ? std::atof(e) : 1.30; }();
  static const double kUpThresh = []() {
    const char* e = std::getenv("PUDICA_UP_THRESHOLD"); return e ? std::atof(e) : 0.9; }();
  static const double kDrainThresh = []() {
    const char* e = std::getenv("PUDICA_DRAIN_THRESHOLD"); return e ? std::atof(e) : 1.1; }();
  // Controller ceiling (independent of the legacy 5 Mbps rtp_max_rate_bps).
  static const int64_t kPudMaxBps = []() {
    const char* e = std::getenv("PUDICA_MAX_RATE_KBPS");
    return e ? static_cast<int64_t>(std::atoll(e)) * 1000 : static_cast<int64_t>(0); }();

  int64_t new_target = old_target;
  const char* mode = "PUD-HOLD";

  if (frame_bur < kUpThresh) {
    // One-step restore to delivered rate, biased up by γ to probe headroom.
    new_target = static_cast<int64_t>(recv_rate * kGammaUp);
    mode = "PUD-RESTORE";
  } else if (frame_bur > kDrainThresh) {
    // Drain the standing queue within ~200 ms (Eq.11-style).
    double queue_delay_ms = (frame_bur - 1.0) * features_.L_ms;
    if (queue_delay_ms < 0.0) queue_delay_ms = 0.0;
    int64_t queue_bytes =
        static_cast<int64_t>(queue_delay_ms * recv_rate / 8000.0);
    int64_t drain_rate_bps =
        (queue_bytes > 0) ? static_cast<int64_t>(queue_bytes * 8.0 / 0.200) : 0;
    new_target =
        static_cast<int64_t>(config_.draining_target * recv_rate) - drain_rate_bps;
    mode = "PUD-DRAIN";
  } else {
    new_target = recv_rate;
    mode = "PUD-HOLD";
  }

  int64_t ceiling = (kPudMaxBps > 0) ? kPudMaxBps : config_.max_rate_bps;
  new_target = std::max(config_.min_rate_bps, std::min(ceiling, new_target));
  pudica_rtp_target_bps_.store(new_target, std::memory_order_relaxed);

  unified_metrics_.mode = mode;
  unified_metrics_.rtp_allocated_mbps = new_target / 1'000'000.0;

  static int64_t pud_log = 0;
  if (++pud_log % 30 == 0) {
    fprintf(stderr, "[PUDICA-RTP] %s bur=%.3f recv=%.2fMbps target=%.2fMbps\n",
            mode, frame_bur, recv_rate / 1e6, new_target / 1e6);
  }
}

}  // namespace webrtc
