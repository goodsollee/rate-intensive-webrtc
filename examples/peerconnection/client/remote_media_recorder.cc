/*
 *  Copyright 2024 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "examples/peerconnection/client/remote_media_recorder.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include "api/environment/environment_factory.h"
#include "api/video/i420_buffer.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "api/video_codecs/sdp_video_format.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "rtc_base/logging.h"

namespace webrtc_example {
namespace {

inline void WriteUint32(std::vector<uint8_t>& buf, uint32_t value) {
  buf.push_back((value >> 24) & 0xFF);
  buf.push_back((value >> 16) & 0xFF);
  buf.push_back((value >> 8) & 0xFF);
  buf.push_back(value & 0xFF);
}

inline void WriteTag(std::vector<uint8_t>& buf, const char tag[4]) {
  buf.insert(buf.end(), tag, tag + 4);
}

inline void WriteUint16(std::vector<uint8_t>& buf, uint16_t value) {
  buf.push_back((value >> 8) & 0xFF);
  buf.push_back(value & 0xFF);
}

void WriteMatrix(std::vector<uint8_t>& buf) {
  // unity matrix
  WriteUint32(buf, 0x00010000);
  WriteUint32(buf, 0);
  WriteUint32(buf, 0);
  WriteUint32(buf, 0);
  WriteUint32(buf, 0x00010000);
  WriteUint32(buf, 0);
  WriteUint32(buf, 0);
  WriteUint32(buf, 0);
  WriteUint32(buf, 0x40000000);
}

std::vector<std::vector<uint8_t>> ParseAnnexBNalus(const uint8_t* data,
                                                   size_t size) {
  std::vector<std::vector<uint8_t>> nalus;
  size_t offset = 0;
  while (offset + 3 < size) {
    while (offset < size && data[offset] == 0) {
      ++offset;
    }
    if (offset == size) {
      break;
    }
    if (offset + 3 >= size) {
      break;
    }
    if (data[offset] != 1) {
      // Not a start code, skip
      ++offset;
      continue;
    }
    offset++;
    size_t next = offset;
    while (next + 3 < size) {
      if (data[next] == 0 && data[next + 1] == 0 &&
          ((data[next + 2] == 1) ||
           (data[next + 2] == 0 && next + 3 < size && data[next + 3] == 1))) {
        break;
      }
      ++next;
    }
    nalus.emplace_back(data + offset, data + next);
    offset = next;
  }
  return nalus;
}

std::vector<uint8_t> ConvertToLengthPrefixed(
    const std::vector<std::vector<uint8_t>>& nalus) {
  std::vector<uint8_t> output;
  for (const auto& nalu : nalus) {
    WriteUint32(output, static_cast<uint32_t>(nalu.size()));
    output.insert(output.end(), nalu.begin(), nalu.end());
  }
  return output;
}

}  // namespace

class Mp4FileWriter {
 public:
  Mp4FileWriter(FILE* file, uint32_t timescale)
      : file_(file), timescale_(timescale) {}
  ~Mp4FileWriter() { if (file_) { fclose(file_); } }

  bool Begin() {
    if (!file_) {
      return false;
    }
    // ftyp
    std::vector<uint8_t> ftyp;
    WriteUint32(ftyp, 0);  // placeholder size
    WriteTag(ftyp, "ftyp");
    WriteTag(ftyp, "isom");
    WriteUint32(ftyp, 0x00000200);
    WriteTag(ftyp, "isom");
    WriteTag(ftyp, "iso2");
    WriteTag(ftyp, "avc1");
    WriteTag(ftyp, "mp41");
    uint32_t size = static_cast<uint32_t>(ftyp.size());
    ftyp[0] = (size >> 24) & 0xFF;
    ftyp[1] = (size >> 16) & 0xFF;
    ftyp[2] = (size >> 8) & 0xFF;
    ftyp[3] = size & 0xFF;
    if (fwrite(ftyp.data(), 1, ftyp.size(), file_) != ftyp.size()) {
      return false;
    }

    mdat_header_pos_ = ftell(file_);
    if (mdat_header_pos_ < 0) {
      return false;
    }
    uint8_t mdat_header[8] = {0, 0, 0, 0, 'm', 'd', 'a', 't'};
    if (fwrite(mdat_header, 1, sizeof(mdat_header), file_) !=
        sizeof(mdat_header)) {
      return false;
    }
    data_start_ = ftell(file_);
    return data_start_ >= 0;
  }

  bool WriteSample(const uint8_t* data,
                   size_t size,
                   uint64_t pts,
                   bool keyframe) {
    if (!file_) {
      return false;
    }
    int64_t offset = ftell(file_);
    if (offset < 0) {
      return false;
    }
    if (size > 0 && fwrite(data, 1, size, file_) != size) {
      return false;
    }
    total_data_size_ += size;
    Sample sample;
    sample.offset = static_cast<uint64_t>(offset);
    sample.size = static_cast<uint32_t>(size);
    sample.pts = pts;
    sample.keyframe = keyframe;
    samples_.push_back(sample);
    return true;
  }

  bool Finalize(uint32_t width,
                uint32_t height,
                const std::vector<uint8_t>& sps,
                const std::vector<uint8_t>& pps,
                const std::vector<uint32_t>& durations) {
    if (!file_) {
      return false;
    }
    if (samples_.empty()) {
      return false;
    }
    if (fseek(file_, mdat_header_pos_, SEEK_SET) != 0) {
      return false;
    }
    uint64_t mdat_size = 8 + total_data_size_;
    uint8_t header[8];
    header[0] = (mdat_size >> 24) & 0xFF;
    header[1] = (mdat_size >> 16) & 0xFF;
    header[2] = (mdat_size >> 8) & 0xFF;
    header[3] = (mdat_size) & 0xFF;
    header[4] = 'm';
    header[5] = 'd';
    header[6] = 'a';
    header[7] = 't';
    if (fwrite(header, 1, sizeof(header), file_) != sizeof(header)) {
      return false;
    }
    if (fseek(file_, 0, SEEK_END) != 0) {
      return false;
    }

    std::vector<uint8_t> moov = BuildMoov(width, height, sps, pps, durations);
    if (fwrite(moov.data(), 1, moov.size(), file_) != moov.size()) {
      return false;
    }
    fflush(file_);
    return true;
  }

 private:
  struct Sample {
    uint64_t pts;
    uint64_t offset;
    uint32_t size;
    bool keyframe;
  };

  std::vector<uint8_t> BuildMoov(uint32_t width,
                                 uint32_t height,
                                 const std::vector<uint8_t>& sps,
                                 const std::vector<uint8_t>& pps,
                                 const std::vector<uint32_t>& durations) {
    std::vector<uint8_t> moov;
    std::vector<uint8_t> mvhd = BuildMvhd(durations);
    std::vector<uint8_t> trak = BuildTrak(width, height, sps, pps, durations);

    WriteUint32(moov, 0);
    WriteTag(moov, "moov");
    moov.insert(moov.end(), mvhd.begin(), mvhd.end());
    moov.insert(moov.end(), trak.begin(), trak.end());
    uint32_t size = static_cast<uint32_t>(moov.size());
    moov[0] = (size >> 24) & 0xFF;
    moov[1] = (size >> 16) & 0xFF;
    moov[2] = (size >> 8) & 0xFF;
    moov[3] = size & 0xFF;
    return moov;
  }

  std::vector<uint8_t> BuildMvhd(const std::vector<uint32_t>& durations) {
    std::vector<uint8_t> box;
    WriteUint32(box, 0);
    WriteTag(box, "mvhd");
    box.push_back(0);  // version
    box.insert(box.end(), {0, 0, 0});  // flags
    WriteUint32(box, 0);
    WriteUint32(box, 0);
    WriteUint32(box, timescale_);
    uint64_t total = 0;
    for (uint32_t d : durations) {
      total += d;
    }
    WriteUint32(box, static_cast<uint32_t>(total));
    WriteUint32(box, 0x00010000);
    WriteUint16(box, 0x0100);
    WriteUint16(box, 0);
    WriteUint32(box, 0);
    WriteUint32(box, 0);
    WriteUint32(box, 0);
    WriteMatrix(box);
    for (int i = 0; i < 6; ++i) {
      WriteUint32(box, 0);
    }
    WriteUint32(box, 1);  // next_track_id
    uint32_t size = static_cast<uint32_t>(box.size());
    box[0] = (size >> 24) & 0xFF;
    box[1] = (size >> 16) & 0xFF;
    box[2] = (size >> 8) & 0xFF;
    box[3] = size & 0xFF;
    return box;
  }

  std::vector<uint8_t> BuildTrak(uint32_t width,
                                 uint32_t height,
                                 const std::vector<uint8_t>& sps,
                                 const std::vector<uint8_t>& pps,
                                 const std::vector<uint32_t>& durations) {
    std::vector<uint8_t> tkhd = BuildTkhd(width, height, durations);
    std::vector<uint8_t> mdia = BuildMdia(width, height, sps, pps, durations);

    std::vector<uint8_t> box;
    WriteUint32(box, 0);
    WriteTag(box, "trak");
    box.insert(box.end(), tkhd.begin(), tkhd.end());
    box.insert(box.end(), mdia.begin(), mdia.end());
    uint32_t size = static_cast<uint32_t>(box.size());
    box[0] = (size >> 24) & 0xFF;
    box[1] = (size >> 16) & 0xFF;
    box[2] = (size >> 8) & 0xFF;
    box[3] = size & 0xFF;
    return box;
  }

  std::vector<uint8_t> BuildTkhd(uint32_t width,
                                 uint32_t height,
                                 const std::vector<uint32_t>& durations) {
    std::vector<uint8_t> box;
    WriteUint32(box, 0);
    WriteTag(box, "tkhd");
    box.push_back(0);
    box.insert(box.end(), {0, 0, 7});
    WriteUint32(box, 0);
    WriteUint32(box, 0);
    WriteUint32(box, 1);
    uint64_t total = 0;
    for (uint32_t d : durations) {
      total += d;
    }
    WriteUint32(box, static_cast<uint32_t>(total));
    WriteUint32(box, 0);
    WriteUint32(box, 0);
    WriteUint16(box, 0);
    WriteUint16(box, 0);
    WriteUint16(box, 0);
    WriteUint16(box, 0);
    WriteMatrix(box);
    WriteUint32(box, width << 16);
    WriteUint32(box, height << 16);
    uint32_t size = static_cast<uint32_t>(box.size());
    box[0] = (size >> 24) & 0xFF;
    box[1] = (size >> 16) & 0xFF;
    box[2] = (size >> 8) & 0xFF;
    box[3] = size & 0xFF;
    return box;
  }

  std::vector<uint8_t> BuildMdia(uint32_t width,
                                 uint32_t height,
                                 const std::vector<uint8_t>& sps,
                                 const std::vector<uint8_t>& pps,
                                 const std::vector<uint32_t>& durations) {
    std::vector<uint8_t> mdhd = BuildMdhd(durations);
    std::vector<uint8_t> hdlr = BuildHdlr();
    std::vector<uint8_t> minf = BuildMinf(width, height, sps, pps, durations);

    std::vector<uint8_t> box;
    WriteUint32(box, 0);
    WriteTag(box, "mdia");
    box.insert(box.end(), mdhd.begin(), mdhd.end());
    box.insert(box.end(), hdlr.begin(), hdlr.end());
    box.insert(box.end(), minf.begin(), minf.end());
    uint32_t size = static_cast<uint32_t>(box.size());
    box[0] = (size >> 24) & 0xFF;
    box[1] = (size >> 16) & 0xFF;
    box[2] = (size >> 8) & 0xFF;
    box[3] = size & 0xFF;
    return box;
  }

  std::vector<uint8_t> BuildMdhd(const std::vector<uint32_t>& durations) {
    std::vector<uint8_t> box;
    WriteUint32(box, 0);
    WriteTag(box, "mdhd");
    box.push_back(0);
    box.insert(box.end(), {0, 0, 0});
    WriteUint32(box, 0);
    WriteUint32(box, 0);
    WriteUint32(box, timescale_);
    uint64_t total = 0;
    for (uint32_t d : durations) {
      total += d;
    }
    WriteUint32(box, static_cast<uint32_t>(total));
    WriteUint16(box, 0);
    WriteUint16(box, 0);
    uint32_t size = static_cast<uint32_t>(box.size());
    box[0] = (size >> 24) & 0xFF;
    box[1] = (size >> 16) & 0xFF;
    box[2] = (size >> 8) & 0xFF;
    box[3] = size & 0xFF;
    return box;
  }

  std::vector<uint8_t> BuildHdlr() {
    std::vector<uint8_t> box;
    WriteUint32(box, 0);
    WriteTag(box, "hdlr");
    box.push_back(0);
    box.insert(box.end(), {0, 0, 0});
    WriteUint32(box, 0);
    WriteTag(box, "vide");
    WriteUint32(box, 0);
    WriteUint32(box, 0);
    WriteUint32(box, 0);
    const char name[] = "VideoHandler";
    box.insert(box.end(), name, name + sizeof(name));
    uint32_t size = static_cast<uint32_t>(box.size());
    box[0] = (size >> 24) & 0xFF;
    box[1] = (size >> 16) & 0xFF;
    box[2] = (size >> 8) & 0xFF;
    box[3] = size & 0xFF;
    return box;
  }

  std::vector<uint8_t> BuildMinf(uint32_t width,
                                 uint32_t height,
                                 const std::vector<uint8_t>& sps,
                                 const std::vector<uint8_t>& pps,
                                 const std::vector<uint32_t>& durations) {
    std::vector<uint8_t> vmhd = BuildVmhd();
    std::vector<uint8_t> dinf = BuildDinf();
    std::vector<uint8_t> stbl = BuildStbl(width, height, sps, pps, durations);

    std::vector<uint8_t> box;
    WriteUint32(box, 0);
    WriteTag(box, "minf");
    box.insert(box.end(), vmhd.begin(), vmhd.end());
    box.insert(box.end(), dinf.begin(), dinf.end());
    box.insert(box.end(), stbl.begin(), stbl.end());
    uint32_t size = static_cast<uint32_t>(box.size());
    box[0] = (size >> 24) & 0xFF;
    box[1] = (size >> 16) & 0xFF;
    box[2] = (size >> 8) & 0xFF;
    box[3] = size & 0xFF;
    return box;
  }

  std::vector<uint8_t> BuildVmhd() {
    std::vector<uint8_t> box;
    WriteUint32(box, 0);
    WriteTag(box, "vmhd");
    box.push_back(0);
    box.insert(box.end(), {0, 0, 1});
    WriteUint16(box, 0);
    WriteUint16(box, 0);
    WriteUint16(box, 0);
    WriteUint16(box, 0);
    uint32_t size = static_cast<uint32_t>(box.size());
    box[0] = (size >> 24) & 0xFF;
    box[1] = (size >> 16) & 0xFF;
    box[2] = (size >> 8) & 0xFF;
    box[3] = size & 0xFF;
    return box;
  }

  std::vector<uint8_t> BuildDinf() {
    std::vector<uint8_t> dref;
    WriteUint32(dref, 0);
    WriteTag(dref, "dref");
    dref.push_back(0);
    dref.insert(dref.end(), {0, 0, 0});
    WriteUint32(dref, 1);
    std::vector<uint8_t> url;
    WriteUint32(url, 0x0000000C);
    WriteTag(url, "url ");
    url.push_back(0);
    url.insert(url.end(), {0, 0, 1});
    dref.insert(dref.end(), url.begin(), url.end());
    uint32_t dref_size = static_cast<uint32_t>(dref.size());
    dref[0] = (dref_size >> 24) & 0xFF;
    dref[1] = (dref_size >> 16) & 0xFF;
    dref[2] = (dref_size >> 8) & 0xFF;
    dref[3] = dref_size & 0xFF;

    std::vector<uint8_t> box;
    WriteUint32(box, 0);
    WriteTag(box, "dinf");
    box.insert(box.end(), dref.begin(), dref.end());
    uint32_t size = static_cast<uint32_t>(box.size());
    box[0] = (size >> 24) & 0xFF;
    box[1] = (size >> 16) & 0xFF;
    box[2] = (size >> 8) & 0xFF;
    box[3] = size & 0xFF;
    return box;
  }

  std::vector<uint8_t> BuildStbl(uint32_t width,
                                 uint32_t height,
                                 const std::vector<uint8_t>& sps,
                                 const std::vector<uint8_t>& pps,
                                 const std::vector<uint32_t>& durations) {
    std::vector<uint8_t> stsd = BuildStsd(width, height, sps, pps);
    std::vector<uint8_t> stts = BuildStts(durations);
    std::vector<uint8_t> stsc = BuildStsc();
    std::vector<uint8_t> stsz = BuildStsz();
    std::vector<uint8_t> stco = BuildStco();
    std::vector<uint8_t> stss = BuildStss();

    std::vector<uint8_t> box;
    WriteUint32(box, 0);
    WriteTag(box, "stbl");
    box.insert(box.end(), stsd.begin(), stsd.end());
    box.insert(box.end(), stts.begin(), stts.end());
    box.insert(box.end(), stsc.begin(), stsc.end());
    box.insert(box.end(), stsz.begin(), stsz.end());
    box.insert(box.end(), stco.begin(), stco.end());
    if (!stss.empty()) {
      box.insert(box.end(), stss.begin(), stss.end());
    }
    uint32_t size = static_cast<uint32_t>(box.size());
    box[0] = (size >> 24) & 0xFF;
    box[1] = (size >> 16) & 0xFF;
    box[2] = (size >> 8) & 0xFF;
    box[3] = size & 0xFF;
    return box;
  }

  std::vector<uint8_t> BuildStsd(uint32_t width,
                                 uint32_t height,
                                 const std::vector<uint8_t>& sps,
                                 const std::vector<uint8_t>& pps) {
    std::vector<uint8_t> avc1;
    WriteUint32(avc1, 0);
    WriteTag(avc1, "avc1");
    avc1.insert(avc1.end(), 6, 0);
    WriteUint16(avc1, 1);
    WriteUint16(avc1, 0);
    WriteUint16(avc1, 0);
    WriteUint32(avc1, 0);
    WriteUint32(avc1, 0);
    WriteUint32(avc1, 0);
    WriteUint16(avc1, width);
    WriteUint16(avc1, height);
    WriteUint32(avc1, 0x00480000);
    WriteUint32(avc1, 0x00480000);
    WriteUint32(avc1, 0);
    WriteUint16(avc1, 1);
    std::array<uint8_t, 32> compressor = {};
    compressor[0] = 11;
    const char label[] = "WebRTC H264";
    std::copy(label, label + 11, compressor.begin() + 1);
    avc1.insert(avc1.end(), compressor.begin(), compressor.end());
    WriteUint16(avc1, 0x0018);
    WriteUint16(avc1, 0xFFFF);

    std::vector<uint8_t> avcc;
    WriteUint32(avcc, 0);
    WriteTag(avcc, "avcC");
    avcc.push_back(1);
    if (sps.size() >= 4) {
      avcc.push_back(sps[1]);
      avcc.push_back(sps[2]);
      avcc.push_back(sps[3]);
    } else {
      avcc.insert(avcc.end(), {0x64, 0, 0x1F});
    }
    avcc.push_back(0xFF);
    avcc.push_back(0xE1);
    WriteUint16(avcc, static_cast<uint16_t>(sps.size()));
    avcc.insert(avcc.end(), sps.begin(), sps.end());
    avcc.push_back(1);
    WriteUint16(avcc, static_cast<uint16_t>(pps.size()));
    avcc.insert(avcc.end(), pps.begin(), pps.end());
    uint32_t avcc_size = static_cast<uint32_t>(avcc.size());
    avcc[0] = (avcc_size >> 24) & 0xFF;
    avcc[1] = (avcc_size >> 16) & 0xFF;
    avcc[2] = (avcc_size >> 8) & 0xFF;
    avcc[3] = avcc_size & 0xFF;
    avc1.insert(avc1.end(), avcc.begin(), avcc.end());
    uint32_t avc1_size = static_cast<uint32_t>(avc1.size());
    avc1[0] = (avc1_size >> 24) & 0xFF;
    avc1[1] = (avc1_size >> 16) & 0xFF;
    avc1[2] = (avc1_size >> 8) & 0xFF;
    avc1[3] = avc1_size & 0xFF;

    std::vector<uint8_t> stsd;
    WriteUint32(stsd, 0);
    WriteTag(stsd, "stsd");
    stsd.push_back(0);
    stsd.insert(stsd.end(), {0, 0, 0});
    WriteUint32(stsd, 1);
    stsd.insert(stsd.end(), avc1.begin(), avc1.end());
    uint32_t stsd_size = static_cast<uint32_t>(stsd.size());
    stsd[0] = (stsd_size >> 24) & 0xFF;
    stsd[1] = (stsd_size >> 16) & 0xFF;
    stsd[2] = (stsd_size >> 8) & 0xFF;
    stsd[3] = stsd_size & 0xFF;
    return stsd;
  }

  std::vector<uint8_t> BuildStts(const std::vector<uint32_t>& durations) {
    std::vector<uint8_t> box;
    WriteUint32(box, 0);
    WriteTag(box, "stts");
    box.push_back(0);
    box.insert(box.end(), {0, 0, 0});
    std::vector<std::pair<uint32_t, uint32_t>> run_length;
    uint32_t current_duration = durations[0];
    uint32_t count = 1;
    for (size_t i = 1; i < durations.size(); ++i) {
      if (durations[i] == current_duration) {
        ++count;
      } else {
        run_length.emplace_back(count, current_duration);
        current_duration = durations[i];
        count = 1;
      }
    }
    run_length.emplace_back(count, current_duration);
    WriteUint32(box, static_cast<uint32_t>(run_length.size()));
    for (auto& entry : run_length) {
      WriteUint32(box, entry.first);
      WriteUint32(box, entry.second);
    }
    uint32_t size = static_cast<uint32_t>(box.size());
    box[0] = (size >> 24) & 0xFF;
    box[1] = (size >> 16) & 0xFF;
    box[2] = (size >> 8) & 0xFF;
    box[3] = size & 0xFF;
    return box;
  }

  std::vector<uint8_t> BuildStsc() {
    std::vector<uint8_t> box;
    WriteUint32(box, 0);
    WriteTag(box, "stsc");
    box.push_back(0);
    box.insert(box.end(), {0, 0, 0});
    WriteUint32(box, 1);
    WriteUint32(box, 1);
    WriteUint32(box, 1);
    WriteUint32(box, 1);
    uint32_t size = static_cast<uint32_t>(box.size());
    box[0] = (size >> 24) & 0xFF;
    box[1] = (size >> 16) & 0xFF;
    box[2] = (size >> 8) & 0xFF;
    box[3] = size & 0xFF;
    return box;
  }

  std::vector<uint8_t> BuildStsz() {
    std::vector<uint8_t> box;
    WriteUint32(box, 0);
    WriteTag(box, "stsz");
    box.push_back(0);
    box.insert(box.end(), {0, 0, 0});
    WriteUint32(box, 0);
    WriteUint32(box, static_cast<uint32_t>(samples_.size()));
    for (const auto& sample : samples_) {
      WriteUint32(box, sample.size);
    }
    uint32_t size = static_cast<uint32_t>(box.size());
    box[0] = (size >> 24) & 0xFF;
    box[1] = (size >> 16) & 0xFF;
    box[2] = (size >> 8) & 0xFF;
    box[3] = size & 0xFF;
    return box;
  }

  std::vector<uint8_t> BuildStco() {
    std::vector<uint8_t> box;
    WriteUint32(box, 0);
    WriteTag(box, "stco");
    box.push_back(0);
    box.insert(box.end(), {0, 0, 0});
    WriteUint32(box, static_cast<uint32_t>(samples_.size()));
    for (const auto& sample : samples_) {
      WriteUint32(box, static_cast<uint32_t>(sample.offset));
    }
    uint32_t size = static_cast<uint32_t>(box.size());
    box[0] = (size >> 24) & 0xFF;
    box[1] = (size >> 16) & 0xFF;
    box[2] = (size >> 8) & 0xFF;
    box[3] = size & 0xFF;
    return box;
  }

  std::vector<uint8_t> BuildStss() {
    std::vector<uint32_t> indices;
    for (size_t i = 0; i < samples_.size(); ++i) {
      if (samples_[i].keyframe) {
        indices.push_back(static_cast<uint32_t>(i + 1));
      }
    }
    if (indices.empty()) {
      return {};
    }
    std::vector<uint8_t> box;
    WriteUint32(box, 0);
    WriteTag(box, "stss");
    box.push_back(0);
    box.insert(box.end(), {0, 0, 0});
    WriteUint32(box, static_cast<uint32_t>(indices.size()));
    for (uint32_t index : indices) {
      WriteUint32(box, index);
    }
    uint32_t size = static_cast<uint32_t>(box.size());
    box[0] = (size >> 24) & 0xFF;
    box[1] = (size >> 16) & 0xFF;
    box[2] = (size >> 8) & 0xFF;
    box[3] = size & 0xFF;
    return box;
  }

  FILE* file_ = nullptr;
  uint32_t timescale_;
  int64_t mdat_header_pos_ = -1;
  int64_t data_start_ = -1;
  uint64_t total_data_size_ = 0;
  std::vector<Sample> samples_;
};

RemoteMediaRecorder::RemoteMediaRecorder(
    std::unique_ptr<webrtc::VideoEncoder> encoder,
    std::unique_ptr<Mp4FileWriter> writer,
    uint32_t timescale)
    : encoder_(std::move(encoder)),
      writer_(std::move(writer)),
      encoder_callback_(this),
      timescale_(timescale) {}

RemoteMediaRecorder::~RemoteMediaRecorder() {
  Stop();
}

void RemoteMediaRecorder::Stop() {
  webrtc::MutexLock l(&lock_);
  FinalizeLocked();
}

void RemoteMediaRecorder::OnFrame(const webrtc::VideoFrame& frame) {
  webrtc::MutexLock l(&lock_);
  if (closed_) {
    return;
  }

  if (!encoder_initialized_) {
    if (!InitializeEncoder(frame)) {
      RTC_LOG(LS_WARNING) << "Failed to initialize encoder for recording.";
      closed_ = true;
      return;
    }
  }

  auto buffer = frame.video_frame_buffer()->ToI420();
  webrtc::VideoFrame frame_to_encode =
      webrtc::VideoFrame::Builder()
          .set_video_frame_buffer(buffer)
          .set_timestamp_us(frame.timestamp_us())
          .set_timestamp_rtp(frame.rtp_timestamp())
          .set_rotation(frame.rotation())
          .build();
  frame_to_encode.set_ntp_time_ms(frame.ntp_time_ms());

  if (encoder_->Encode(frame_to_encode, nullptr) != WEBRTC_VIDEO_CODEC_OK) {
    RTC_LOG(LS_WARNING) << "Failed to encode frame for recorder.";
  }
}

bool RemoteMediaRecorder::InitializeEncoder(const webrtc::VideoFrame& frame) {
  if (encoder_initialized_) {
    return true;
  }
  width_ = frame.width();
  height_ = frame.height();
  if (!EnsureWriterInitialized(width_, height_)) {
    return false;
  }
  webrtc::VideoCodec codec_settings;
  codec_settings.codecType = webrtc::kVideoCodecH264;
  codec_settings.width = width_;
  codec_settings.height = height_;
  codec_settings.startBitrate = target_bitrate_kbps_;
  codec_settings.maxBitrate = target_bitrate_kbps_ * 2;
  codec_settings.minBitrate = 300;
  codec_settings.maxFramerate = target_fps_;

  if (!encoder_) {
    return false;
  }
  encoder_->RegisterEncodeCompleteCallback(&encoder_callback_);
  int32_t result = encoder_->InitEncode(&codec_settings, 1, 1200);
  if (result != WEBRTC_VIDEO_CODEC_OK) {
    RTC_LOG(LS_ERROR) << "InitEncode failed: " << result;
    return false;
  }
  encoder_initialized_ = true;
  return true;
}

bool RemoteMediaRecorder::EnsureWriterInitialized(int width, int height) {
  if (writer_initialized_) {
    return true;
  }
  if (!writer_) {
    return false;
  }
  if (!writer_->Begin()) {
    return false;
  }
  writer_initialized_ = true;
  return true;
}

void RemoteMediaRecorder::HandleEncodedImage(
    const webrtc::EncodedImage& image) {
  if (!writer_initialized_) {
    return;
  }
  std::vector<std::vector<uint8_t>> nalus =
      ParseAnnexBNalus(image.data(), image.size());
  if (nalus.empty()) {
    return;
  }
  for (const auto& nalu : nalus) {
    if (nalu.empty()) {
      continue;
    }
    uint8_t nal_type = nalu[0] & 0x1F;
    if (nal_type == 7) {
      sps_ = nalu;
    } else if (nal_type == 8) {
      pps_ = nalu;
    }
  }
  std::vector<uint8_t> converted = ConvertToLengthPrefixed(nalus);
  uint64_t pts = static_cast<uint64_t>(image.RtpTimestamp());
  sample_pts_.push_back(pts);
  bool keyframe = image._frameType == webrtc::VideoFrameType::kVideoFrameKey;
  writer_->WriteSample(converted.data(), converted.size(), pts, keyframe);
}

void RemoteMediaRecorder::FinalizeLocked() {
  if (closed_) {
    return;
  }
  closed_ = true;
  if (!writer_initialized_ || sample_pts_.empty() || !writer_) {
    return;
  }
  std::vector<uint32_t> durations;
  for (size_t i = 1; i < sample_pts_.size(); ++i) {
    uint64_t delta = sample_pts_[i] - sample_pts_[i - 1];
    durations.push_back(static_cast<uint32_t>(delta));
  }
  if (!durations.empty()) {
    durations.push_back(durations.back());
  } else {
    durations.push_back(timescale_ / target_fps_);
  }
  if (sps_.empty() || pps_.empty()) {
    RTC_LOG(LS_WARNING) << "Missing SPS/PPS when finalizing MP4.";
  }
  writer_->Finalize(width_, height_, sps_, pps_, durations);
  encoder_.reset();
  writer_.reset();
}

webrtc::EncodedImageCallback::Result
RemoteMediaRecorder::EncoderCallback::OnEncodedImage(
    const webrtc::EncodedImage& encoded_image,
    const webrtc::CodecSpecificInfo* codec_specific_info) {
  RemoteMediaRecorder* owner = owner_;
  if (!owner) {
    return webrtc::EncodedImageCallback::Result(
        webrtc::EncodedImageCallback::Result::OK);
  }
  webrtc::MutexLock l(&owner->lock_);
  if (owner->closed_) {
    return webrtc::EncodedImageCallback::Result(
        webrtc::EncodedImageCallback::Result::OK);
  }
  owner->HandleEncodedImage(encoded_image);
  return webrtc::EncodedImageCallback::Result(
      webrtc::EncodedImageCallback::Result::OK);
}

std::unique_ptr<RemoteMediaRecorder> CreateRemoteRecorder(
    const std::string& output_path,
    uint32_t timescale) {
  FILE* file = fopen(output_path.c_str(), "wb");
  if (!file) {
    RTC_LOG(LS_ERROR) << "Failed to open output file for recorder: "
                      << output_path;
    return nullptr;
  }
  auto writer = std::make_unique<Mp4FileWriter>(file, timescale);
  auto encoder_factory = webrtc::CreateBuiltinVideoEncoderFactory();
  webrtc::Environment env = webrtc::CreateEnvironment();
  webrtc::SdpVideoFormat format("H264");
  std::unique_ptr<webrtc::VideoEncoder> encoder =
      encoder_factory->Create(env, format);
  if (!encoder) {
    RTC_LOG(LS_ERROR) << "No H264 encoder available for recorder.";
    return nullptr;
  }
  return std::make_unique<RemoteMediaRecorder>(std::move(encoder),
                                              std::move(writer), timescale);
}

}  // namespace webrtc_example
