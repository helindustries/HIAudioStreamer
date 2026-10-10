#pragma once

#include <QtCore/QString>

namespace HIAudioStreamer
{
    // HKCU Run key on Windows, a user LaunchAgent on macOS. Both point at the running binary, so
    // moving the app means toggling this off and on again. Setting it returns what went wrong,
    // or nothing.
    [[nodiscard]] bool AutostartEnabled();
    [[nodiscard]] QString SetAutostart(bool enabled);
} // namespace HIAudioStreamer
