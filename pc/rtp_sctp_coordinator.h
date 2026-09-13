/*
 *  Copyright (c) 2024 The WebRTC project authors. All Rights Reserved.
 *
 *  RTP-SCTP Coordinator for BUR-based pacing control.
 *  V14: OWD-based RTP BUR from GCC accumulated_delay.
 *  - RTP BUR = (accumulated_delay - min_accumulated_delay) / L
 *  - SCTP BUR = max(RTT - RTT_min) / L
 *  - L = configurable normalization constant (BUR_L_MS env var)
 */

#ifndef PC_RTP_SCTP_COORDINATOR_H_
#define PC_RTP_SCTP_COORDINATOR_H_

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <cstring>
#include <string>
#include <fstream>
#include <vector>

#include "api/task_queue/task_queue_base.h"
#include "pc/coordinator/mafs_config.h"
#include "pc/coordinator/multi_agent_flow_coordinator.h"
#include "pc/coordinator/pudica_rtp_rate.h"
#include "rtc_base/logging.h"
#include "rtc_base/thread.h"
#include "rtc_base/thread_annotations.h"

namespace rtc {
class Thread;
}

namespace webrtc {

// Forward declarations
class DcSctpTransport;

// Coordinator mode
enum class CoordinatorMode {
  kDisabled,   // No pacing - send immediately
  kAgentRtc,   // BUR-based dynamic pacing
  kFse,        // FSEv2 coupled CC (priority-proportional rate redistribution)
  kPudica,     // Pudica NSDI'24: RTP-only BUR + paper CC. SCTP combiner unused.
  kFseV2       // Lightweight: NC-mode SCTP CC + 1Hz fixed RTP override (test isolation)
};

// SCTP BUR measurement mode (BUR_SCTP_MODE env var)
enum class SctpBurMode {
  kRttRef,        // avg(RTT - RTT_ref) / L — original per-frame-ref delta
  kRttMin,        // median(RTT - RTT_min) / L — absolute queue depth
  kRttAdditive,   // Pudica Eq.4-5: additive correction to frame BUR (RTT_ref baseline)
  kRttMinFrameHi, // RTT_min baseline + batch-relative H_i (Pudica-style anti-double-counting)
  kRttMinAll      // RTT_min baseline + Σ min(excess, gap) / L — all SACKs, SCTP-only
};

// BUR combine mode for multi-signal fusion
enum class BurCombineMode {
  kSctp,         // SCTP BUR only (v12 baseline)
  kRtp,          // RTP BUR only (accumulated_delay)
  kMax,          // max(rtp, sctp)
  kWeightedAvg,  // weighted average
  kAdditive,     // bur_rtp + bur_sctp (combined signal)
  kFallback,     // bur_sctp when SACK, bur_rtp when no SACK
  kDecay         // bur_sctp when SACK, halve rate every 200ms toward init when no SACK
};

// Feature toggles (environment variable driven)
struct CoordinatorFeatures {
  bool twcc_bur = false;           // TWCC-based RTP BUR estimation
  bool sctp_bur = true;            // SACK-based SCTP BUR (existing)
  SctpBurMode sctp_bur_mode = SctpBurMode::kRttRef;  // SCTP BUR formula
  BurCombineMode combine = BurCombineMode::kSctp;
  bool rtp_recv_rate = false;      // TWCC ACK bytes → RTP recv rate
  bool total_recv_rate = false;    // recv_rate = sctp_recv + rtp_recv
  double combine_weight = 0.5;     // weight for kWeightedAvg (rtp weight)
  double L_ms = 33.0;              // BUR normalization constant (BUR_L_MS)
  bool alloc_dynamic = true;        // Dynamic RTP allocation (BUR_ALLOC_DYNAMIC)
  bool unified_rtp_rate_ctrl = false; // Feed rtp_floor to GCC (UNIFIED_RTP_RATE_CTRL)
  double max_rtp_share = 0.5;        // Max RTP share at high BW (UNIFIED_MAX_RTP_SHARE)

