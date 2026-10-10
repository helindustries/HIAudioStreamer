#include "AudioBackend.h"

#include <QtCore/QString>

#include <QtCore/QSettings>

#include <Windows.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <audioclientactivationparams.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <wrl/implements.h>
// Defines the property keys below in this translation unit rather than leaving them extern.
#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <atomic>
#include <future>
#include <thread>
#include <vector>

namespace
{
    using Microsoft::WRL::ComPtr;

    constexpr auto StreamRate = 48000;
    constexpr auto BufferDuration = REFERENCE_TIME{200000};
    constexpr auto WaitTimeoutMs = DWORD{200};
    constexpr auto ActivationTimeoutMs = DWORD{5000};
    constexpr auto MaxFrames = std::size_t{48000};
    // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, spelled out so no extra library has to provide it.
    constexpr auto FloatSubtype =
        GUID{0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

    enum class Mode
    {
        SystemCapture,
        DeviceCapture,
        Render,
    };

    QString Describe(const char* what, HRESULT result)
    {
        return QStringLiteral("%1 failed (0x%2)")
            .arg(QString::fromUtf8(what))
            .arg(static_cast<quint32>(result), 8, 16, QLatin1Char('0'));
    }

    // Shared-mode streams are opened in this format and Windows converts to the device's own,
    // so every device looks alike from here.
    WAVEFORMATEXTENSIBLE FloatFormat()
    {
        auto format = WAVEFORMATEXTENSIBLE{};
        format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        format.Format.nChannels = 2;
        format.Format.nSamplesPerSec = StreamRate;
        format.Format.wBitsPerSample = 32;
        format.Format.nBlockAlign = format.Format.nChannels * format.Format.wBitsPerSample / 8;
        format.Format.nAvgBytesPerSec = format.Format.nSamplesPerSec * format.Format.nBlockAlign;
        format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        format.Samples.wValidBitsPerSample = 32;
        format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        format.SubFormat = FloatSubtype;
        return format;
    }

    // Process loopback has no mix format to ask for and accepts plain PCM, as Microsoft's own
    // sample uses.
    WAVEFORMATEX LoopbackFormat()
    {
        auto format = WAVEFORMATEX{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 2;
        format.nSamplesPerSec = StreamRate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        return format;
    }

    class ActivationHandler
        : public Microsoft::WRL::RuntimeClass<
              Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, Microsoft::WRL::FtmBase,
              IActivateAudioInterfaceCompletionHandler>
    {
    public:
        ActivationHandler()
            : Done(CreateEventW(nullptr, TRUE, FALSE, nullptr))
        {}

        ~ActivationHandler() override { CloseHandle(Done); }

        STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation* operation) override
        {
            auto activated = HRESULT{E_FAIL};
            auto unknown = ComPtr<IUnknown>();
            Result = operation->GetActivateResult(&activated, &unknown);
            if (SUCCEEDED(Result)) Result = activated;
            if (SUCCEEDED(Result)) Result = unknown.As(&Client);
            SetEvent(Done);
            return S_OK;
        }

        HANDLE Done;
        HRESULT Result = E_FAIL;
        ComPtr<IAudioClient> Client;
    };

    HRESULT ActivateProcessLoopback(ComPtr<IAudioClient>& client)
    {
        // Everything except this process tree: the half that stops two machines sending each
        // other their system audio from feeding back.
        auto params = AUDIOCLIENT_ACTIVATION_PARAMS{};
        params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        params.ProcessLoopbackParams.TargetProcessId = GetCurrentProcessId();
        params.ProcessLoopbackParams.ProcessLoopbackMode =
            PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;
        auto variant = PROPVARIANT{};
        variant.vt = VT_BLOB;
        variant.blob.cbSize = sizeof(params);
        variant.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

        auto handler = Microsoft::WRL::Make<ActivationHandler>();
        auto operation = ComPtr<IActivateAudioInterfaceAsyncOperation>();
        auto result = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
            __uuidof(IAudioClient), &variant, handler.Get(), &operation);
        if (SUCCEEDED(result))
            result = WaitForSingleObject(handler->Done, ActivationTimeoutMs) == WAIT_OBJECT_0
                         ? handler->Result
                         : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        if (SUCCEEDED(result)) client = handler->Client;
        return result;
    }

