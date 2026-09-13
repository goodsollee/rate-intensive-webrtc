#ifndef EXAMPLES_PEERCONNECTION_CLIENT_NVENC_NVENC_ENV_H_
#define EXAMPLES_PEERCONNECTION_CLIENT_NVENC_NVENC_ENV_H_

#include <cstdlib>

namespace webrtc {
namespace nvenc {

// Opt-in hardware encode. Unset or "0" => stock VP8 factory (byte-identical
// offer). Any other non-empty value advertises NVENC H.264 High 5.2 only.
inline bool NvencEnabled() {
  static const bool on = [] {
    const char* e = std::getenv("KFT_NVENC");
    return e != nullptr && e[0] != '\0' && !(e[0] == '0' && e[1] == '\0');
  }();
  return on;
}

inline bool MaeEnabled() {
  static const bool on = [] {
    const char* e = std::getenv("KFT_MAE");
    return e != nullptr && e[0] != '\0' && !(e[0] == '0' && e[1] == '\0');
  }();
  return on;
}

}  // namespace nvenc
}  // namespace webrtc

#endif  // EXAMPLES_PEERCONNECTION_CLIENT_NVENC_NVENC_ENV_H_
