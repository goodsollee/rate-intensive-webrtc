/*
 *  Shared state between RTP video bitrate control and SCTP pacing coordinator.
 *  Minimal header — no dependencies beyond <atomic> and <cstdint>.
 *  Both call/ and pc/ can include this without circular deps.
 */

#ifndef PC_RTP_SCTP_SHARED_STATE_H_
#define PC_RTP_SCTP_SHARED_STATE_H_

#include <atomic>
#include <cstdint>

namespace webrtc {
namespace rtp_sctp_shared {

// Written by RtpSctpCoordinator during one-shot recovery, read by RTP BWE
extern std::atomic<int64_t> recommended_video_bitrate_bps;

// Written by VideoSendStreamImpl::OnBitrateUpdated(), read by coordinator
extern std::atomic<int64_t> current_video_bitrate_bps;

// Feature toggle (set from RTP_RECOVERY_ENABLED env var)
extern std::atomic<bool> rtp_recovery_enabled;

}  // namespace rtp_sctp_shared
}  // namespace webrtc

#endif  // PC_RTP_SCTP_SHARED_STATE_H_