  // Pudica-specific (NSDI'24)
  bool pudica_probing = false;       // Send probe padding packets between frames
  bool sctp_pacing_bypass = false;   // SCTP_PACING_BYPASS: skip pacing → bursty SCTP
  bool measure_only = false;         // BUR_MEASURE_ONLY: measure BUR but skip rate control
  int pudica_num_probes = 4;         // Number of probe packets per frame gap
  double pudica_gamma_rho = 1.25;    // Adaptive pacing multiplier γ_ρ

  // SCTP frame-burst probing: burst SCTP at frame boundary for BUR measurement
  bool sctp_burst_probe = false;     // SCTP_BURST_PROBE: frame-sync burst pacing
  double sctp_burst_mult = 2.0;     // SCTP_BURST_MULT: burst rate multiplier
  int64_t sctp_burst_us = 5000;     // SCTP_BURST_MS: burst duration in μs (default 5ms)

  // SCTP jitter probing: random ±jitter per frame for BUR measurement
  double sctp_jitter = 0.0;         // SCTP_JITTER: jitter amplitude (0.2 = ±20%)
};

// Coordinator configuration
struct CoordinatorConfig {
  CoordinatorMode mode = CoordinatorMode::kDisabled;

  // Initial pacing rate in bps
  int64_t initial_rate_bps = 100'000'000;  // 100 Mbps

  // Static pacing rate (if > 0, overrides dynamic)
  int64_t static_rate_bps = 0;

  // BUR parameters
  double alpha = 0.85;
  double bur_threshold = 0.5;        // AI-MD threshold
  double spike_threshold = 2.0;      // RAW-CORR/DRAINING trigger (BUR > threshold)
  double max_mi_multiplier = 1.2;    // Max multiplier for PROBE mode only
  double spike_reduction = 0.93;     // Gentle SPIKE reduction
  double draining_target = 0.85;     // Draining: target × recv_rate - drain_rate

  // AI-MD Fairness parameters (VCP-style BUR-proportional)
  double gamma_md = 0.20;
  double gamma_mi = 0.12;
  double fairness_upper_threshold = 1.0;
  double draining_bur_threshold = 1.0;
  double gamma_spike = 0.30;
  double raw_corr_max_step = 0.03;
  double alpha_target = 0.9;
  double max_step_fraction = 0.08;
  double ewma_alpha = 0.30;
  double recv_cap_margin = 1.20;  // Default recv-rate cap margin for low BUR
  double ai_recv_alpha = 0.0;     // AI step = alpha × recv_max_10s (0 = legacy TCP AI)
  double raw_corr_factor = 0.85;  // RAW-CORR: new_rate = old_rate × factor
  bool skip_draining = false;     // If true, skip Pudica DRAINING; every BUR>spike → RAW-CORR only

  int64_t interval_ms = 33;
  int64_t timeout_ms = 100;
  int64_t rate_decision_interval_ms = 100;
  int64_t max_rate_bps = 1'000'000'000;
  int64_t min_rate_bps = 1'000'000;
  int64_t rtp_max_rate_bps = 5'000'000;  // RTP max bitrate (RTP_MAX_RATE_KBPS)

  // FSEv2 parameters
  double fse_p_rtp = 2.0;          // P(rtp): priority weight (RTP > SCTP)
  double fse_p_sctp = 1.0;         // P(sctp): priority weight
  int64_t fse_dr_rtp_kbps = 10000; // DR(rtp): desired max RTP rate (kbps)

  static CoordinatorConfig FromEnvironment();
};

// Unified metrics for CSV logging
struct UnifiedMetrics {
  int64_t sctp_kbps = 0;
  int64_t pacing_rate_mbps = 0;
  double current_bur = 0.0;
  double smoothed_bur = 0.0;
  int64_t rtt_min_ms = 0;
  int64_t rtt_max_ms = 0;
  int64_t L_ms = 0;
  int64_t available_bw_kbps = 0;
  int64_t cwnd_bytes = 0;
  int64_t srtt_ms = 0;
  int64_t peer_rwnd_bytes = 0;      // Receiver window from peer SACK
  int64_t unacked_bytes = 0;        // In-flight bytes (outstanding)
  int64_t receiving_rate_kbps = 0;

