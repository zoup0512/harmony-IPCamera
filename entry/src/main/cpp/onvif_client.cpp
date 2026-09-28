#include "onvif_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <hilog/log.h>

#define LOG_DOMAIN 0xC010
#define LOG_TAG "OnvifClient"

namespace ipcam {
namespace {

constexpr const char* kProbeTemplate =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
    "xmlns:a=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\">"
    "<s:Header>"
    "<a:Action s:mustUnderstand=\"1\">http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</a:Action>"
    "<a:MessageID>urn:uuid:6a2f0f%s-0000-1000-8000-%06llx</a:MessageID>"
    "<a:To s:mustUnderstand=\"1\">urn:schemas-xmlsoap-org:ws:2005:04:discovery:2004/01</a:To>"
    "</s:Header>"
    "<s:Body>"
    "<Probe xmlns=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\">"
    "<Types xmlns:dp0=\"http://www.onvif.org/ver10/network/wsdl\">dp0:NetworkVideoTransmitter</Types>"
    "</Probe>"
    "</s:Body>"
    "</s:Envelope>";

const char* kProfilesTemplate =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
    "<s:Body>"
    "<GetProfiles xmlns=\"http://www.onvif.org/ver10/media/wsdl\"/>"
    "</s:Body>"
    "</s:Envelope>";

const char* kStreamUriTemplate =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
    "<s:Body>"
    "<GetStreamUri xmlns=\"http://www.onvif.org/ver10/media/wsdl\">"
    "<StreamSetup><Stream xmlns=\"http://www.onvif.org/ver10/schema\">RTP-Unicast</Stream>"
    "<Transport xmlns=\"http://www.onvif.org/ver10/schema\"><Protocol>RTSP</Protocol></Transport>"
    "</StreamSetup><ProfileToken>%s</ProfileToken>"
    "</GetStreamUri>"
    "</s:Body>"
    "</s:Envelope>";

std::string XmlEscape(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (char c : in) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default: out += c;
    }
  }
  return out;
}

}  // namespace

std::string OnvifClient::ExtractTag(const std::string& xml, const std::string& tag) {
  // matches <tag>, <tag attr=...>, </tag>; returns inner text of first pair
  std::string open = "<" + tag;
  size_t pos = 0;
  while (pos < xml.size()) {
    size_t start = xml.find(open, pos);
    if (start == std::string::npos) return std::string();
    size_t afterOpen = start + open.size();
    size_t gt = xml.find('>', afterOpen);
    if (gt == std::string::npos) return std::string();
    if (afterOpen < xml.size() && xml[afterOpen] == '/') {  // self-closing <tag/>
      pos = gt + 1;
      continue;
    }
    size_t end = xml.find("</" + tag + ">", gt);
    if (end == std::string::npos) return std::string();
    return xml.substr(gt + 1, end - gt - 1);
  }
  return std::string();
}

std::vector<std::string> OnvifClient::ExtractAll(const std::string& xml,
                                                 const std::string& tag) {
  std::vector<std::string> out;
  std::string open = "<" + tag;
  size_t pos = 0;
  while (pos < xml.size()) {
    size_t start = xml.find(open, pos);
    if (start == std::string::npos) break;
    size_t afterOpen = start + open.size();
    size_t gt = xml.find('>', afterOpen);
    if (gt == std::string::npos) break;
    if (xml.compare(afterOpen, 1, "/") == 0) {  // self-closing
      pos = gt + 1;
      out.push_back(std::string());
      continue;
    }
    size_t end = xml.find("</" + tag + ">", gt);
    if (end == std::string::npos) break;
    out.push_back(xml.substr(gt + 1, end - gt - 1));
    pos = end + tag.size() + 3;
  }
  return out;
}

