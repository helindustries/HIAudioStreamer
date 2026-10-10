#include "AudioBackend.h"

#include <QtCore/QString>

#import <CoreAudio/AudioHardware.h>
#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <cstddef>
#include <unistd.h>
#include <vector>

namespace
{
    constexpr auto MaxFrames = std::size_t{8192};

    struct ChannelRef
    {
        float* Data = nullptr;
        UInt32 Stride = 0;
        std::size_t Frames = 0;
    };

    // Owns whatever a stream set up and tears it down in reverse. The IOProc is stopped before
    // anything it reads from goes away.
    class CoreAudioStream final : public AudioBridge::AudioStream
    {
    public:
        ~CoreAudioStream() override
        {
            if (Proc != nullptr)
            {
                AudioDeviceStop(Device, Proc);
                AudioDeviceDestroyIOProcID(Device, Proc);
            }
            if (Aggregate != kAudioObjectUnknown) AudioHardwareDestroyAggregateDevice(Aggregate);
            if (Tap != kAudioObjectUnknown) AudioHardwareDestroyProcessTap(Tap);
        }

        [[nodiscard]] QString Error() const override { return Failure; }

        AudioObjectID Device = kAudioObjectUnknown;
        AudioObjectID Aggregate = kAudioObjectUnknown;
        AudioObjectID Tap = kAudioObjectUnknown;
        AudioDeviceIOProcID Proc = nullptr;
        QString Failure;
        std::vector<float> Scratch = std::vector<float>(MaxFrames * 2);
    };

    AudioObjectPropertyAddress Address(AudioObjectPropertySelector selector,
        AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal)
    {
        return {selector, scope, kAudioObjectPropertyElementMain};
    }

    QString Describe(const char* what, OSStatus status)
    {
        return QStringLiteral("%1 failed (OSStatus %2)").arg(QString::fromUtf8(what)).arg(status);
    }

    QString StringProperty(AudioObjectID object, AudioObjectPropertySelector selector)
    {
        auto address = Address(selector);
        auto value = CFStringRef{nullptr};
        auto size = UInt32{sizeof(value)};
        auto text = QString();
        if (AudioObjectGetPropertyData(object, &address, 0, nullptr, &size, &value) == noErr
            && value != nullptr)
        {
            text = QString::fromCFString(value);
            CFRelease(value);
        }
        return text;
    }

