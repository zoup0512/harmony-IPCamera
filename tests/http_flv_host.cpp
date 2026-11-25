// Synthetic source + real HTTP server. No bind(): Python passes a listener
// already bound to loopback. Only the platform audio codec is stubbed.
#include "flv_stream.h"
#include "opus_stream.h"
#include <thread>
#define private public
#include "http_server.h"
#undef private
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <sstream>

using namespace ipcam;
bool OpusStream::EnsureReady() { return false; }
bool OpusStream::Serve(int, const std::atomic<bool>&) { return false; }

static void Limits() {
  FlvStream stream;
  const uint8_t config[]{0,0,1,0x67,0x42,0,0x0a,0,0,1,0x68,0xce};
  const uint8_t idr[]{0,0,1,0x65,0x80}, p[]{0,0,1,0x41,0x80};
  const uint8_t a[]{0xff,0xf1,0x50,0x80,1,0x1f,0xfc,1};
  stream.FeedVideo(config, sizeof(config), 0);
  std::shared_ptr<FlvStream::Reader> fast, slow, c, d, extra;
  for (auto* r : {&fast, &slow, &c, &d}) assert(stream.Open(*r) == FlvStream::OpenResult::Ok);
  assert(stream.Open(extra) == FlvStream::OpenResult::Busy);
  c.reset(); d.reset();
  assert(!stream.TryBeginVideoChange());
  stream.FeedVideo(idr, sizeof(idr), 1000);
  stream.FeedAudio(a, sizeof(a), 1000);
  FlvStream::Tag tag{}; bool audio = false;
  for (int i = 0; i < 4; ++i) assert(stream.Read(fast, tag, audio) == FlvStream::ReadResult::TagReady);
  assert(audio);
  for (int i = 1; i <= 160; ++i) {
    stream.FeedVideo(p, sizeof(p), 1000 + 1000 * i);
    assert(stream.Read(fast, tag, audio) == FlvStream::ReadResult::TagReady);
    assert(tag.ms == static_cast<uint32_t>(i) && tag.body->at(0) == 0x27);
  }
  assert(stream.Read(slow, tag, audio) == FlvStream::ReadResult::Closed);
  slow.reset();
  assert(stream.Open(slow) == FlvStream::OpenResult::Ok);
  std::vector<uint8_t> big(1500000, 0x83);
  big[0] = big[1] = 0; big[2] = 1; big[3] = 0x65;
  for (int i = 0; i < 3; ++i) stream.FeedVideo(big.data(), big.size(), 500000);
  assert(stream.Read(slow, tag, audio) == FlvStream::ReadResult::Closed);
  slow.reset(); fast.reset();
  assert(stream.TryBeginVideoChange());
  assert(stream.Open(extra) == FlvStream::OpenResult::Busy);
  stream.EndVideoChange();
}

int main(int argc, char** argv) {
  if (argc != 2) return 1;
  Limits();
  auto flv = std::make_shared<FlvStream>();
  HttpServer server;
  server.SetFlvStream(flv);
  server.user_ = "user"; server.password_ = "pass";
  server.listenFd_ = std::atoi(argv[1]);
  server.running_ = true;
  server.acceptThread_ = std::thread(&HttpServer::AcceptLoop, &server);
  std::cout << "ready" << std::endl;
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream in(line); std::string cmd;
    in >> cmd;
    if (cmd == "count") { std::cout << flv->ViewerCount() << std::endl; continue; }
    if (cmd == "begin") { std::cout << flv->TryBeginVideoChange() << std::endl; continue; }
    if (cmd == "stop") server.Stop();
    else if (cmd == "end") flv->EndVideoChange();
    else if (cmd == "hevc" || cmd == "avc") flv->ResetVideoFormat(cmd == "hevc");
    else if (cmd == "large") {
      TimestampUs ts; in >> ts;
      // Valid user_data_unregistered SEI followed by a synthetic P picture.
      // Pad the SEI rather than making the camera or decoder do extra work.
      std::vector<uint8_t> au{0,0,1,6,5};
      constexpr int size = 1024 * 1024;
      au.insert(au.end(), size / 255, 255); au.push_back(size % 255);
      au.insert(au.end(), size, 0x88); au.push_back(0x80);
      au.insert(au.end(), {0,0,1,0x41,0xe2,0x2a});
      flv->FeedVideo(au.data(), au.size(), ts);
    }
    else if (cmd == "v" || cmd == "a") {
      TimestampUs ts; std::string hex; in >> ts >> hex;
      std::vector<uint8_t> data;
      for (size_t i = 0; i < hex.size(); i += 2) data.push_back(uint8_t(std::stoi(hex.substr(i, 2), nullptr, 16)));
      if (cmd == "v") flv->FeedVideo(data.data(), data.size(), ts);
      else flv->FeedAudio(data.data(), data.size(), ts);
    } else return 2;
    std::cout << "ok" << std::endl;
  }
  server.Stop();
}