  // V14 BUR breakdown
  double bur_rtp = 0.0;              // OWD-based RTP BUR
  double bur_sctp = 0.0;             // SACK-based SCTP BUR
  double bur_combined = 0.0;         // Combined BUR (used for rate decisions)
  double excess_rtp_ms = 0.0;        // accumulated_delay - min (raw queue depth ms)
  double excess_sctp_ms = 0.0;       // max(rtt - rtt_min) in ms
  int64_t rtp_recv_kbps = 0;
  int64_t total_recv_kbps = 0;
  std::string combine_mode = "sctp";

  // Rate allocation diagnostics
  double unified_rate_mbps = 0.0;
  double rtp_allocated_mbps = 0.0;
  double sctp_allocated_mbps = 0.0;
  std::string alloc_phase = "WAIT";

  // Rate control diagnostics
  std::string mode = "INIT";
  double rate_delta_mbps = 0.0;
  double recv_rate_mbps = 0.0;
  int consecutive_high = 0;

  // Internal
  int64_t sctp_bytes_accumulated = 0;
  int64_t last_log_time_ms = 0;
};

// RTP-SCTP Coordinator: Controls SCTP pacing based on BUR
// V14: OWD-based RTP BUR from accumulated_delay + SCTP RTT queue
class RtpSctpCoordinator {
  friend class PudicaFeedbackTest;
 public:
  explicit RtpSctpCoordinator(
      rtc::Thread* network_thread,
      const CoordinatorConfig& config = CoordinatorConfig::FromEnvironment());
  ~RtpSctpCoordinator();

  void SetDcSctpTransport(DcSctpTransport* transport);

  CoordinatorMode GetMode() const { return config_.mode; }
  bool IsDisabledMode() const {
    return config_.mode == CoordinatorMode::kDisabled;
  }

  // ===== Static RTP floor for GCC (UNIFIED_RTP_RATE_CTRL) =====
  static int64_t GetRtpTargetFloor();

  // ===== FSEv2: GCC rate override =====
  // Called from delay_based_bwe when FSE mode is active.
  // Reports GCC's natural CC_R to FSE and returns FSE_R override (>0 if active).
  static void OnGccRateUpdated(int64_t cc_r_bps);
  static int64_t GetRtpFseOverride();
  static bool IsFseMode();
  // Returns true when coordinator_mode=fse_v2 (lightweight: NC-mode SCTP CC + 1Hz fixed RTP override).
  static bool IsFseV2Mode();

  // ===== Static callbacks (called from GCC modules via singleton) =====

  // Called per TWCC group from TrendlineEstimator::UpdateTrendline()
  // V14: receives accumulated_delay (absolute OWD proxy) instead of deltas
  static void OnTwccUpdate(double accumulated_delay_ms,
                           int64_t arrival_time_ms);

  // Called per TWCC feedback message from DelayBasedBwe
  // This is the interval completion trigger (~50-100ms)
  static void OnTwccFeedbackComplete(int64_t rtp_bytes_acked,
                                      int64_t feedback_time_ms,
                                      int64_t max_data_rate_bps = -1);

  // ===== Pudica: per-packet OWD feedback (called from DelayBasedBwe) =====
  // `rtp_timestamp` is the on-wire timestamp of the frame this packet belongs
  // to; it delimits the BUR accumulator. `is_frame_last` is the transport
  // send-time group boundary, used only when rtp_timestamp is unavailable.
  static void OnPudicaPacketFeedback(int64_t transport_seq,
                                      int64_t send_time_us,
                                      int64_t recv_time_us,
                                      bool is_probe,
                                      bool is_frame_last,
                                      uint32_t rtp_timestamp = 0,
                                      int64_t size_bytes = 0,
                                      int64_t inflight_bytes = 0,
                                      int64_t intended_span_us = 0,
                                      int64_t probe_interval_us = 0,
                                      uint64_t pudica_send_id = 0,
                                      int64_t pudica_send_time_us = 0);

