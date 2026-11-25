#include "rtsp_client_digest.h"
#include <CryptoArchitectureKit/crypto_digest.h>
#include <CryptoArchitectureKit/crypto_rand.h>
#include <memory>

namespace rtspdigest {
namespace {
struct Output {
  Crypto_DataBlob blob{};
  ~Output() { if (blob.data) OH_Crypto_FreeDataBlob(&blob); }
  std::string result(size_t n) const { return blob.data && blob.len==n ? hex(blob.data,n) : ""; }
};
}
std::string hash(bool sha256,const std::string& input) {
  OH_CryptoDigest* raw=nullptr;
  if (OH_CryptoDigest_Create(sha256?"SHA256":"MD5",&raw)!=CRYPTO_SUCCESS) return {};
  std::unique_ptr<OH_CryptoDigest,decltype(&OH_DigestCrypto_Destroy)> digest(raw,OH_DigestCrypto_Destroy);
  Crypto_DataBlob in{reinterpret_cast<uint8_t*>(const_cast<char*>(input.data())),input.size()};
  Output out;
  if (OH_CryptoDigest_Update(raw,&in)!=CRYPTO_SUCCESS || OH_CryptoDigest_Final(raw,&out.blob)!=CRYPTO_SUCCESS) return {};
  return out.result(sha256?32:16);
}
std::string randomNonce() {
  OH_CryptoRand* raw=nullptr;
  if (OH_CryptoRand_Create(&raw)!=CRYPTO_SUCCESS) return {};
  std::unique_ptr<OH_CryptoRand,decltype(&OH_CryptoRand_Destroy)> rng(raw,OH_CryptoRand_Destroy);
  Output out;
  if (OH_CryptoRand_GenerateRandom(raw,16,&out.blob)!=CRYPTO_SUCCESS) return {};
  return out.result(16);
}
} // namespace rtspdigest