    int ChannelCount(AudioObjectID device, AudioObjectPropertyScope scope)
    {
        auto address = Address(kAudioDevicePropertyStreamConfiguration, scope);
        auto size = UInt32{0};
        auto channels = 0;
        if (AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &size) == noErr && size > 0)
        {
            auto storage = std::vector<std::byte>(size);
            auto* list = reinterpret_cast<AudioBufferList*>(storage.data());
            if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, list) == noErr)
                for (auto i = UInt32{0}; i < list->mNumberBuffers; ++i)
                    channels += static_cast<int>(list->mBuffers[i].mNumberChannels);
        }
        return channels;
    }

    int NominalRate(AudioObjectID device)
    {
        auto address = Address(kAudioDevicePropertyNominalSampleRate);
        auto rate = Float64{0};
        auto size = UInt32{sizeof(rate)};
        AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &rate);
        return static_cast<int>(rate);
    }

    AudioObjectID DefaultDevice(bool input)
    {
        auto address = Address(input ? kAudioHardwarePropertyDefaultInputDevice
                                     : kAudioHardwarePropertyDefaultOutputDevice);
        auto device = AudioObjectID{kAudioObjectUnknown};
        auto size = UInt32{sizeof(device)};
        AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, &device);
        return device;
    }

    AudioObjectID DeviceForUid(const QString& uid, bool input)
    {
        auto device = AudioObjectID{kAudioObjectUnknown};
        if (uid.isEmpty())
            device = DefaultDevice(input);
        else
        {
            auto address = Address(kAudioHardwarePropertyTranslateUIDToDevice);
            auto cfUid = uid.toCFString();
            auto size = UInt32{sizeof(device)};
            AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, sizeof(cfUid), &cfUid,
                &size, &device);
            CFRelease(cfUid);
        }
        return device;
    }

    // Finds a channel by its position across all buffers, since a device may deliver one
    // interleaved buffer or one buffer per channel.
    ChannelRef FindChannel(const AudioBufferList* list, int channel)
    {
        auto found = ChannelRef();
        auto base = 0;
        for (auto i = UInt32{0}; list != nullptr && i < list->mNumberBuffers && found.Data == nullptr; ++i)
        {
            const auto& buffer = list->mBuffers[i];
            const auto count = static_cast<int>(buffer.mNumberChannels);
            if (channel < base + count && buffer.mData != nullptr && count > 0)
                found = {static_cast<float*>(buffer.mData) + (channel - base), buffer.mNumberChannels,
                    buffer.mDataByteSize / (buffer.mNumberChannels * sizeof(float))};
            base += count;
        }
        return found;
    }

    void StartInput(CoreAudioStream& stream, int firstChannel, AudioBridge::CaptureCallback callback)
    {
        const auto rate = NominalRate(stream.Device);
        auto* target = &stream;
        auto status = AudioDeviceCreateIOProcIDWithBlock(&stream.Proc, stream.Device, nullptr,
            ^(const AudioTimeStamp*, const AudioBufferList* input, const AudioTimeStamp*,
                AudioBufferList*, const AudioTimeStamp*) {
                const auto left = FindChannel(input, firstChannel);
                auto right = FindChannel(input, firstChannel + 1);
                if (right.Data == nullptr) right = left;
                if (left.Data != nullptr)
                {
                    const auto frames = std::min({left.Frames, right.Frames, MaxFrames});
                    for (auto i = std::size_t{0}; i < frames; ++i)
                    {
                        target->Scratch[i * 2] = left.Data[i * left.Stride];
                        target->Scratch[i * 2 + 1] = right.Data[i * right.Stride];
                    }
                    callback(target->Scratch.data(), frames, rate);
                }
            });
        if (status == noErr) status = AudioDeviceStart(stream.Device, stream.Proc);
        if (status != noErr) stream.Failure = Describe("Starting audio capture", status);
    }

    void StartOutput(CoreAudioStream& stream, AudioBridge::RenderCallback callback)
    {
        const auto rate = NominalRate(stream.Device);
        auto* target = &stream;
        auto status = AudioDeviceCreateIOProcIDWithBlock(&stream.Proc, stream.Device, nullptr,
            ^(const AudioTimeStamp*, const AudioBufferList*, const AudioTimeStamp*,
                AudioBufferList* output, const AudioTimeStamp*) {
                const auto first = FindChannel(output, 0);
                const auto frames = std::min(first.Frames, MaxFrames);
                std::fill(target->Scratch.begin(), target->Scratch.begin() + frames * 2, 0.0f);
                callback(target->Scratch.data(), frames, rate);
                auto base = 0;
                for (auto b = UInt32{0}; b < output->mNumberBuffers; ++b)
                {
                    auto& buffer = output->mBuffers[b];
                    auto* data = static_cast<float*>(buffer.mData);
                    const auto channels = static_cast<int>(buffer.mNumberChannels);
                    for (auto i = std::size_t{0}; data != nullptr && i < frames; ++i)
                        for (auto c = 0; c < channels; ++c)
                            data[i * channels + c] = base + c < 2 ? target->Scratch[i * 2 + base + c] : 0.0f;
                    base += channels;
                }
            });
        if (status == noErr) status = AudioDeviceStart(stream.Device, stream.Proc);
        if (status != noErr) stream.Failure = Describe("Starting audio output", status);
    }

    QList<AudioBridge::DeviceInfo> Devices(AudioObjectPropertyScope scope)
    {
        auto address = Address(kAudioHardwarePropertyDevices);
        auto size = UInt32{0};
        auto devices = QList<AudioBridge::DeviceInfo>();
        if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size)
            == noErr)
        {
            auto ids = std::vector<AudioObjectID>(size / sizeof(AudioObjectID));
            if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size,
                    ids.data())
                == noErr)
                for (const auto id : ids)
                    if (ChannelCount(id, scope) > 0)
                        devices.append({StringProperty(id, kAudioDevicePropertyDeviceUID),
                            StringProperty(id, kAudioObjectPropertyName)});
        }
        return devices;
    }
} // namespace

