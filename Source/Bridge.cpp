#include "Bridge.h"

#include <QtCore/QDateTime>

#include <algorithm>

namespace
{
    // One second at the highest rate a device is likely to run, per direction.
    constexpr auto BufferFrames = std::size_t{192000};
    constexpr auto MixChunkFrames = std::size_t{4096};
    constexpr auto SilentAfterMs = 1000;
} // namespace

AudioBridge::Bridge::Bridge()
    : Outgoing(BufferFrames)
    , MixScratch(MixChunkFrames * Channels)
    , NetworkObject(std::make_unique<Network>(Outgoing, Incoming, Stats))
{
    for (auto i = 0; i < MaxPeers; ++i)
        Incoming.push_back(std::make_unique<Playout>(BufferFrames));

    auto* network = NetworkObject.get();
    connect(network, &Network::PairingCodeShown, this, &Bridge::PairingCodeShown);
    connect(network, &Network::PairingCodeNeeded, this, &Bridge::PairingCodeNeeded);
    connect(network, &Network::Paired, this, &Bridge::Paired);
    connect(network, &Network::PairingFailed, this, &Bridge::PairingFailed);
    connect(network, &Network::Failed, this, &Bridge::Failed);
    network->moveToThread(&NetworkThread);
    NetworkThread.start(QThread::TimeCriticalPriority);
}

AudioBridge::Bridge::~Bridge()
{
    Capture.reset();
    Render.reset();
    auto* network = NetworkObject.get();
    QMetaObject::invokeMethod(network, [network] { network->Shutdown(); },
        Qt::BlockingQueuedConnection);
    NetworkThread.quit();
    NetworkThread.wait();
}

void AudioBridge::Bridge::Apply(const Config& config)
{
    Current = config;
    Errors.clear();
    Capture.reset();
    Render.reset();
    Stats.CaptureRate.store(0, std::memory_order_relaxed);
    for (auto& playout : Incoming)
        playout->TargetMs.store(config.BufferMs, std::memory_order_relaxed);

    auto* network = NetworkObject.get();
    QMetaObject::invokeMethod(network, [network, config] { network->Configure(config); },
        Qt::QueuedConnection);

    // Output first: on macOS the system tap can only exclude this process once the process has
    // an audio object, which playing gives it.
    Render = StartRender(config.Output,
        [this](float* frames, std::size_t count, int rate) { Mix(frames, count, rate); });
    if (config.Send)
    {
        auto onFrames = [this](const float* frames, std::size_t count, int rate) {
            Stats.CaptureRate.store(rate, std::memory_order_relaxed);
            Outgoing.Write(frames, count);
        };
        Capture = config.Source.isEmpty() ? StartSystemCapture(onFrames)
                                          : StartDeviceCapture(config.Source, onFrames);
    }
    for (const auto* stream : {Render.get(), Capture.get()})
        if (stream != nullptr && !stream->Error().isEmpty()) Errors.append(stream->Error());
}

void AudioBridge::Bridge::Mix(float* frames, std::size_t count, int rate)
{
    for (auto done = std::size_t{0}; done < count; done += MixChunkFrames)
    {
        const auto chunk = std::min(MixChunkFrames, count - done);
        auto* out = frames + done * Channels;
        for (auto& playout : Incoming)
            playout->MixInto(out, chunk, rate);
        for (auto i = std::size_t{0}; i < chunk * Channels; ++i)
            out[i] = std::clamp(out[i], -1.0f, 1.0f);
    }
}

bool AudioBridge::Bridge::Active() const
{
    const auto now = QDateTime::currentMSecsSinceEpoch();
    auto receiving = false;
    for (auto i = 0; i < std::min<int>(MaxPeers, Current.Peers.size()); ++i)
        receiving = receiving || now - Stats.LastReceivedMs[i].load() < SilentAfterMs;
    return receiving || (Current.Send && Stats.CaptureRate.load() > 0);
}

QString AudioBridge::Bridge::Status() const
{
    const auto now = QDateTime::currentMSecsSinceEpoch();
    auto lines = QStringList();
    if (Current.Send)
    {
        const auto rate = Stats.CaptureRate.load();
        const auto targets = std::count_if(Current.Peers.begin(), Current.Peers.end(),
            [](const auto& peer) { return peer.Send; });
        const auto source = Current.Source.isEmpty() ? QStringLiteral("system audio")
                                                     : QStringLiteral("input");
        lines.append(rate > 0 && targets > 0
                         ? QStringLiteral("Sending %1 to %2 at %3 kHz")
                               .arg(source)
                               .arg(targets)
                               .arg(rate / 1000.0)
                         : QStringLiteral("Sending: nothing playing yet"));
    }
    for (auto i = 0; i < std::min<int>(MaxPeers, Current.Peers.size()); ++i)
    {
        const auto receiving = now - Stats.LastReceivedMs[i].load() < SilentAfterMs;
        lines.append(receiving ? QStringLiteral("%1: playing, %2 ms buffered")
                                     .arg(Current.Peers[i].Name)
                                     .arg(Incoming[i]->BufferedMs.load())
                               : QStringLiteral("%1: silent").arg(Current.Peers[i].Name));
    }
    lines += Errors;
    return lines.isEmpty() ? QStringLiteral("No connections") : lines.join(QLatin1Char('\n'));
}

void AudioBridge::Bridge::StartPairing(const QString& address)
{
    auto* network = NetworkObject.get();
    QMetaObject::invokeMethod(network, [network, address] { network->StartPairing(address); },
        Qt::QueuedConnection);
}

void AudioBridge::Bridge::ConfirmPairing(const QString& code)
{
    auto* network = NetworkObject.get();
    QMetaObject::invokeMethod(network, [network, code] { network->ConfirmPairing(code); },
        Qt::QueuedConnection);
}

void AudioBridge::Bridge::CancelPairing()
{
    auto* network = NetworkObject.get();
    QMetaObject::invokeMethod(network, [network] { network->CancelPairing(); },
        Qt::QueuedConnection);
}
