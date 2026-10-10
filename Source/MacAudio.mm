#include "AudioBackend.h"
#include "Protocol.h"

#import <CoreAudio/AudioHardware.h>
#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>
#import <Foundation/Foundation.h>
#include <QtCore/QString>

#include <algorithm>
#include <cstddef>
#include <span>
#include <unistd.h>
#include <vector>

// <editor-fold desc="Helpers">
namespace
{
    using HIAudioStreamer::Channels;

    constexpr auto MaxFrames = std::size_t{8192};

    struct ChannelRef
    {
        float* Data = nullptr;
        UInt32 Stride = 0;
        std::size_t Frames = 0;
    };

    // Owns whatever a stream set up and tears it down in reverse. The IOProc is stopped before
    // anything it reads from goes away.
    class CoreAudioStream final : public HIAudioStreamer::AudioStream
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
        std::vector<float> Scratch = std::vector<float>(MaxFrames * Channels);
    };

    AudioObjectPropertyAddress Address(
        AudioObjectPropertySelector selector,
        AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal)
    {
        return {selector, scope, kAudioObjectPropertyElementMain};
    }

    QString Describe(const char* what, OSStatus status)
    {
        return QStringLiteral("%1 failed (OSStatus %2)").arg(QString::fromUtf8(what)).arg(status);
    }

    std::span<AudioBuffer> Buffers(AudioBufferList* list)
    {
        return list != nullptr ? std::span(&list->mBuffers[0], list->mNumberBuffers)
                               : std::span<AudioBuffer>();
    }

    std::span<const AudioBuffer> Buffers(const AudioBufferList* list)
    {
        return list != nullptr ? std::span(&list->mBuffers[0], list->mNumberBuffers)
                               : std::span<const AudioBuffer>();
    }

    QString StringProperty(AudioObjectID object, AudioObjectPropertySelector selector)
    {
        auto address = Address(selector);
        auto size = UInt32{sizeof(CFStringRef)};
        auto text = QString();
        if (auto value = CFStringRef{nullptr};
            AudioObjectGetPropertyData(object, &address, 0, nullptr, &size, &value) == noErr
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
        auto channels = 0;
        if (auto size = UInt32{0};
            AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &size) == noErr
            && size > 0)
        {
            auto storage = std::vector<std::byte>(size);
            if (auto* list = reinterpret_cast<AudioBufferList*>(storage.data());
                AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, list) == noErr)
            {
                for (const auto& buffer : Buffers(list))
                    channels += static_cast<int>(buffer.mNumberChannels);
            }
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
        if (uid.isEmpty()) device = DefaultDevice(input);
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
        for (const auto& buffer : Buffers(list))
        {
            const auto count = static_cast<int>(buffer.mNumberChannels);
            if (found.Data == nullptr && channel >= base && channel < base + count
                && buffer.mData != nullptr)
            {
                found = {static_cast<float*>(buffer.mData) + (channel - base),
                         buffer.mNumberChannels,
                         buffer.mDataByteSize / (buffer.mNumberChannels * sizeof(float))};
            }
            base += count;
        }
        return found;
    }

    // A tap on everything but this process; nothing when this process has no audio object yet,
    // which it only gets once it plays. Leaving this process out is what keeps two machines that
    // send each other their system audio from feeding back, and what keeps a mute from
    // silencing what this process plays itself.
    CATapDescription* TapExcludingThisProcess(NSString* name, CATapMuteBehavior behavior)
    {
        auto address = Address(kAudioHardwarePropertyTranslatePIDToProcessObject);
        auto pid = getpid();
        auto process = AudioObjectID{kAudioObjectUnknown};
        auto size = UInt32{sizeof(process)};
        auto* description = static_cast<CATapDescription*>(nil);
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, sizeof(pid), &pid, &size,
                                       &process)
                == noErr
            && process != kAudioObjectUnknown)
        {
            description = [[CATapDescription alloc]
                initStereoGlobalTapButExcludeProcesses:@[ @(process) ]];
            description.name = name;
            description.privateTap = YES;
            description.muteBehavior = behavior;
        }
        return description;
    }

    void StartInput(CoreAudioStream& stream, int firstChannel,
                    HIAudioStreamer::CaptureCallback callback)
    {
        const auto rate = NominalRate(stream.Device);
        auto* target = &stream;
        auto status = AudioDeviceCreateIOProcIDWithBlock(
            &stream.Proc, stream.Device, nullptr,
            ^(const AudioTimeStamp*, const AudioBufferList* input, const AudioTimeStamp*,
              AudioBufferList*, const AudioTimeStamp*) {
                const auto left = FindChannel(input, firstChannel);
                const auto second = FindChannel(input, firstChannel + 1);
                const auto right = second.Data != nullptr ? second : left;
                if (left.Data != nullptr)
                {
                    const auto frames = std::min({left.Frames, right.Frames, MaxFrames});
                    for (auto i = std::size_t{0}; i < frames; ++i)
                    {
                        target->Scratch[i * Channels] = left.Data[i * left.Stride];
                        target->Scratch[i * Channels + 1] = right.Data[i * right.Stride];
                    }
                    callback(target->Scratch.data(), frames, rate);
                }
            });
        if (status == noErr) status = AudioDeviceStart(stream.Device, stream.Proc);
        if (status != noErr) stream.Failure = Describe("Starting audio capture", status);
    }

    void StartOutput(CoreAudioStream& stream, HIAudioStreamer::RenderCallback callback)
    {
        const auto rate = NominalRate(stream.Device);
        auto* target = &stream;
        auto status = AudioDeviceCreateIOProcIDWithBlock(
            &stream.Proc, stream.Device, nullptr,
            ^(const AudioTimeStamp*, const AudioBufferList*, const AudioTimeStamp*,
              AudioBufferList* output, const AudioTimeStamp*) {
                const auto frames = std::min(FindChannel(output, 0).Frames, MaxFrames);
                const auto mix = std::span(target->Scratch).first(frames * Channels);
                std::ranges::fill(mix, 0.0f);
                callback(mix.data(), frames, rate);
                // Spread the stereo mix over the device's buffers, silencing any channel past it.
                auto base = 0;
                for (auto& buffer : Buffers(output))
                {
                    const auto channels = static_cast<int>(buffer.mNumberChannels);
                    if (auto* data = static_cast<float*>(buffer.mData); data != nullptr)
                        for (auto i = std::size_t{0}; i < frames; ++i)
                            for (auto c = 0; c < channels; ++c)
                            {
                                const auto source = base + c;
                                data[i * channels + c] =
                                    source < Channels ? mix[i * Channels + source] : 0.0f;
                            }
                    base += channels;
                }
            });
        if (status == noErr) status = AudioDeviceStart(stream.Device, stream.Proc);
        if (status != noErr) stream.Failure = Describe("Starting audio output", status);
    }

    QList<HIAudioStreamer::DeviceInfo> Devices(AudioObjectPropertyScope scope)
    {
        auto address = Address(kAudioHardwarePropertyDevices);
        auto devices = QList<HIAudioStreamer::DeviceInfo>();
        if (auto size = UInt32{0};
            AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size)
            == noErr)
        {
            auto ids = std::vector<AudioObjectID>(size / sizeof(AudioObjectID));
            if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size,
                                           ids.data())
                == noErr)
            {
                for (const auto id : ids)
                    if (ChannelCount(id, scope) > 0)
                    {
                        devices.append({StringProperty(id, kAudioDevicePropertyDeviceUID),
                                        StringProperty(id, kAudioObjectPropertyName)});
                    }
            }
        }
        if (devices.isEmpty()) qWarning("No audio devices found for scope %u", scope);
        return devices;
    }
} // namespace
// </editor-fold>