  // Pudica: adaptive pacing multiplier (read by PacingController)
  static double GetPudicaPacingMultiplier();
  static bool IsPudicaProbingEnabled();

  // ===== Pudica RTP-video rate override (Apollo) =====
  // Mirrors the FSE override: delay_based_bwe applies GetPudicaRtpOverride()
  // as the RTP delay-based target when IsPudicaMode() is true. The override is
  // recomputed per frame from the frame BUR + measured receiving_rate.
  static bool IsPudicaMode();
  static int64_t GetPudicaRtpOverride();
  // [A28e] Timer-path variant: returns >0 only when the next-delay fallback
  // actually cut the target, so the periodic path stays inert otherwise.
  static int64_t GetPudicaTimerFallback();

  // ===== Called by DcSctpTransport =====

  void OnChunkSent(uint32_t tsn, int64_t bytes, int64_t now_us);

  void OnSackReceived(uint32_t cumulative_tsn_ack,
                      int64_t rtt_us,
                      int64_t bytes_acked,
                      bool has_packet_loss,
                      int64_t now_us);

  void CheckTimeouts(int64_t now_us);

  int64_t GetPacingRate() const;
  int64_t GetRttMinUs() const {
    return rtt_min_us_.load(std::memory_order_relaxed);
  }
  int64_t GetTotalRecvRateBps() const;

  // Metrics setters (from dcSCTP socket)
  void SetCwnd(int64_t cwnd_bytes);
  void SetSrtt(int64_t srtt_ms);
  void SetPeerRwnd(int64_t peer_rwnd_bytes);
  void SetUnackedBytes(int64_t unacked_bytes);
  void SetAvailableBandwidth(int64_t bw_kbps);

  const CoordinatorConfig& config() const { return config_; }
  double GetCurrentBur() const { return last_bur_; }
  UnifiedMetrics& GetUnifiedMetrics() { return unified_metrics_; }

  // ===== MAFS Flow Scheduling API =====
  static RtpSctpCoordinator* GetActiveInstance() { return active_instance_; }
  uint32_t RegisterFlow(int stream_id, const std::string& label, size_t total_bytes);
  void OnFlowDataSent(int stream_id, size_t bytes);
  void OnFlowDataReceived(int stream_id, size_t bytes);
  void OnSctpDataReceived(size_t bytes);
  bool SetComputeTime(uint32_t flow_id, double P_hat_ms, double sigma_P_ms);
  void MarkFlowComplete(uint32_t flow_id);
  bool IsMafsEnabled() const { return mafs_config_.enabled; }
  void ForceUpdateMafsPriorities();

 private:
  // Compute BUR and make rate decision (called from GCC thread)
  void ComputeBurAndDecide(int64_t now_ms);

  // Combine RTP and SCTP BUR
  double CombineBur(double bur_rtp, double bur_sctp);

  // Congestion detected (timeout)
  void OnCongestionTimeout(int64_t now_us);

  // Adjust pacing rate based on BUR value
  void AdjustPacingRate(double bur, const char* trigger, int64_t now_us);

  // Allocate unified rate between RTP and SCTP
  void AllocateUnifiedRate();

  // SCTP ACK-based receiving rate
  int64_t GetAckRateBps() const;
  int64_t GetDirectSctpRecvRateBps() const;

  // RTP ACK-based receiving rate
  int64_t GetRtpRecvRateBps() const;

  // CSV
  void InitCsvLogging();
  void LogToCsv(int64_t now_us);

  // AI-MD Fairness rate computation
  int64_t ComputeFairnessAimdRate(int64_t pacing_rate_bps,
                                   double smoothed_bur);

