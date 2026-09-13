# Local evaluation integration

This branch combines upstream `evaluation` (`a2473c8`) with Pudica A41
(`472feb0`) and the deployed `nvenc-pudica-a41` source checkpoint (`684a736`).
The NVENC encoder, decoder threading, RTX/payload-type handling, MAE and
resolution adaptation are retained.

The merge imports evaluation's media-only pacer wakeups and end-of-frame debt
handling, sliding-minimum D_min calculation, target-anchored next-delay fallback,
OpenH264 MAE/QP/thread/slice changes, ten-second receiver activity check and
Gecko/L4S updates. A41's configurable frame cadence, sender decision clock,
measured-rate ceiling safeguards, exact probe/frame metadata and no-frame-drop
queue preservation remain in place. D_min continues to exclude probes.

The next-delay fallback intentionally uses `target * (1-zeta)^steps`, replacing
the integrated branch's receiving-rate anchor. Existing benchmark results across
this boundary are different software cohorts; no performance improvement is
implied by the merge.

`--video_codec` defaults to `H264` as on upstream evaluation. Use an explicit
`--video_codec=VP8` for a VP8 experiment. `KFT_NVENC=1` selects the retained NVENC
encoder factory. Existing `KFT_MAE` and `KFT_NO_FRAME_DROP` gates are preserved;
the imported OpenH264 implementation also consumes them.

OpenH264 must include
`modules/video_coding/codecs/h264/openh264_kft_mae.patch`. Apply it to a private
dependency checkout, never a dependency tree shared with an older build. The
integration worktree has an isolated patched copy. Other dependency locations
and exact tool hashes are recorded with the build evidence.

Validation and immutable build/release receipts are maintained in the workspace
under `agent/evaluation_integration_20260912/`. Production builds follow
`agent/HARNESS.md` section 5.2 and `fgq-releases/docs/BUILD_RELEASE.md`.