// <editor-fold desc="Streams and devices">
std::unique_ptr<HIAudioStreamer::AudioStream> HIAudioStreamer::StartSystemCapture(
    CaptureCallback callback)
{
    auto stream = std::make_unique<CoreAudioStream>();
    if (auto* description = TapExcludingThisProcess(@"HIAudioStreamer", CATapUnmuted);
        description != nil)
    {
        auto status = AudioHardwareCreateProcessTap(description, &stream->Tap);
        // A tap is read through an aggregate device. The default output is its clock, and its
        // own input channels, if it has any, come before the tap's two.
        const auto output = DefaultDevice(false);
        auto* outputUid = StringProperty(output, kAudioDevicePropertyDeviceUID).toNSString();
        if (status == noErr)
        {
            auto* aggregate = @{
                @kAudioAggregateDeviceNameKey : @"HIAudioStreamer System Audio",
                @kAudioAggregateDeviceUIDKey : [[NSUUID UUID] UUIDString],
                @kAudioAggregateDeviceMainSubDeviceKey : outputUid,
                @kAudioAggregateDeviceIsPrivateKey : @YES,
                @kAudioAggregateDeviceIsStackedKey : @NO,
                @kAudioAggregateDeviceTapAutoStartKey : @YES,
                @kAudioAggregateDeviceSubDeviceListKey : @[
                    @{@kAudioSubDeviceUIDKey : outputUid}
                ],
                @kAudioAggregateDeviceTapListKey : @[ @{
                    @kAudioSubTapDriftCompensationKey : @YES,
                    @kAudioSubTapUIDKey : description.UUID.UUIDString,
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
        else stream->Failure = Describe("Creating the system audio tap", status);
    }
    else
    {
        stream->Failure =
            QStringLiteral("This process has no audio object to leave out of the capture yet");
    }
    return stream;
}

std::unique_ptr<HIAudioStreamer::AudioStream> HIAudioStreamer::StartDeviceCapture(
    const QString& deviceId, CaptureCallback callback)
{
    auto stream = std::make_unique<CoreAudioStream>();
    if (stream->Device = DeviceForUid(deviceId, true); stream->Device != kAudioObjectUnknown)
        StartInput(*stream, 0, std::move(callback));
    else stream->Failure = QStringLiteral("Input device %1 not found").arg(deviceId);
    return stream;
}

std::unique_ptr<HIAudioStreamer::AudioStream> HIAudioStreamer::StartRender(const QString& deviceId,
                                                                           RenderCallback callback)
{
    auto stream = std::make_unique<CoreAudioStream>();
    if (stream->Device = DeviceForUid(deviceId, false); stream->Device != kAudioObjectUnknown)
        StartOutput(*stream, std::move(callback));
    else stream->Failure = QStringLiteral("Output device %1 not found").arg(deviceId);
    return stream;
}

QList<HIAudioStreamer::DeviceInfo> HIAudioStreamer::InputDevices()
{
    return Devices(kAudioObjectPropertyScopeInput);
}

QList<HIAudioStreamer::DeviceInfo> HIAudioStreamer::OutputDevices()
{
    return Devices(kAudioObjectPropertyScopeOutput);
}

QString HIAudioStreamer::DefaultOutputId()
{
    return StringProperty(DefaultDevice(false), kAudioDevicePropertyDeviceUID);
}
// </editor-fold>

// <editor-fold desc="Local mute">
namespace
{
    // A second, muting tap that nothing reads: while it exists every process but this one is
    // silent on the outputs, and the capturing tap still hears them. The tap belongs to this
    // process, so the system removes it - and the mute with it - however the process ends.
    class TapMute final : public HIAudioStreamer::LocalMute
    {
    public:
        TapMute()
        {
            if (auto* description = TapExcludingThisProcess(@"HIAudioStreamer mute", CATapMuted);
                description == nil)
            {
                Failure =
                    QStringLiteral("This process has no audio object to leave out of the mute");
            }
            else if (const auto status = AudioHardwareCreateProcessTap(description, &Tap);
                     status != noErr)
                Failure = Describe("Muting the local output", status);
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

std::unique_ptr<HIAudioStreamer::LocalMute> HIAudioStreamer::MuteLocalOutput()
{
    return std::make_unique<TapMute>();
}

// Nothing to undo: the muting tap dies with the process.
void HIAudioStreamer::RestoreLocalOutput() {}
// </editor-fold>
