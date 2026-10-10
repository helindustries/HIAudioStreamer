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

HIAudioStreamer::Bridge::Bridge()
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

HIAudioStreamer::Bridge::~Bridge()
{
    Mute.reset();
    Capture.reset();
    Render.reset();
    auto* network = NetworkObject.get();
    QMetaObject::invokeMethod(network, [network] { network->Shutdown(); },
        Qt::BlockingQueuedConnection);
    NetworkThread.quit();
    NetworkThread.wait();
}

void HIAudioStreamer::Bridge::Apply(const Config& config)
{
    Current = config;
    Errors.clear();
    // Released with the streams it belongs to; UpdateLocalMute takes it again within a second
    // if the new configuration still streams, now on whichever output is the default.
    Mute.reset();
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
            RecordPeak(Stats.CapturePeak, frames, count * Channels);
            Outgoing.Write(frames, count);
            NetworkObject->NotifyCaptured(rate);
        };
        Capture = config.Source.isEmpty() ? StartSystemCapture(onFrames)
                                          : StartDeviceCapture(config.Source, onFrames);
    }
    for (const auto* stream : {Render.get(), Capture.get()})
        if (stream != nullptr && !stream->Error().isEmpty()) Errors.append(stream->Error());
}

void HIAudioStreamer::Bridge::UpdateLocalMute()
{
    const auto now = QDateTime::currentMSecsSinceEpoch();
    auto listened = false;
    for (auto i = 0; i < std::min<int>(MaxPeers, Current.Peers.size()); ++i)
        listened = listened
                   || (Current.Peers[i].Send && now - Stats.LastListeningMs[i].load() < ListeningTimeoutMs);
    // Only the system mix is moved elsewhere; sending a microphone leaves the speakers alone.
    const auto streaming = Current.Send && Current.Source.isEmpty() && listened
                           && Capture != nullptr && Capture->Error().isEmpty();
    if (streaming && Mute == nullptr)
    {
        Mute = MuteLocalOutput();
        if (!Mute->Error().isEmpty()) emit Failed(Mute->Error());
    }
    else if (!streaming && Mute != nullptr)
        Mute.reset();
}

void HIAudioStreamer::Bridge::ReleaseLocalMute()
{
    Mute.reset();
}

void HIAudioStreamer::Bridge::Mix(float* frames, std::size_t count, int rate)
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

bool HIAudioStreamer::Bridge::Active() const
{
    const auto now = QDateTime::currentMSecsSinceEpoch();
    auto receiving = false;
    for (auto i = 0; i < std::min<int>(MaxPeers, Current.Peers.size()); ++i)
        receiving = receiving || now - Stats.LastReceivedMs[i].load() < SilentAfterMs;
    return receiving || (Current.Send && Stats.CaptureRate.load() > 0);
}

QString HIAudioStreamer::Bridge::Status() const
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
    if (Mute != nullptr && Mute->Error().isEmpty())
        lines.append(QStringLiteral("Local output muted while streaming"));
    lines += Errors;
    return lines.isEmpty() ? QStringLiteral("No connections") : lines.join(QLatin1Char('\n'));
}

QString HIAudioStreamer::Bridge::Diagnostics()
{
    const auto now = QDateTime::currentMSecsSinceEpoch();
    auto line = QStringLiteral("send %1 local %2 capture %3 Hz peak %4 sent %5 rejected %6")
                    .arg(Current.Send ? QStringLiteral("on") : QStringLiteral("off"))
                    .arg(Mute != nullptr ? QStringLiteral("muted") : QStringLiteral("playing"))
                    .arg(Stats.CaptureRate.load())
                    .arg(Stats.CapturePeak.exchange(0) / 1000.0, 0, 'f', 3)
                    .arg(Stats.PacketsSent.load())
                    .arg(Stats.PacketsRejected.load());
    for (auto i = 0; i < std::min<int>(MaxPeers, Current.Peers.size()); ++i)
    {
        const auto last = Stats.LastReceivedMs[i].load();
        const auto listening = Stats.LastListeningMs[i].load();
        line += QStringLiteral(" | %1 listening %2 received %3 peak %4 last %5 buffered %6 ms underruns %7")
                    .arg(Current.Peers[i].Name)
                    .arg(listening > 0 ? QStringLiteral("%1 ms ago").arg(now - listening) : QStringLiteral("never"))
                    .arg(Stats.PacketsReceived[i].load())
                    .arg(Stats.ReceivedPeak[i].exchange(0) / 1000.0, 0, 'f', 3)
                    .arg(last > 0 ? QStringLiteral("%1 ms ago").arg(now - last) : QStringLiteral("never"))
                    .arg(Incoming[i]->BufferedMs.load())
                    .arg(Incoming[i]->Underruns.load());
    }
    return line + (Errors.isEmpty() ? QString() : QStringLiteral(" | errors: ") + Errors.join("; "));
}

void HIAudioStreamer::Bridge::StartPairing(const QString& address)
{
    auto* network = NetworkObject.get();
    QMetaObject::invokeMethod(network, [network, address] { network->StartPairing(address); },
        Qt::QueuedConnection);
}

void HIAudioStreamer::Bridge::ConfirmPairing(const QString& code)
{
    auto* network = NetworkObject.get();
    QMetaObject::invokeMethod(network, [network, code] { network->ConfirmPairing(code); },
        Qt::QueuedConnection);
}

void HIAudioStreamer::Bridge::CancelPairing()
{
    auto* network = NetworkObject.get();
    QMetaObject::invokeMethod(network, [network] { network->CancelPairing(); },
        Qt::QueuedConnection);
}
