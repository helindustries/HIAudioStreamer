# HIAudioStreamer

HIAudioStreamer streams audio between Windows and macOS computers on a local network. It appears as a tray icon on Windows and a menu bar item on macOS, using encrypted UDP for transmission. All machines can send and receive simultaneously; audio from a paired machine is played as it arrives.

## Features

- **System audio without a virtual cable.** Capturing system audio is achieved via process loopback on Windows and a Core Audio process tap on macOS. HIAudioStreamer excludes its own playback from the capture, allowing two machines to exchange system audio without feedback. Any input device can also be selected for transmission.
- **Manual connections.** Machines are added by IP address and are remembered across restarts. There is no discovery mechanism and no server.
- **Pairing with a 4-digit code.** Machines exchange X25519 keys; one displays a code derived from the shared key while the other enters it, preventing man-in-the-middle attacks. Subsequently, every packet is encrypted and authenticated with XChaCha20-Poly1305, and packets from unpaired machines are discarded.
- **Local output follows the stream.** While a paired machine streams this machine's audio, the local speakers are muted. They resume when the remote machine stops listening, when HIAudioStreamer quits, or upon the next startup if the application was terminated.
- **Buffering.** A jitter buffer of 20 to 150 ms is used per connection. Playback resamples to the output device and adheres to the sender's clock, maintaining a stable buffer level.
- **Follows the system default output**, and can start at login.

## Requirements

- Windows 11 (or Windows Server 2022) on x64. Capturing system audio requires build 20348 or later.
- macOS 14.2 or later, supporting both Apple Silicon and Intel.
- UDP port 49710 must be reachable between the machines.

## Installing

Download the installer for each machine from the [releases page](https://github.com/helindustries/HIAudioStreamer/releases).

**Windows:** run `HIAudioStreamer-<version>-windows-x64-setup.exe`. It requests administrator rights to create a firewall rule permitting other machines to reach HIAudioStreamer on private and domain networks. On networks classified as public by Windows, no connections will be established; switch such networks to private. The installer is not yet code-signed, so SmartScreen may display a warning (More info, Run anyway).

**macOS:** open `HIAudioStreamer-<version>-macos.dmg` and drag the app to Applications. The app is signed ad hoc but not notarized, blocking the first launch: open System Settings, Privacy & Security, and select Open Anyway. macOS then requests two permissions:

- **Local Network**, required for sending or receiving any data.
- **System Audio Recording**, required the first time this Mac sends its system audio.

Due to the ad hoc signature, macOS will request permission again after every update.

## Using it

1. Start HIAudioStreamer on both machines.
2. On one machine, select **Connections > Add Connection…** and enter the other machine's IP address, optionally including `:port`.
3. The second machine displays a 4-digit code. Enter this code on the first machine. Both machines will remember the pairing subsequently.

The remaining menu options:

| Item | What it does |
|---|---|
| Send | Sends this machine's audio to every connection with sending enabled |
| Source | System Audio, or one of the input devices |
| Output | Destination for received audio; System Default follows the system setting |
| Buffer | The jitter buffer per connection |
| Connections | Per machine: Send to it, or Remove it |
| Start at Login | Starts HIAudioStreamer upon login |

## Settings and logs

| | Windows | macOS |
|---|---|---|
| Settings | `HKCU\Software\Hel Industries\HIAudioStreamer` | `~/Library/Preferences/com.helindustries.HIAudioStreamer.plist` |
| Log | `%LOCALAPPDATA%\Hel Industries\HIAudioStreamer\HIAudioStreamer.log` | `~/Library/Application Support/Hel Industries/HIAudioStreamer/HIAudioStreamer.log` |

The UDP port is configured via the `Port` setting; it has no menu entry. The log records counter lines every ten seconds, which tell "nothing arrives" apart from "arrives but is rejected" and "arrives but does not play".

## Building

Qt 6 (Widgets and Network), CMake 3.21 or later, and Ninja are required. The crypto library, Monocypher, is vendored in `Source/ThirdParty`.

**macOS**, creating a universal app and disk image in `../Build/Mac`:

```
QT_ROOT_DIR=/path/to/Qt/6.9.3/macos Tools/Build.sh
```

**Windows**, using Visual Studio 2022 (x64 C++ tools) and Inno Setup 6, creating the installer in `..\Build\Windows`:

```
set QT_ROOT_DIR=C:\path\to\Qt\6.9.3\msvc2022_64
Tools\Build.cmd
```

Both scripts accept a different build directory as an argument. GitHub Actions executes them on every push; pushing a `v*` tag publishes the installer and disk image as a release.

## Licence

MIT, see [LICENSE](LICENSE). The binaries include Qt, linked dynamically under the LGPL 3.0, and Monocypher under BSD 2-Clause or CC0; their licences are located in [Documentation/Licenses](Documentation/Licenses) and included with both packages.
