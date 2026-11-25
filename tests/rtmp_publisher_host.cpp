#include "rtmp_publisher.h"
#include <iostream>

// Driven by a loopback peer. No camera, platform logging, or external server.
int main(int argc, char** argv) {
  if (argc != 2) return 2;
  ipcam::RtmpPublisher p;
  if (!p.Start(argv[1], 1280, 720, false, nullptr)) {
    std::cout << p.LastError() << std::endl;
    return 0;
  }
  const uint8_t video[] = {0,0,1,0x67,0x42,0,0x1f,0,0,1,0x68,1,0,0,1,0x65,9};
  const uint8_t audio[] = {0xff,0xf1,0x4c,0x40,1,0x1f,0xfc,0xac};
  p.SendH264Packet(video, sizeof(video), 1000000);
  p.SendAdtsPacket(audio, sizeof(audio), 1000000);
  std::cout << "publishing" << std::endl;
  std::string line;
  std::getline(std::cin, line);
  p.Stop();
  std::cout << p.LastError() << std::endl;
}
