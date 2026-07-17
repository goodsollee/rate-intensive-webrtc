#ifndef EXAMPLES_PEERCONNECTION_CLIENT_TRAFFIC_PROFILE_H_
#define EXAMPLES_PEERCONNECTION_CLIENT_TRAFFIC_PROFILE_H_

#include <optional>
#include <string>
#include <vector>

struct TrafficProfile {
  std::string traffic_name;
  std::string protocol;
  std::string pattern;
  int file_size = 0;
  int periodicity = 0;
  std::string custom_trace;
  int max_bitrate = 0;
  int frame_rate = 0;
  std::string video_file_name;
  int slo_ms = 0;
};

struct RtpTrafficConfig {
  std::string traffic_name;
  std::string pattern;
  int max_bitrate = 0;
  int frame_rate = 0;
  std::string video_file_name;
};

// Loads traffic profiles from a CSV file at |path|.
// The CSV is expected to have columns matching the fields in TrafficProfile.
std::vector<TrafficProfile> LoadProfiles(const std::string& path);

// Loads only SCTP profiles from the CSV at |path|. Returns an empty vector when
// the file does not exist or no SCTP profiles are present.
std::vector<TrafficProfile> LoadSctpProfiles(const std::string& path);

// Loads the first RTP profile from the CSV at |path|. Returns std::nullopt if
// the file does not exist or no RTP profile could be parsed.
std::optional<RtpTrafficConfig> LoadRtpConfig(const std::string& path);

#endif  // EXAMPLES_PEERCONNECTION_CLIENT_TRAFFIC_PROFILE_H_