bool OnvifClient::SoapPost(const std::string& url, const std::string& action,
                           const std::string& body, int timeoutMs,
                           std::string* response, std::string* error) {
  // parse http://host[:port]/path
  if (url.rfind("http://", 0) != 0) {
    *error = "unsupported url";
    return false;
  }
  std::string rest = url.substr(7);
  size_t slash = rest.find('/');
  std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
  std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
  size_t colon = hostport.rfind(':');
  std::string host = colon == std::string::npos ? hostport : hostport.substr(0, colon);
  int port = colon == std::string::npos ? 80 : atoi(hostport.substr(colon + 1).c_str());

  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    *error = "socket";
    return false;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    struct hostent* he = ::gethostbyname(host.c_str());
    if (he == nullptr || he->h_addr_list[0] == nullptr) {
      ::close(fd);
      *error = "resolve " + host;
      return false;
    }
    memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(in_addr));
  }
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    *error = "connect " + hostport;
    return false;
  }

  std::string contentType = "application/soap+xml; charset=utf-8";
  if (!action.empty()) {
    contentType += "; action=\"" + action + "\"";
  }
  std::string req = "POST " + path + " HTTP/1.1\r\nHost: " + hostport +
                    "\r\nContent-Type: " + contentType +
                    "\r\nContent-Length: " + std::to_string(body.size()) +
                    "\r\nConnection: close\r\n\r\n" + body;

  // send + receive with deadline
  auto deadline = [](int ms) {
    return ms;
  };
  (void)deadline;
  struct timeval tv {};
  tv.tv_sec = timeoutMs / 1000;
  tv.tv_usec = (timeoutMs % 1000) * 1000;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  size_t sent = 0;
  while (sent < req.size()) {
    ssize_t n = ::send(fd, req.data() + sent, req.size() - sent, MSG_NOSIGNAL);
    if (n <= 0) {
      ::close(fd);
      *error = "send";
      return false;
    }
    sent += static_cast<size_t>(n);
  }
  std::string raw;
  char buf[4096];
  for (;;) {
    ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) break;
    raw.append(buf, static_cast<size_t>(n));
    if (raw.size() > (1u << 20)) break;
  }
  ::close(fd);
  size_t headerEnd = raw.find("\r\n\r\n");
  if (headerEnd == std::string::npos) {
    *error = "no http header";
    return false;
  }
  std::string statusLine = raw.substr(0, raw.find("\r\n"));
  if (statusLine.find("200") == std::string::npos) {
    *error = "http " + statusLine.substr(0, statusLine.find("\r"));
    return false;
  }
  *response = raw.substr(headerEnd + 4);
  return true;
}

