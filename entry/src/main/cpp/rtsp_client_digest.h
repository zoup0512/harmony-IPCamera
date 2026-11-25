#pragma once
// Per-connection RTSP Digest client. No credential discovery, Basic fallback or nonce refresh.
#include "rtsp_client_protocol.h"
#include <map>
#include <string_view>

namespace rtspdigest {
struct Credentials {
  std::string user, password;
  bool valid() const {
    auto safe=[](const std::string& s) {
      return !s.empty() && s.size()<=4096 && std::none_of(s.begin(),s.end(),[](unsigned char c){return c<32 || c==127;});
    };
    return safe(user) && user.find(':')==user.npos && safe(password);
  }
};
struct Challenge {
  std::string realm, nonce, opaque;
  bool sha256=false, qop=false, hasOpaque=false;
};
inline bool tchar(char c) {
  return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') ||
         std::string_view("!#$%&'*+-.^_`|~").find(c)!=std::string_view::npos;
}
// Read complete challenges, not substrings: commas in quoted strings and repeated
// WWW-Authenticate fields are both legal. Never reinterpret a malformed offer as MD5.
inline bool choose(const std::string& s, Challenge& best) {
  if (s.size()>8192) return false;
  size_t i=0;
  auto space=[&]{while(i<s.size() && (s[i]==' ' || s[i]=='\t')) ++i;};
  auto word=[&]{auto start=i; while(i<s.size() && tchar(s[i])) ++i; return s.substr(start,i-start);};
  bool found=false;
  space();
  while (i<s.size()) {
    auto scheme=liveavc::lower(word());
    if (scheme.empty() || i==s.size() || (s[i]!=' ' && s[i]!='\t')) return false;
    space(); std::map<std::string,std::string> p;
    for (;;) {
      auto key=liveavc::lower(word()); space();
      if (key.empty() || i==s.size() || s[i++]!='=') return false;
      space(); std::string value;
      if (i<s.size() && s[i]=='"') {
        ++i;
        while (i<s.size() && s[i]!='"') {
          if (s[i]=='\\' && ++i==s.size()) return false;
          unsigned char c=s[i++]; if (c<32 || c==127) return false;
          value+=static_cast<char>(c);
        }
        if (i==s.size()) return false;
        ++i;
      } else if ((value=word()).empty()) return false;
      if (!p.emplace(key,value).second) return false;
      space(); if (i==s.size()) break;
      if (s[i++]!=',') return false;
      space(); auto start=i;
      if (word().empty()) return false;
      space(); bool param=i<s.size() && s[i]=='='; i=start;
      if (!param) break;
    }
    if (scheme!="digest") continue;
    Challenge c; c.realm=p["realm"]; c.nonce=p["nonce"];
    if (c.realm.empty() || c.nonce.empty()) return false;
    auto alg=p.count("algorithm") ? liveavc::lower(p["algorithm"]) : "md5";
    if (alg!="md5" && alg!="sha-256") return false;
    c.sha256=alg=="sha-256";
    if ((p.count("charset") && liveavc::lower(p["charset"])!="utf-8") ||
        (p.count("userhash") && liveavc::lower(p["userhash"])!="false")) return false;
    if (p.count("qop")) {
      std::istringstream list(p["qop"]); std::string q;
      while (std::getline(list,q,',')) if (liveavc::lower(liveavc::trim(q))=="auth") c.qop=true;
      if (!c.qop) return false; // no auth-int or fallback to RFC 2069
    }
    c.hasOpaque=p.count("opaque")!=0; c.opaque=p["opaque"];
    if (!found || std::make_pair(c.sha256,c.qop)>std::make_pair(best.sha256,best.qop)) best=std::move(c);
    found=true;
  }
  return found;
}
inline std::string quote(const std::string& value) {
  std::string out="\"";
  for(char c:value){if(c=='"' || c=='\\')out+='\\';out+=c;}
  return out+'"';
}
inline std::string hex(const uint8_t* data,size_t n) {
  const char digits[]="0123456789abcdef"; std::string out;
  for(size_t i=0;i<n;++i){out+=digits[data[i]>>4];out+=digits[data[i]&15];}
  return out;
}
// Production implementations use CryptoArchitectureKit. Empty result means failure.
std::string hash(bool sha256,const std::string& input);
std::string randomNonce();
enum class Action { Continue, Repeat, Denied, Unsupported };
class Client {
  std::string origin_, user_, ha1_, cnonce_;
  Challenge challenge_;
  uint32_t count_=0;
 public:
  explicit Client(std::string origin):origin_(std::move(origin)) {}
  bool active() const {return !ha1_.empty();}
  Action onResponse(int code,const std::string& uri,const std::string& field,const Credentials& credentials) {
    if (origin_.empty() || liveavc::origin(uri)!=origin_ || code==403) return Action::Denied;
    if (code!=401) return Action::Continue;
    if (active() || !credentials.valid() || field.empty()) return Action::Denied;
    if (!choose(field,challenge_) || (challenge_.qop && (cnonce_=randomNonce()).empty())) return Action::Unsupported;
    user_=credentials.user;
    ha1_=hash(challenge_.sha256,user_+":"+challenge_.realm+":"+credentials.password);
    return active()?Action::Repeat:Action::Unsupported;
  }
  std::string authorization(const std::string& method,const std::string& uri) {
    if (!active() || liveavc::origin(uri)!=origin_ || count_==UINT32_MAX) return {};
    const auto& c=challenge_;
    auto ha2=hash(c.sha256,method+":"+uri); if (ha2.empty()) return {};
    ++count_; uint8_t bytes[]={uint8_t(count_>>24),uint8_t(count_>>16),uint8_t(count_>>8),uint8_t(count_)};
    auto nc=hex(bytes,4);
    auto response=hash(c.sha256,ha1_+":"+c.nonce+":"+(c.qop?nc+":"+cnonce_+":auth:":"")+ha2);
    if (response.empty()) return {};
    return "Digest username="+quote(user_)+", realm="+quote(c.realm)+", nonce="+quote(c.nonce)+
      ", uri="+quote(uri)+", algorithm="+(c.sha256?"SHA-256":"MD5")+", response="+quote(response)+
      (c.hasOpaque?", opaque="+quote(c.opaque):"")+(c.qop?", qop=auth, nc="+nc+", cnonce="+quote(cnonce_):"");
  }
};
} // namespace rtspdigest
