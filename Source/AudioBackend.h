#pragma once

#include <QtCore/QList>
#include <QtCore/QString>

#include <cstddef>
#include <functional>
#include <memory>

namespace AudioBridge
{
    struct DeviceInfo
    {
        QString Id;
        QString Name;
    };

    // Audio crosses this interface as interleaved stereo float at the rate given with each call.
    // Backends mix up or down to two channels and leave resampling to the receiving Playout.
    using CaptureCallback = std::function<void(const float* frames, std::size_t count, int rate)>;
    using RenderCallback = std::function<void(float* frames, std::size_t count, int rate)>;

    // Runs from construction to destruction. A stream that could not start says why in Error
    // and produces nothing; callbacks arrive on the backend's real-time thread.
    class AudioStream
    {
    public:
        virtual ~AudioStream() = default;
        [[nodiscard]] virtual QString Error() const = 0;
    };

    // Everything this machine plays except AudioBridge's own output, so two machines sending
    // each other their system audio do not feed back.
    [[nodiscard]] std::unique_ptr<AudioStream> StartSystemCapture(CaptureCallback callback);
    [[nodiscard]] std::unique_ptr<AudioStream> StartDeviceCapture(const QString& deviceId,
        CaptureCallback callback);
    // An empty device id means the system default output.
    [[nodiscard]] std::unique_ptr<AudioStream> StartRender(const QString& deviceId,
        RenderCallback callback);

    [[nodiscard]] QList<DeviceInfo> InputDevices();
    [[nodiscard]] QList<DeviceInfo> OutputDevices();
    // Polled to notice the system default changing, which neither OS hands to a running stream.
    [[nodiscard]] QString DefaultOutputId();
} // namespace AudioBridge