std::vector<OnvifClient::Device> OnvifClient::Discover(int timeoutMs, int settleMs) {
  std::vector<Device> devices;
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "probe socket failed");
    return devices;
  }
  int reuse = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  ip_mreq mreq{};
  mreq.imr_multiaddr.s_addr = inet_addr("239.255.255.250");
  mreq.imr_interface.s_addr = htonl(INADDR_ANY);
  if (::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
    OH_LOG_Print(LOG_APP, LOG_WARN, LOG_DOMAIN, LOG_TAG,
                 "multicast join failed err=%{public}d (scan may not receive)", errno);
  }

  sockaddr_in dst{};
  dst.sin_family = AF_INET;
  dst.sin_port = htons(3702);
  dst.sin_addr.s_addr = inet_addr("239.255.255.250");
  char nonce[32];
  snprintf(nonce, sizeof(nonce), "%04x%04x", static_cast<unsigned>(rand() & 0xFFFF),
           static_cast<unsigned>(rand() & 0xFFFF));
  char body[2048];
  snprintf(body, sizeof(body), kProbeTemplate, nonce,
           static_cast<unsigned long long>(time(nullptr) & 0xFFFFFF));
  if (::sendto(fd, body, strlen(body), 0, reinterpret_cast<sockaddr*>(&dst),
               sizeof(dst)) < 0) {
    OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, "probe send err=%{public}d", errno);
    ::close(fd);
    return devices;
  }
  // Some devices answer slowly; keep listening for the full window.
  const int64_t totalMs = timeoutMs + settleMs;
  int64_t listened = 0;
  char rbuf[8192];
  std::map<std::string, size_t> byAddress;
  while (listened < totalMs) {
    pollfd p{fd, POLLIN, 0};
    int wait = static_cast<int>(std::min<int64_t>(500, totalMs - listened));
    int r = ::poll(&p, 1, wait);
    listened += wait;
    if (r <= 0) continue;
    ssize_t n = ::recv(fd, rbuf, sizeof(rbuf) - 1, 0);
    if (n <= 0) continue;
    rbuf[n] = 0;
    std::string xml(rbuf, static_cast<size_t>(n));
    std::string addr = ExtractTag(xml, "a:Address");
    std::string xaddrs = ExtractTag(xml, "d:XAddrs");
    std::string scopes = ExtractTag(xml, "d:Scopes");
    if (addr.empty() || xaddrs.empty()) continue;
    // one XAddrs entry (first space-separated)
    size_t sp = xaddrs.find(' ');
    if (sp != std::string::npos) xaddrs = xaddrs.substr(0, sp);

    Device dev;
    dev.address = addr;
    dev.xaddrs = xaddrs;
    dev.scopes = scopes;
    // Model from scopes: onvif://www.onvif.org/Model/<name>
    size_t m = scopes.find("onvif://www.onvif.org/Model/");
    if (m != std::string::npos) {
      size_t begin = m + strlen("onvif://www.onvif.org/Model/");
      size_t end = scopes.find(' ', begin);
      dev.model = scopes.substr(begin, end == std::string::npos ? std::string::npos
                                                                : end - begin);
    }
    if (dev.model.empty()) {
      size_t h = scopes.find("onvif://www.onvif.org/name/");
      if (h != std::string::npos) {
        size_t begin = h + strlen("onvif://www.onvif.org/name/");
        size_t end = scopes.find(' ', begin);
        dev.model = scopes.substr(begin, end == std::string::npos ? std::string::npos
                                                                  : end - begin);
      }
    }
    auto it = byAddress.find(addr);
    if (it == byAddress.end()) {
      byAddress[addr] = devices.size();
      devices.push_back(dev);
      OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG,
                   "discovered %{public}s model=%{public}s", xaddrs.c_str(),
                   dev.model.c_str());
    }
  }
  ::close(fd);
  return devices;
}

bool OnvifClient::ResolveMedia(Device* device) {
  if (device == nullptr) return false;
  std::string resp;
  std::string err;
  if (!SoapPost(device->xaddrs, "http://www.onvif.org/ver10/media/wsdl/GetProfiles",
                kProfilesTemplate, 3000, &resp, &err)) {
    device->lastError = "GetProfiles: " + err;
    return false;
  }
  // SOAP 1.2 fault?
  if (resp.find("s:Fault") != std::string::npos || resp.find("soapenv:Fault") != std::string::npos) {
    device->lastError = "GetProfiles fault";
    return false;
  }
  std::string token = ExtractTag(resp, "token");
  if (token.empty()) {
    // attributes variant: <trt:Profile fixed="true" token="profile_1">
    size_t t = resp.find("token=\"");
    if (t != std::string::npos) {
      size_t e = resp.find('"', t + 7);
      if (e != std::string::npos) token = resp.substr(t + 7, e - t - 7);
    }
  }
  if (token.empty()) {
    device->lastError = "no profile token";
    return false;
  }
  device->profile = token;

  char body[1024];
  snprintf(body, sizeof(body), kStreamUriTemplate, XmlEscape(token).c_str());
  std::string uriResp;
  if (!SoapPost(device->xaddrs, "http://www.onvif.org/ver10/media/wsdl/GetStreamUri",
                body, 3000, &uriResp, &err)) {
    device->lastError = "GetStreamUri: " + err;
    return false;
  }
  std::string uri = ExtractTag(uriResp, "tt:Uri");
  if (uri.empty()) {
    uri = ExtractTag(uriResp, "Uri");
  }
  device->streamUri = uri;
  return !uri.empty();
}

}  // namespace ipcam