    HRESULT ActivateDevice(const QString& id, EDataFlow flow, ComPtr<IAudioClient>& client)
    {
        auto enumerator = ComPtr<IMMDeviceEnumerator>();
        auto device = ComPtr<IMMDevice>();
        auto result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(&enumerator));
        if (SUCCEEDED(result))
            result = id.isEmpty()
                         ? enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device)
                         : enumerator->GetDevice(reinterpret_cast<LPCWSTR>(id.utf16()), &device);
        if (SUCCEEDED(result))
            result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                reinterpret_cast<void**>(client.ReleaseAndGetAddressOf()));
        return result;
    }

    // One thread per stream, event-driven. Setup happens on that thread, since WASAPI objects
    // belong to the apartment that created them, and the constructor waits until it is known
    // whether the stream started.
    class WasapiStream final : public HIAudioStreamer::AudioStream
    {
    public:
        WasapiStream(Mode mode, QString deviceId, HIAudioStreamer::CaptureCallback onCapture,
            HIAudioStreamer::RenderCallback onRender)
            : StreamMode(mode)
            , DeviceId(std::move(deviceId))
            , OnCapture(std::move(onCapture))
            , OnRender(std::move(onRender))
        {
            auto started = std::promise<QString>();
            auto outcome = started.get_future();
            Worker = std::thread([this, &started] { Run(started); });
            Failure = outcome.get();
        }

        ~WasapiStream() override
        {
            Stopping = true;
            if (Worker.joinable()) Worker.join();
        }

        [[nodiscard]] QString Error() const override { return Failure; }

    private:
        void Run(std::promise<QString>& started);
        void PumpCapture(IAudioCaptureClient& capture, bool pcm16);
        void PumpRender(IAudioClient& client, IAudioRenderClient& render, UINT32 bufferFrames);

        Mode StreamMode;
        QString DeviceId;
        HIAudioStreamer::CaptureCallback OnCapture;
        HIAudioStreamer::RenderCallback OnRender;
        QString Failure;
        std::atomic<bool> Stopping{false};
        std::vector<float> Scratch = std::vector<float>(MaxFrames * 2);
        std::thread Worker;
    };

    void WasapiStream::Run(std::promise<QString>& started)
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto taskIndex = DWORD{0};
        const auto task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
        const auto event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        const auto system = StreamMode == Mode::SystemCapture;
        const auto render = StreamMode == Mode::Render;

        auto client = ComPtr<IAudioClient>();
        auto result = system ? ActivateProcessLoopback(client)
                             : ActivateDevice(DeviceId, render ? eRender : eCapture, client);
        const auto loopbackFormat = LoopbackFormat();
        const auto floatFormat = FloatFormat();
        const auto* format = system ? &loopbackFormat : &floatFormat.Format;
        const auto flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                           | (system ? AUDCLNT_STREAMFLAGS_LOOPBACK
                                     : AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                                           | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY);
        if (SUCCEEDED(result))
            result = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, BufferDuration, 0, format,
                nullptr);
        if (SUCCEEDED(result)) result = client->SetEventHandle(event);

        auto capture = ComPtr<IAudioCaptureClient>();
        auto renderer = ComPtr<IAudioRenderClient>();
        if (SUCCEEDED(result))
            result = render ? client->GetService(IID_PPV_ARGS(&renderer))
                            : client->GetService(IID_PPV_ARGS(&capture));
        auto bufferFrames = UINT32{0};
        if (SUCCEEDED(result)) result = client->GetBufferSize(&bufferFrames);
        if (SUCCEEDED(result)) result = client->Start();

        const auto running = SUCCEEDED(result);
        started.set_value(running ? QString() : Describe(render ? "Opening the audio output"
                                                         : system ? "Capturing system audio"
                                                                  : "Opening the audio input",
                                                    result));
        while (running && !Stopping)
            if (WaitForSingleObject(event, WaitTimeoutMs) == WAIT_OBJECT_0)
            {
                if (render)
                    PumpRender(*client.Get(), *renderer.Get(), bufferFrames);
                else
                    PumpCapture(*capture.Get(), system);
            }

        if (running) client->Stop();
        CloseHandle(event);
        if (task != nullptr) AvRevertMmThreadCharacteristics(task);
        CoUninitialize();
    }

    void WasapiStream::PumpCapture(IAudioCaptureClient& capture, bool pcm16)
    {
        auto packet = UINT32{0};
        auto draining = true;
        while (draining && SUCCEEDED(capture.GetNextPacketSize(&packet)) && packet > 0)
        {
            auto* data = static_cast<BYTE*>(nullptr);
            auto frames = UINT32{0};
            auto flags = DWORD{0};
            draining = SUCCEEDED(capture.GetBuffer(&data, &frames, &flags, nullptr, nullptr));
            if (draining)
            {
                const auto count = std::min<std::size_t>(frames, MaxFrames);
                const auto silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                for (auto i = std::size_t{0}; i < count * 2; ++i)
                    Scratch[i] = silent ? 0.0f
                                 : pcm16 ? reinterpret_cast<const std::int16_t*>(data)[i] / 32768.0f
                                         : reinterpret_cast<const float*>(data)[i];
                OnCapture(Scratch.data(), count, StreamRate);
                capture.ReleaseBuffer(frames);
            }
        }
    }

    void WasapiStream::PumpRender(IAudioClient& client, IAudioRenderClient& render,
        UINT32 bufferFrames)
    {
        auto padding = UINT32{0};
        if (SUCCEEDED(client.GetCurrentPadding(&padding)) && padding < bufferFrames)
        {
            const auto frames = bufferFrames - padding;
            auto* data = static_cast<BYTE*>(nullptr);
            if (SUCCEEDED(render.GetBuffer(frames, &data)))
            {
                auto* samples = reinterpret_cast<float*>(data);
                std::fill(samples, samples + frames * 2, 0.0f);
                OnRender(samples, frames, StreamRate);
                render.ReleaseBuffer(frames, 0);
            }
        }
    }

    QList<HIAudioStreamer::DeviceInfo> Endpoints(EDataFlow flow)
    {
        auto devices = QList<HIAudioStreamer::DeviceInfo>();
        auto enumerator = ComPtr<IMMDeviceEnumerator>();
        auto collection = ComPtr<IMMDeviceCollection>();
        auto count = UINT{0};
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                IID_PPV_ARGS(&enumerator)))
            && SUCCEEDED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection))
            && SUCCEEDED(collection->GetCount(&count)))
            for (auto i = UINT{0}; i < count; ++i)
            {
                auto device = ComPtr<IMMDevice>();
                auto properties = ComPtr<IPropertyStore>();
                auto* id = LPWSTR{nullptr};
                auto name = PROPVARIANT{};
                PropVariantInit(&name);
                if (SUCCEEDED(collection->Item(i, &device)) && SUCCEEDED(device->GetId(&id))
                    && SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))
                    && SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &name))
                    && name.vt == VT_LPWSTR)
                    devices.append({QString::fromWCharArray(id), QString::fromWCharArray(name.pwszVal)});
                PropVariantClear(&name);
                if (id != nullptr) CoTaskMemFree(id);
            }
        return devices;
    }
} // namespace

