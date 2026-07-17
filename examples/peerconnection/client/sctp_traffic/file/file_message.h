#pragma once

#include <cstddef>
#include <cstdint>

#include "absl/types/span.h"
#include "modules/rtp_rtcp/source/byte_io.h"

namespace sctp::file {

// Header that precedes every SCTP file chunk. The header is always written
// using little-endian byte order.
struct FileChunkHeader {
  uint64_t sequence = 0;
  uint64_t send_time_ms = 0;
  uint64_t file_size_bytes = 0;
  uint64_t chunk_size_bytes = 0;
  uint32_t chunk_index = 0;
  uint32_t chunk_count = 0;
};

// The serialized size of the header. Keep this in sync with FileChunkHeader.
constexpr size_t kFileChunkHeaderSize =
    sizeof(uint64_t) * 4 + sizeof(uint32_t) * 2;

inline void WriteFileChunkHeader(uint8_t* dest,
                                 const FileChunkHeader& header) {
  webrtc::ByteWriter<uint64_t>::WriteLittleEndian(dest, header.sequence);
  webrtc::ByteWriter<uint64_t>::WriteLittleEndian(dest + sizeof(uint64_t),
                                                  header.send_time_ms);
  webrtc::ByteWriter<uint64_t>::WriteLittleEndian(
      dest + sizeof(uint64_t) * 2, header.file_size_bytes);
  webrtc::ByteWriter<uint64_t>::WriteLittleEndian(
      dest + sizeof(uint64_t) * 3, header.chunk_size_bytes);
  webrtc::ByteWriter<uint32_t>::WriteLittleEndian(
      dest + sizeof(uint64_t) * 4, header.chunk_index);
  webrtc::ByteWriter<uint32_t>::WriteLittleEndian(
      dest + sizeof(uint64_t) * 4 + sizeof(uint32_t), header.chunk_count);
}

inline bool ParseFileChunkHeader(absl::Span<const uint8_t> bytes,
                                 FileChunkHeader* header) {
  if (!header || bytes.size() < kFileChunkHeaderSize) {
    return false;
  }

  header->sequence =
      webrtc::ByteReader<uint64_t>::ReadLittleEndian(bytes.data());
  header->send_time_ms = webrtc::ByteReader<uint64_t>::ReadLittleEndian(
      bytes.data() + sizeof(uint64_t));
  header->file_size_bytes = webrtc::ByteReader<uint64_t>::ReadLittleEndian(
      bytes.data() + sizeof(uint64_t) * 2);
  header->chunk_size_bytes = webrtc::ByteReader<uint64_t>::ReadLittleEndian(
      bytes.data() + sizeof(uint64_t) * 3);
  header->chunk_index = webrtc::ByteReader<uint32_t>::ReadLittleEndian(
      bytes.data() + sizeof(uint64_t) * 4);
  header->chunk_count = webrtc::ByteReader<uint32_t>::ReadLittleEndian(
      bytes.data() + sizeof(uint64_t) * 4 + sizeof(uint32_t));

  return header->chunk_count > 0;
}

}  // namespace sctp::file

