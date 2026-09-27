#ifndef IPCAMERA_CODEC_RECORDS_H
#define IPCAMERA_CODEC_RECORDS_H

// Shared builders for MP4/enhanced-RTMP codec configuration records and small
// AAC helpers. Operates on NALUs WITHOUT start codes.

#include <cstdint>
#include <string>
#include <vector>

namespace ipcam {

inline void PutLen2(std::vector<uint8_t>& v, size_t len) {
  v.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
  v.push_back(static_cast<uint8_t>(len & 0xFF));
}

// AVCDecoderConfigurationRecord (avcC) from H.264 SPS/PPS.
inline std::vector<uint8_t> AvcDecoderConfigRecord(const std::vector<uint8_t>& sps,
                                                   const std::vector<uint8_t>& pps) {
  std::vector<uint8_t> r;
  r.push_back(0x01);
  r.push_back(sps.size() > 1 ? sps[1] : 0x64);
  r.push_back(sps.size() > 2 ? sps[2] : 0x00);
  r.push_back(sps.size() > 3 ? sps[3] : 0x1F);
  r.push_back(0xFF);  // NAL length size - 1 = 3
  r.push_back(0xE1);  // one SPS
  PutLen2(r, sps.size());
  r.insert(r.end(), sps.begin(), sps.end());
  r.push_back(0x01);  // one PPS
  PutLen2(r, pps.size());
  r.insert(r.end(), pps.begin(), pps.end());
  return r;
}

// HEVCDecoderConfigurationRecord (hvcC) from H.265 VPS/SPS/PPS. Profile fields
// are copied from the SPS profile_tier_level bytes.
inline std::vector<uint8_t> HevcDecoderConfigRecord(const std::vector<uint8_t>& vps,
                                                    const std::vector<uint8_t>& sps,
                                                    const std::vector<uint8_t>& pps) {
  std::vector<uint8_t> r;
  r.push_back(0x01);  // configurationVersion
  if (sps.size() > 13) {
    r.push_back(sps[2]);                                   // space/tier/profile_idc
    r.insert(r.end(), sps.begin() + 3, sps.begin() + 7);   // compat flags
    r.insert(r.end(), sps.begin() + 7, sps.begin() + 13);  // constraint flags
    r.push_back(sps[13]);                                  // level_idc
  } else {
    r.insert(r.end(), 12, 0x01);
  }
  r.push_back(0xF0);  // reserved + min_spatial_segmentation_idc hi
  r.push_back(0x00);
  r.push_back(0xFC);  // reserved + parallelismType=0
  r.push_back(0xFD);  // reserved(6) + chromaFormat(2)=1 (4:2:0)
  r.push_back(0xE0);  // reserved(5) + bitDepthLumaMinus8=0
  r.push_back(0xE0);  // reserved(5) + bitDepthChromaMinus8=0
  r.push_back(0x00);  // avgFrameRate hi
  r.push_back(0x00);
  r.push_back(0x1F);  // constantFrameRate=0 numTemporalLayers=1 temporalIdNested=1 lengthSizeMinusOne=3
  r.push_back(0x03);  // numOfArrays
  auto pushArray = [&r](uint8_t nalType, const std::vector<uint8_t>& nal) {
    // array_completeness(1)=1 reserved(1)=0 NAL_unit_type(6)
    r.push_back(static_cast<uint8_t>(0x80 | nalType));
    r.push_back(0x00);
    r.push_back(0x01);
    PutLen2(r, nal.size());
    r.insert(r.end(), nal.begin(), nal.end());
  };
  pushArray(32, vps);
  pushArray(33, sps);
  pushArray(34, pps);
  return r;
}

// 2-byte AudioSpecificConfig from ADTS fields.
inline std::vector<uint8_t> AacConfigBytes(int profileBits, int sfIndex, int channels) {
  int aot = profileBits + 1;
  std::vector<uint8_t> r;
  r.push_back(static_cast<uint8_t>((aot << 3) | (sfIndex >> 1)));
  r.push_back(static_cast<uint8_t>(((sfIndex & 1) << 7) | (channels << 3)));
  return r;
}

struct AdtsInfo {
  int profileBits = 1;
  int sfIndex = 4;
  int channels = 2;
  int sampleRate = 44100;
  int headerLen = 7;
  int frameLen = 0;
};

inline bool ParseAdts(const uint8_t* d, size_t len, AdtsInfo* out) {
  static const int kRates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                 22050, 16000, 12000, 11025, 8000,  7350};
  if (d == nullptr || len < 7) return false;
  if (d[0] != 0xFF || (d[1] & 0xF6) != 0xF0) return false;
  out->profileBits = (d[2] >> 6) & 0x03;
  out->sfIndex = (d[2] >> 2) & 0x0F;
  out->channels = ((d[2] & 0x01) << 2) | ((d[3] >> 6) & 0x03);
  out->frameLen = ((d[3] & 0x03) << 11) | (d[4] << 3) | ((d[5] >> 5) & 0x07);
  out->headerLen = (d[1] & 0x01) ? 7 : 9;
  if (out->sfIndex >= 13) return false;
  out->sampleRate = kRates[out->sfIndex];
  if (out->sampleRate == 0 || out->channels == 0) return false;
  if (out->frameLen < out->headerLen + 1 || out->frameLen > 8192) return false;
  return true;
}

}  // namespace ipcam

#endif  // IPCAMERA_CODEC_RECORDS_H