  // === Singleton for static callbacks ===
  static RtpSctpCoordinator* active_instance_;
  static std::atomic<int64_t> rtp_target_floor_bps_;
  static std::atomic<int64_t> rtp_fse_override_bps_;  // FSE RTP rate override
  static std::atomic<bool> fse_mode_active_;           // FSE mode flag for GCC
  static std::atomic<bool> fse_v2_mode_active_;        // FSEv2 mode flag (lightweight override)
  static std::atomic<int64_t> pudica_rtp_target_bps_;  // Apollo: Pudica RTP target override
  static std::atomic<bool> pudica_mode_active_;        // Apollo: Pudica mode flag for GCC
  // Next delay retains the newest ACK's original sender time for diagnostics;
  // its exact sender-local ID retires frames without timestamp quantization.
  static std::atomic<int64_t> pudica_acked_send_us_;
  static std::atomic<uint64_t> pudica_acked_send_id_;
  // [A28e] True while GetPudicaRtpOverride() is running on the periodic path,
  // so next_delay.csv can say which call site produced each row ("-t" suffix).
  static std::atomic<bool> pudica_in_timer_path_;

  CoordinatorConfig config_;
  CoordinatorFeatures features_;
  DcSctpTransport* transport_ = nullptr;
  std::atomic<int64_t> pacing_rate_bps_{0};     // Unified rate (BUR output)
  std::atomic<int64_t> sctp_allocated_bps_{0};  // SCTP portion after allocation

  // === V14: RTP BUR from accumulated_delay ===
  double min_acc_delay_ms_ = 0.0;        // Minimum accumulated_delay (D_min equiv)
  bool acc_delay_initialized_ = false;

  // RTP excess queue (GCC thread pushes per TWCC group, drained at feedback)
  // Protected by rtp_queue_mutex_ since OnTwccUpdate and ComputeBurAndDecide
  // can run on different threads (GCC thread vs SACK fallback on network thread)
  std::mutex rtp_queue_mutex_;
  std::deque<double> rtp_excess_queue_;  // excess_ms per frame (frame BUR × L)
  std::deque<double> sctp_excess_queue_; // excess_ms per frame (SCTP correction × L, kRttAdditive only)

  // === V14: SCTP RTT queue (network thread pushes, GCC thread drains) ===
  struct SctpRttSample {
    int64_t timestamp_us;
    int64_t rtt_us;
    int64_t gap_us;      // time since previous SACK
    int64_t rtt_ref_us;  // RTT_ref that was active when this SACK arrived
  };
  std::mutex sctp_queue_mutex_;  // Only protects sctp_rtt_queue_
  std::deque<SctpRttSample> sctp_rtt_queue_;
  std::atomic<int64_t> rtt_min_us_{-1};  // 10s sliding-window min RTT (atomic: written by net, read by GCC)
  // Sliding window for rtt_min computation (matches Pudica D_min: 10s)
  struct RttMinSample { int64_t time_us; int64_t rtt_us; };
  std::deque<RttMinSample> rtt_min_window_;  // guarded by sctp_queue_mutex_
  static constexpr int64_t kRttMinWindowUs = 10'000'000;  // 10 seconds

  // Max delivery rate tracking (BBR-style btlbw): peak rate in window.
  // Used as the link-capacity proxy for adaptive floor computation.
  // Default 30s: long enough to retain pre-crossTraffic peak through a
  // 5-10s backoff spiral, short enough to forget stale capacity estimates.
  // Configurable via BUR_BTLBW_WINDOW_S env var.
  std::deque<std::pair<int64_t, int64_t>> max_delivery_window_;  // (time_us, rate_bps)
  std::atomic<int64_t> max_delivery_rate_bps_{0};
  int64_t last_sack_time_us_ = 0;        // For inter-packet gap calculation
  std::atomic<int64_t> last_raw_rtt_us_{0};  // Most recent raw SACK RTT
  std::atomic<int64_t> rtt_ref_us_{-1};  // Per-frame reference RTT (set at frame boundary)
  // SACK history for RTT_ref timestamp matching (recent 200ms)
  struct SackHistoryEntry {
    int64_t send_time_us;  // when the SACK-triggering packet was sent
    int64_t rtt_us;
  };
  std::deque<SackHistoryEntry> sack_history_;  // guarded by sctp_queue_mutex_
  static constexpr int64_t kSackHistoryWindowUs = 200'000;  // 200ms

