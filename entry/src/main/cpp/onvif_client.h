#ifndef IPCAMERA_ONVIF_CLIENT_H
#define IPCAMERA_ONVIF_CLIENT_H

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ipcam {

// Minimal ONVIF device discovery + media profile client, mirroring the
// Android OnvifScannerActivity behaviour:
//   1. WS-Discovery Probe over UDP multicast (239.255.255.250:3702)
//   2. GetProfiles (SOAP over HTTP) on each discovered device service
//   3. GetStreamUri for the first video profile -> rtsp://...
// No XML library: responses are parsed with simple tag extraction, which is
// sufficient for the well-known ONVIF envelopes.
class OnvifClient {
 public:
  struct Device {
    std::string address;   // WS-Discovery endpoint reference (urn:uuid)
    std::string xaddrs;    // http://ip:port/onvif/device_service
    std::string scopes;    // raw scopes string (contains Model/Hardware)
    std::string model;     // extracted onvif://www.onvif.org/Model or name
    std::string profile;   // first media profile token ("" if none)
    std::string streamUri; // rtsp://... ("" if none)
    std::string lastError; // SOAP/parse error for diagnostics ("" if ok)
  };

  using ResultSink = std::function<void(std::vector<Device> devices)>;

  // Runs discovery + profile resolution synchronously (call from a worker
  // thread; the napi layer wraps this in an async work). Devices are deduped
  // by endpoint reference.
  static std::vector<Device> Discover(int timeoutMs, int settleMs);

  // GetProfiles + GetStreamUri against one device; fills profile/streamUri
  // (or lastError). Exposed separately so a UI can refresh a single device.
  static bool ResolveMedia(Device* device);

 private:
  static bool SoapPost(const std::string& url, const std::string& action,
                       const std::string& body, int timeoutMs, std::string* response,
                       std::string* error);
  static std::string ExtractTag(const std::string& xml, const std::string& tag);
  static std::vector<std::string> ExtractAll(const std::string& xml,
                                             const std::string& tag);
};

}  // namespace ipcam

#endif  // IPCAMERA_ONVIF_CLIENT_H
