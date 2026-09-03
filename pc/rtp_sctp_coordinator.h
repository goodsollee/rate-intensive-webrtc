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
  kPudica,     // Pudica NSDI'24: RTP-only BUR measurement (rate control = AgentRtc)
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
  bool pudica_intra_frame_pacing = true;  // Eq.2: spread a frame's packets over
                                          // L/ρ instead of burst-then-gap. Off
                                          // reverts to the gap-only pacer.
  bool pudica_recvrate_fixed_window = false;  // Divide acked bytes by the fixed
                                              // prune window, not the observed
                                              // sample span

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
                                      int64_t max_data_rate_bps = -1,
                                      int64_t acked_bitrate_bps = 0);

  // ===== Pudica: per-packet OWD feedback (called from DelayBasedBwe) =====
  static void OnPudicaPacketFeedback(int64_t transport_seq,
                                      int64_t send_time_us,
                                      int64_t recv_time_us,
                                      bool is_probe,
                                      bool is_frame_last);

  // Pudica: adaptive pacing multiplier (read by PacingController)
  static double GetPudicaPacingMultiplier();
  static bool IsPudicaProbingEnabled();

  // ===== Pudica RTP-video rate override (Apollo) =====
  // Mirrors the FSE override: delay_based_bwe applies GetPudicaRtpOverride()
  // as the RTP delay-based target when IsPudicaMode() is true. The override is
  // recomputed per frame from the frame BUR + measured receiving_rate.
  static bool IsPudicaMode();
  static int64_t GetPudicaRtpOverride();

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
  // steady_clock us when pudica_rtp_target_bps_ was last published. The
  // controller only runs on a completed frame, so a dip deep enough to stop
  // frame completions freezes the override; GetPudicaRtpOverride() uses this
  // to hand the rate back to GCC instead of serving a stale value forever.
  static std::atomic<int64_t> pudica_rtp_target_pub_us_;
  static std::atomic<bool> pudica_mode_active_;        // Apollo: Pudica mode flag for GCC
  // §4.3 next delay: the newest send time seen in TWCC feedback. Every frame
  // the pacer recorded at or before this has been accounted for, so the pacer's
  // oldest survivor is the "earliest sent frame among the in-flight frames".
  // Written from the feedback path, read from GetPudicaRtpOverride().
  static std::atomic<int64_t> pudica_acked_send_us_;

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
  // GoogCc's AcknowledgedBitrateEstimator output — the delivered RTP rate,
  // windowed on TWCC receive timestamps. Preferred over rtp_ack_samples_ above,
  // which this fork rolled by hand; see GetRtpRecvRateBps. 0 until GCC has one.
  std::atomic<int64_t> rtp_acked_bitrate_bps_{0};

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
    int64_t last_send_us = -1;    // last packet send time (μs); the frame's own
                                  // send span, needed to keep it out of Eq.1 --
                                  // see PudicaComputeFrameBur.
    int64_t last_recv_us = -1;    // last packet receive time (μs)
    int frame_packets = 0;
  };
  PudicaFrameInfo pudica_frame_;

  // D_min: minimum packet OWD over 10-second window (μs)
  double pudica_d_min_us_ = -1.0;
  std::deque<std::pair<int64_t, double>> pudica_owd_window_;  // (time_us, owd_us)
  // (time_us, recv_rate_bps) for the delivered-rate ceiling's rolling max.
  // See PUDICA_ACK_CEIL_K in PudicaUpdateRtpTarget().
  std::deque<std::pair<int64_t, int64_t>> pudica_recv_window_;
  static constexpr int64_t kPudicaDminWindowUs = 10'000'000;  // 10 seconds in μs
  // A frame's packets all leave within L/rho <= L. Past this many frame
  // intervals the marker that would close pudica_frame_ is not coming.
  static constexpr double kFrameStaleFactor = 3.0;
  // Frames discarded by that watchdog. Non-zero means markers are being lost
  // (V7 ABANDON_DRB does exactly that), so Eq.1 is measuring fewer frames.
  std::atomic<int64_t> pudica_stale_frames_{0};

  // SACK_AGG=diff state: previous frame's mean RTT (μs)
  double prev_frame_mean_rtt_us_ = -1.0;
  // SACK_AGG=ewma state: slow EWMA of mean RTT (μs)
  double slow_ewma_mean_rtt_us_ = -1.0;

  // Probe packet results
  struct PudicaProbeResult {
    double owd_us;    // probe one-way delay (μs)
    double h_us;      // time since last frame packet arrival (μs)
  };
  std::deque<PudicaProbeResult> pudica_probe_results_;
  int64_t pudica_last_frame_recv_us_ = 0;  // last frame's last packet arrival (μs)

  // §4.3 short-term BUR control state.
  // The persistent target, before any temporary fallback is applied. Keeping it
  // separate is what makes the fallback transient: the published override may
  // carry -zeta for one frame while this value is untouched.
  int64_t pudica_base_target_bps_ = 0;
  int pudica_consec_high_bur_ = 0;  // frames in a row with R > 1
  bool pudica_draining_ = false;    // inside the Eq.11 queue-draining phase

  // §4.2 state.
  // tau: frames received since the last AI-step initialization. Grows the AI
  // step; reset when R~ > 1 and, time-driven, every 5 s.
  int64_t pudica_tau_ = 0;
  int64_t pudica_tau_reset_us_ = 0;
  // MI defers the next increase until the feedback for the previous one is in
  // ("the next adjustment is postponed until the feedback regarding the current
  // adjustment is received"), counted in frames.
  int pudica_mi_hold_frames_ = 0;
  std::atomic<int64_t> frame_burst_start_us_{0};  // SCTP burst probe: frame boundary timestamp
  std::atomic<double> frame_jitter_mult_{1.0};    // SCTP jitter: per-frame random multiplier

  // Adaptive pacing multiplier ρ
  static std::atomic<double> pudica_rho_;

  // Smoothed BUR (weighted average over 200ms, 논문 Eq.6)
  struct PudicaBurSample {
    int64_t time_us;
    double bur;
    double bitrate_bps;       // encoding bitrate B_k the frame was produced at
    double frame_rate_bps;    // the frame's OWN rate (size / L); see ratsum
  };
  std::deque<PudicaBurSample> pudica_bur_history_;
  static constexpr int64_t kPudicaBurWindowUs = 200'000;  // 200ms in μs

  // Pudica debug CSV
  std::ofstream pudica_csv_file_;
  bool pudica_csv_initialized_ = false;
  int64_t pudica_csv_start_us_ = 0;

  // Per-packet OWD trace (PUDICA_OWD_TRACE=1, off by default). Eq.1's D is the
  // frame's LAST receive time, i.e. the max over the frame's N packets, while
  // D_min is the min over a 10 s window of single packets. If per-packet OWD has
  // jitter, D - D_min picks up max(N) - min(window) — an extreme-value gap that
  // grows with N and then saturates, which is the shape of the ~6-8 ms
  // load-independent excess in section 4c. This trace records every packet's OWD
  // so that gap can be measured directly instead of inferred.
  std::ofstream pudica_owd_file_;
  bool pudica_owd_initialized_ = false;
  int64_t pudica_owd_start_us_ = 0;
  int64_t pudica_owd_frame_id_ = 0;
  int pudica_owd_pkt_in_frame_ = 0;

  // Pudica RTP rate-controller trace CSV (rotary diagnostics)
  std::ofstream pudica_ctrl_csv_;
  bool pudica_ctrl_csv_initialized_ = false;
  int64_t pudica_ctrl_csv_start_us_ = 0;

  // Pudica methods
  double PudicaComputeFrameBur(int64_t now_us);
  double PudicaSmoothedBur(int64_t now_us);
  // Apollo: per-frame Pudica RTP-video rate controller. Called on each frame
  // completion with the frame BUR; updates pudica_rtp_target_bps_.
  void PudicaUpdateRtpTarget(double frame_bur, int64_t now_us);

  // ===== MAFS Flow Scheduling =====
  MafsConfig mafs_config_;
  std::unique_ptr<coordinator::MultiAgentFlowCoordinator> flow_coordinator_;
  void MaybeUpdateMafsPriorities(int64_t now_ms);
};

}  // namespace webrtc

#endif  // PC_RTP_SCTP_COORDINATOR_H_