  // === Interval tracking (simple, no mutex needed) ===
  int64_t interval_start_us_ = 0;
  int64_t interval_pacing_rate_ = 0;   // Rate at interval start (feedback matching)
  std::atomic<int> atomic_sack_count_{0};

  // === Congestion state ===
  int congestion_count_ = 0;
  bool spike_applied_ = false;          // Spike reduction applied once per episode
  bool timeout_was_draining_ = false;   // Draining active during timeout episode
  double last_bur_ = 0.0;
  double smoothed_bur_ = 0.0;
  bool ewma_initialized_ = false;

  // V14 BUR breakdown (for logging)
  double last_bur_rtp_ = 0.0;
  double last_bur_sctp_ = 0.0;
  double last_bur_combined_ = 0.0;
  double last_excess_rtp_ms_ = 0.0;
  double last_excess_sctp_ms_ = 0.0;

  // === Rate decision throttling ===
  int64_t last_rate_decision_us_ = 0;
  int64_t last_completion_us_ = 0;
  int64_t last_decay_us_ = 0;  // kDecay: last rate halving timestamp

  // === Feedback-matched rate control ===
  bool feedback_pending_ = false;
  int64_t rate_change_us_ = 0;
  int64_t rate_after_change_ = 0;

  // === SCTP idle restart (RFC 2581 §4.1 style) ===
  int64_t idle_start_us_ = 0;  // When SCTP idle began (0 = active)

  // === Consecutive high BUR → DRAINING ===
  int consecutive_high_bur_ = 0;
  bool draining_active_ = false;
  int64_t draining_recv_snapshot_ = 0;  // recv_rate at draining onset
  int sustained_high_count_ = 0;  // BUR > threshold for sustained period → halve rate

  // === ACK rate sliding window (SCTP) ===
  // Window = rtt_min (not fixed 200ms). Recv_rate should reflect link
  // throughput over one base RTT, not be diluted by queueing delay.
  int64_t GetAckWindowMs() const {
    int64_t rtt_min = rtt_min_us_.load(std::memory_order_relaxed);
    if (rtt_min > 0) return std::max(int64_t{30}, rtt_min / 1000);
    return 200;  // fallback before first SACK
  }
  static constexpr int64_t kAckWindowMs = 200;  // legacy, use GetAckWindowMs()
  struct AckSample {
    int64_t timestamp_ms;
    int64_t bytes;
  };
  std::deque<AckSample> ack_samples_;
  int64_t ack_samples_total_bytes_ = 0;
  std::deque<AckSample> sctp_recv_samples_;

  // === RTP ACK rate sliding window ===
  std::deque<AckSample> rtp_ack_samples_;
  int64_t rtp_samples_total_bytes_ = 0;

  // === Throughput tracking ===
  int64_t receiving_rate_bps_ = 0;

  // === Capacity estimation ===
  int64_t capacity_est_bps_ = 0;
  int low_bur_count_ = 0;

  // === Passive logging for disabled mode ===
  int64_t passive_log_last_ms_ = 0;

  // === CSV logging ===
  UnifiedMetrics unified_metrics_;
  std::ofstream csv_file_;
  bool csv_initialized_ = false;
  int64_t csv_start_time_ms_ = 0;

