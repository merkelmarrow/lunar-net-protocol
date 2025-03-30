// src/common/lumen_packet.cpp

#include "lumen_packet.hpp"
#include "lumen_header.hpp"

LumenPacket::LumenPacket(const LumenHeader &header,
                         const std::vector<uint8_t> &payload)
    : header_(header), payload_(payload) {}