std::unique_ptr<HIAudioStreamer::AudioStream> HIAudioStreamer::StartSystemCapture(CaptureCallback callback)
{
    return std::make_unique<WasapiStream>(Mode::SystemCapture, QString(), std::move(callback),
        RenderCallback());
}

std::unique_ptr<HIAudioStreamer::AudioStream> HIAudioStreamer::StartDeviceCapture(const QString& deviceId,
    CaptureCallback callback)
{
    return std::make_unique<WasapiStream>(Mode::DeviceCapture, deviceId, std::move(callback),
        RenderCallback());
}

std::unique_ptr<HIAudioStreamer::AudioStream> HIAudioStreamer::StartRender(const QString& deviceId,
    RenderCallback callback)
{
    return std::make_unique<WasapiStream>(Mode::Render, deviceId, CaptureCallback(),
        std::move(callback));
}

QList<HIAudioStreamer::DeviceInfo> HIAudioStreamer::InputDevices()
{
    return Endpoints(eCapture);
}

QList<HIAudioStreamer::DeviceInfo> HIAudioStreamer::OutputDevices()
{
    return Endpoints(eRender);
}

namespace
{
    constexpr auto MuteDeviceKey = "LocalMute/Device";
    constexpr auto MuteWasMutedKey = "LocalMute/WasMuted";