  // ===== FSEv2 State =====
  double fse_s_cr_ = 0.0;         // S_CR: aggregate rate (bps)
  double fse_r_rtp_ = 0.0;        // FSE_R for RTP (bps)
  double fse_r_sctp_ = 0.0;       // FSE_R for SCTP (bps)
  double fse_dr_rtp_bps_ = 0.0;   // DR(rtp): desired rate (max video bitrate)
  int64_t fse_rtt_base_us_ = -1;  // RTTbase for CWND conversion
  double fse_sctp_cc_r_bps_ = 0.0; // Latest SCTP CC_R for logging
  double fse_gcc_estimate_bps_ = 0.0; // Latest GCC estimate (= RTP CC_R for S_CR)
  int64_t fse_last_log_us_ = 0;       // Throttle CSV logging
  int64_t fse_last_sctp_update_us_ = 0; // Last SCTP SACK time (for activity detection)
  int64_t fse_last_override_us_ = 0;   // Throttle rtp_fse_override_bps_ store (1Hz default)
  int64_t fse_v2_last_override_us_ = 0; // FSEv2 1Hz override timer

  // FSEv2 methods
  void FseOnRtpUpdate(double cc_r_bps, int64_t now_us);
  void FseOnSctpUpdate(double cc_cwnd_bytes, int64_t rtt_us, int64_t now_us);
  void FseRedistribute(int64_t now_us);

  // ===== Pudica BUR Measurement (NSDI'24) =====
  // Per-frame OWD tracking (all times in microseconds for sub-ms precision)
  struct PudicaFrameInfo {
    int64_t first_send_us = -1;   // first packet send time (μs)
    int64_t last_send_us = -1;    // last packet send time (μs) — for SACK H_i clamping
    int64_t last_recv_us = -1;    // last packet receive time (μs)
    int frame_packets = 0;
    int64_t frame_bytes = 0;      // [A41] acked bytes: the frame's realised B_k
    int64_t intended_span_us = 0;  // Eq.2 arm captured at this frame's send time
  };
  PudicaFrameInfo pudica_frame_;

  // [DRAIN-INFLIGHT] Outstanding bytes reported with the most recent video
  // media feedback. One OWD stale by construction — it is the queue the acked
  // packet actually saw, which is the same instant the frame's BUR describes.
  int64_t pudica_inflight_meas_bytes_ = 0;

  // D_min: minimum packet OWD over 10-second window (μs)
  double pudica_d_min_us_ = -1.0;
  // [A28] §4.3 next delay instrumentation, logged in pudica_ctrl.csv. Written
  // from the feedback thread inside GetPudicaRtpOverride(), read when the row
  // is emitted, so they are atomic. nd_steps is 0 whenever the fallback is not
  // engaged; nd_out_bps holds the last value it published.
  std::atomic<double> pudica_next_delay_ms_{-1.0};
  std::atomic<int> pudica_nd_steps_{0};
  std::atomic<int64_t> pudica_nd_out_bps_{0};
  // Emission span of the last frame (last_send - first_send), subtracted from
  // D before Eq.1. Logged as span_ms in pudica_ctrl.csv: when it approaches or
  // exceeds L the sender, not the network, is what D was measuring.
  double pudica_frame_span_us_ = 0.0;
  // On-wire RTP timestamp of the frame currently accumulating in
  // pudica_frame_. Sentinel: pudica_frame_rtp_ts_valid_ == false means the
  // accumulator has no frame identity and the transport group boundary is
  // being used instead.
  uint32_t pudica_frame_rtp_ts_ = 0;
  bool pudica_frame_rtp_ts_valid_ = false;
  // D_min window. pudica_owd_times_ holds every sample's recv time in arrival
  // order and sets the expiry frontier; pudica_owd_minq_ holds (arrival index,
  // owd_us) with owd increasing front to back, so its front is the minimum.
  std::deque<int64_t> pudica_owd_times_;
  std::deque<std::pair<int64_t, double>> pudica_owd_minq_;
  int64_t pudica_owd_pushed_ = 0;   // samples ever added
  int64_t pudica_owd_expired_ = 0;  // samples ever expired from the front
  static constexpr int64_t kPudicaDminWindowUs = 10'000'000;  // 10 seconds in μs

