#pragma once

namespace HIAudioStreamer
{
    // HKCU Run key on Windows, a user LaunchAgent on macOS. Both point at the running binary, so
    // moving the app means toggling this off and on again.
    [[nodiscard]] bool AutostartEnabled();
    void SetAutostart(bool enabled);
} // namespace HIAudioStreamer