std::unique_ptr<AudioBridge::AudioStream> AudioBridge::StartSystemCapture(CaptureCallback callback)
{
    auto stream = std::make_unique<CoreAudioStream>();
    if (@available(macOS 14.2, *))
    {
        // Excluding this process is what stops two machines that send each other their system
        // audio from feeding back. The process only has an audio object once it plays, which is
        // why the receiving side is started first.
        auto pidAddress = Address(kAudioHardwarePropertyTranslatePIDToProcessObject);
        auto pid = getpid();
        auto process = AudioObjectID{kAudioObjectUnknown};
        auto size = UInt32{sizeof(process)};
        AudioObjectGetPropertyData(kAudioObjectSystemObject, &pidAddress, sizeof(pid), &pid,
            &size, &process);
        auto* excluded = process != kAudioObjectUnknown ? @[ @(process) ] : @[];
        auto* description =
            [[CATapDescription alloc] initStereoGlobalTapButExcludeProcesses:excluded];
        description.name = @"AudioBridge";
        description.privateTap = YES;
        description.muteBehavior = CATapUnmuted;
        auto status = AudioHardwareCreateProcessTap(description, &stream->Tap);

        // A tap is read through an aggregate device. The default output is its clock, and its
        // own input channels, if it has any, come before the tap's two.
        const auto output = DefaultDevice(false);
        auto* outputUid = StringProperty(output, kAudioDevicePropertyDeviceUID).toNSString();
        if (status == noErr)
        {
            auto* aggregate = @{
                @kAudioAggregateDeviceNameKey: @"AudioBridge System Audio",
                @kAudioAggregateDeviceUIDKey: [[NSUUID UUID] UUIDString],
                @kAudioAggregateDeviceMainSubDeviceKey: outputUid,
                @kAudioAggregateDeviceIsPrivateKey: @YES,
                @kAudioAggregateDeviceIsStackedKey: @NO,
                @kAudioAggregateDeviceTapAutoStartKey: @YES,
                @kAudioAggregateDeviceSubDeviceListKey: @[ @{ @kAudioSubDeviceUIDKey: outputUid } ],
                @kAudioAggregateDeviceTapListKey: @[ @{
                    @kAudioSubTapDriftCompensationKey: @YES,
                    @kAudioSubTapUIDKey: description.UUID.UUIDString,
                } ],
            };
            status = AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)aggregate,
                &stream->Aggregate);
        }
        if (status == noErr)
        {
            stream->Device = stream->Aggregate;
            StartInput(*stream, ChannelCount(output, kAudioObjectPropertyScopeInput),
                std::move(callback));
        }
        else
            stream->Failure = Describe("Creating the system audio tap", status);
    }
    else
        stream->Failure = QStringLiteral("System audio capture needs macOS 14.2 or later");
    return stream;
}

std::unique_ptr<AudioBridge::AudioStream> AudioBridge::StartDeviceCapture(const QString& deviceId,
    CaptureCallback callback)
{
    auto stream = std::make_unique<CoreAudioStream>();
    stream->Device = DeviceForUid(deviceId, true);
    if (stream->Device != kAudioObjectUnknown)
        StartInput(*stream, 0, std::move(callback));
    else
        stream->Failure = QStringLiteral("Input device %1 not found").arg(deviceId);
    return stream;
}

std::unique_ptr<AudioBridge::AudioStream> AudioBridge::StartRender(const QString& deviceId,
    RenderCallback callback)
{
    auto stream = std::make_unique<CoreAudioStream>();
    stream->Device = DeviceForUid(deviceId, false);
    if (stream->Device != kAudioObjectUnknown)
        StartOutput(*stream, std::move(callback));
    else
        stream->Failure = QStringLiteral("Output device %1 not found").arg(deviceId);
    return stream;
}

QList<AudioBridge::DeviceInfo> AudioBridge::InputDevices()
{
    return Devices(kAudioObjectPropertyScopeInput);
}

QList<AudioBridge::DeviceInfo> AudioBridge::OutputDevices()
{
    return Devices(kAudioObjectPropertyScopeOutput);
}

QString AudioBridge::DefaultOutputId()
{
    return StringProperty(DefaultDevice(false), kAudioDevicePropertyDeviceUID);
}

namespace
{
    // A second, muting tap that nothing reads: while it exists every process but this one is
    // silent on the outputs, and the capturing tap still hears them. The tap belongs to this
    // process, so the system removes it - and the mute with it - however the process ends.
    class TapMute final : public AudioBridge::LocalMute
    {
    public:
        TapMute()
        {
            if (@available(macOS 14.2, *))
            {
                auto pidAddress = Address(kAudioHardwarePropertyTranslatePIDToProcessObject);
                auto pid = getpid();
                auto process = AudioObjectID{kAudioObjectUnknown};
                auto size = UInt32{sizeof(process)};
                AudioObjectGetPropertyData(kAudioObjectSystemObject, &pidAddress, sizeof(pid),
                    &pid, &size, &process);
                auto* excluded = process != kAudioObjectUnknown ? @[ @(process) ] : @[];
                auto* description =
                    [[CATapDescription alloc] initStereoGlobalTapButExcludeProcesses:excluded];
                description.name = @"AudioBridge mute";
                description.privateTap = YES;
                description.muteBehavior = CATapMuted;
                const auto status = AudioHardwareCreateProcessTap(description, &Tap);
                if (status != noErr) Failure = Describe("Muting the local output", status);
            }
            else
                Failure = QStringLiteral("Muting the local output needs macOS 14.2 or later");
        }

        ~TapMute() override
        {
            if (Tap != kAudioObjectUnknown) AudioHardwareDestroyProcessTap(Tap);
        }

        [[nodiscard]] QString Error() const override { return Failure; }

    private:
        AudioObjectID Tap = kAudioObjectUnknown;
        QString Failure;
    };
} // namespace

std::unique_ptr<AudioBridge::LocalMute> AudioBridge::MuteLocalOutput()
{
    return std::make_unique<TapMute>();
}

void AudioBridge::RestoreLocalOutput()
{
}
