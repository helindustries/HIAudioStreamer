#pragma once

#include <cstddef>
#include <cstdint>

namespace AudioBridge
{
    static constexpr auto DefaultPort = std::uint16_t{49710};
    static constexpr auto Magic = std::uint32_t{0x31474241};
    static constexpr auto Channels = 2;
    static constexpr auto MaxPacketFrames = 480;
    static constexpr auto MaxPeers = 8;
    static constexpr auto MaxNameBytes = 64;

    static constexpr auto KeySize = 32;
    static constexpr auto KeyIdSize = 8;
    static constexpr auto NonceSize = 24;
    static constexpr auto SaltSize = 16;
    static constexpr auto MacSize = 16;

    // Every datagram starts with Magic (4 bytes, little-endian) and one of these. The layouts
    // that follow are written out where each message is built, in Network.cpp.
    enum class MessageType : std::uint8_t
    {
        PairRequest = 1,
        PairResponse = 2,
        PairConfirm = 3,
        Audio = 4,
        // "I am running and will play what you send", twice a second to every peer. The sender
        // silences its own output only while one of these keeps arriving.
        Listening = 5,
    };
    static constexpr auto HeartbeatMs = 500;
    static constexpr auto ListeningTimeoutMs = 2000;
} // namespace AudioBridge