    ComPtr<IAudioEndpointVolume> EndpointVolume(const QString& deviceId, QString* resolvedId)
    {
        auto enumerator = ComPtr<IMMDeviceEnumerator>();
        auto device = ComPtr<IMMDevice>();
        auto volume = ComPtr<IAudioEndpointVolume>();
        auto* id = LPWSTR{nullptr};
        auto result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(&enumerator));
        if (SUCCEEDED(result))
            result = deviceId.isEmpty()
                         ? enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)
                         : enumerator->GetDevice(reinterpret_cast<LPCWSTR>(deviceId.utf16()), &device);
        if (SUCCEEDED(result)) result = device->GetId(&id);
        if (SUCCEEDED(result) && resolvedId != nullptr) *resolvedId = QString::fromWCharArray(id);
        if (SUCCEEDED(result))
            result = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                reinterpret_cast<void**>(volume.ReleaseAndGetAddressOf()));
        if (id != nullptr) CoTaskMemFree(id);
        return SUCCEEDED(result) ? volume : ComPtr<IAudioEndpointVolume>();
    }

    // Mutes the default output endpoint. Process loopback captures each application's stream
    // before the endpoint applies its mute, so the capture still hears everything. The previous
    // state goes into the settings before muting, so a crash cannot leave the speakers muted
    // beyond the next start, which calls RestoreLocalOutput.
    class EndpointMute final : public HIAudioStreamer::LocalMute
    {
    public:
        EndpointMute()
            : Volume(EndpointVolume(QString(), &DeviceId))
        {
            auto muted = BOOL{FALSE};
            if (Volume != nullptr && SUCCEEDED(Volume->GetMute(&muted)))
            {
                WasMuted = muted != FALSE;
                auto settings = QSettings();
                settings.setValue(MuteDeviceKey, DeviceId);
                settings.setValue(MuteWasMutedKey, WasMuted);
                settings.sync();
                if (FAILED(Volume->SetMute(TRUE, nullptr)))
                    Failure = QStringLiteral("Could not mute the local output");
            }
            else
                Failure = QStringLiteral("Could not reach the local output to mute it");
        }

        ~EndpointMute() override
        {
            if (Volume != nullptr) Volume->SetMute(WasMuted ? TRUE : FALSE, nullptr);
            auto settings = QSettings();
            settings.remove(QStringLiteral("LocalMute"));
        }

        [[nodiscard]] QString Error() const override { return Failure; }

    private:
        QString DeviceId;
        ComPtr<IAudioEndpointVolume> Volume;
        bool WasMuted = false;
        QString Failure;
    };
} // namespace

std::unique_ptr<HIAudioStreamer::LocalMute> HIAudioStreamer::MuteLocalOutput()
{
    return std::make_unique<EndpointMute>();
}

void HIAudioStreamer::RestoreLocalOutput()
{
    auto settings = QSettings();
    if (settings.contains(MuteDeviceKey))
    {
        const auto volume = EndpointVolume(settings.value(MuteDeviceKey).toString(), nullptr);
        if (volume != nullptr)
            volume->SetMute(settings.value(MuteWasMutedKey).toBool() ? TRUE : FALSE, nullptr);
        settings.remove(QStringLiteral("LocalMute"));
    }
}

QString HIAudioStreamer::DefaultOutputId()
{
    auto enumerator = ComPtr<IMMDeviceEnumerator>();
    auto device = ComPtr<IMMDevice>();
    auto* id = LPWSTR{nullptr};
    auto result = QString();
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(&enumerator)))
        && SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))
        && SUCCEEDED(device->GetId(&id)))
        result = QString::fromWCharArray(id);
    if (id != nullptr) CoTaskMemFree(id);
    return result;
}