  // SACK_AGG=diff state: previous frame's mean RTT (μs)
  double prev_frame_mean_rtt_us_ = -1.0;
  // SACK_AGG=ewma state: slow EWMA of mean RTT (μs)
  double slow_ewma_mean_rtt_us_ = -1.0;

  // Probe packet results
  struct PudicaProbeResult {
    double owd_us;    // probe one-way delay (μs)
    double h_us;      // time since last frame packet arrival (μs)
    double interval_us;  // T_packet that scheduled this probe, sender metadata
  };
  std::deque<PudicaProbeResult> pudica_probe_results_;
  int64_t pudica_last_frame_recv_us_ = 0;  // last frame's last packet arrival (μs)
  int64_t pudica_last_frame_send_us_ = 0;  // last frame's last packet send time (sender clock, μs)
  std::atomic<int64_t> frame_burst_start_us_{0};  // SCTP burst probe: frame boundary timestamp
  std::atomic<double> frame_jitter_mult_{1.0};    // SCTP jitter: per-frame random multiplier

  // Adaptive pacing multiplier ρ
  static std::atomic<double> pudica_rho_;

  // Smoothed BUR (Eq.6 + Appendix B, 200 ms window)
  std::deque<PudicaBurSampleEq6> pudica_bur_history_;
  static constexpr int64_t kPudicaBurWindowUs = 200'000;  // 200ms in μs
  PudicaRtpRateCtrl pudica_rtp_ctrl_;
  bool pudica_legacy_ = false;  // PUDICA_LEGACY=1 → old recv-anchored 3-way branch

  // Pudica debug CSV
  std::ofstream pudica_csv_file_;
  bool pudica_csv_initialized_ = false;
  int64_t pudica_csv_start_us_ = 0;

  // Pudica RTP rate-controller trace CSV (rotary diagnostics)
  std::ofstream pudica_ctrl_csv_;
  bool pudica_ctrl_csv_initialized_ = false;
  int64_t pudica_ctrl_csv_start_us_ = 0;
  // [A28d] next_delay.csv -- one row per GetPudicaRtpOverride() call, i.e. per
  // TWCC feedback. pudica_ctrl.csv cannot answer whether next delay fired: its
  // rows are written on FRAME COMPLETION, which is exactly what stops during a
  // blackout (5 of 7 zero-capacity seconds in run 1789021240 produced zero
  // ctrl rows). This log runs on the path that keeps ticking, and it records
  // the no-fire cases too -- including the empty-ledger early return, which is
  // the leading hypothesis for the missing fallback.
  std::ofstream pudica_nd_csv_;
  bool pudica_nd_csv_initialized_ = false;
  int64_t pudica_nd_csv_start_us_ = 0;
  std::mutex pudica_nd_csv_mu_;
  void PudicaLogNextDelay(int64_t now_us, double next_delay_ms, double d_min_ms,
                          int steps, int64_t target_bps, int64_t out_bps,
                          const char* reason, int64_t oldest_us, int64_t acked_us,
                          uint64_t oldest_id, uint64_t acked_id);

  // Pudica methods
  double PudicaComputeFrameBur(int64_t now_us);
  double PudicaSmoothedBur(int64_t now_us);
  // Per-frame Pudica RTP target (NSDI'24 §4.2–§4.3). Default is paper
  // MI / AI-MD / fallback / 3-frame drain / drain-exit restore. PUDICA_LEGACY=1
  // restores the old recv-anchored 3-way branch (J-251).
  void PudicaUpdateRtpTarget(double frame_bur, int64_t now_us);

  // ===== MAFS Flow Scheduling =====
  MafsConfig mafs_config_;
  std::unique_ptr<coordinator::MultiAgentFlowCoordinator> flow_coordinator_;
  void MaybeUpdateMafsPriorities(int64_t now_ms);
};

}  // namespace webrtc

#endif  // PC_RTP_SCTP_COORDINATOR_H_
