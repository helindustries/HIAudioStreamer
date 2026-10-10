# AudioBridge

Streams audio between machines on the LAN: a tray icon on Windows, a menu bar item on macOS, and
UDP in between. Both sides send and receive; whatever arrives from a paired machine is played.

- **Send** captures this machine's own output (*System Audio*: process loopback on Windows, a
  Core Audio process tap on macOS 14.2+) or any input device. AudioBridge leaves its own output
  out of the capture, so two machines can send each other their system audio without feedback.
  No virtual cable is needed.
- **Connections** are entered by IP address and kept across restarts. There is no discovery.
- **Pairing** exchanges X25519 keys over UDP; one machine shows a 4-digit code derived from the
  shared key and the other types it in, which rules out a man in the middle. After that every
  audio packet is encrypted and authenticated (XChaCha20-Poly1305), and packets from anything
  unpaired are dropped.
- **Buffer** is the jitter buffer per peer. Playback resamples to the output device and adjusts
  its rate slightly to follow the sender's clock, so the buffer holds its level.

UDP port 49710 by default (`Port` in the settings). The firewall needs to let it in on both sides.

## Building

Qt 6 (Widgets, Network) and CMake. Crypto is vendored Monocypher in `Source/ThirdParty`.

```
cmake -S . -B ../Build/Mac -DCMAKE_PREFIX_PATH=/Users/Shared/Qt/6.9.3/macos
cmake --build ../Build/Mac
```

On Windows, from a Visual Studio x64 prompt, with `-DCMAKE_PREFIX_PATH` pointing at the MSVC Qt
kit, then `windeployqt` on the built `AudioBridge.exe`.
