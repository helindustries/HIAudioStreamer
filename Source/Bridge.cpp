#include "Bridge.h"

#include <QtCore/QDateTime>

#include <algorithm>
#include <iterator>
#include <ranges>
#include <span>

namespace
{
    // One second at the highest rate a device is likely to run, per direction.
    constexpr auto BufferFrames = std::size_t{192000};
    constexpr auto SilentAfterMs = 1000;
} // namespace

HIAudioStreamer::Bridge::Bridge()
    : Outgoing(BufferFrames)
    , NetworkObject(std::make_unique<Network>(Outgoing, Incoming, Stats))
{
    std::ranges::generate_n(std::back_inserter(Incoming), MaxPeers,
                            [] { return std::make_unique<Playout>(BufferFrames); });

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
    OnNetwork([](auto& network) { network.Shutdown(); }, Qt::BlockingQueuedConnection);
    NetworkThread.quit();
    NetworkThread.wait();
}

void HIAudioStreamer::Bridge::OnNetwork(std::function<void(Network&)> work, Qt::ConnectionType type)
{
    auto* network = NetworkObject.get();
    QMetaObject::invokeMethod(network, [network, work = std::move(work)] { work(*network); }, type);
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
    OnNetwork([config](auto& network) { network.Configure(config); });

    // Output first: on macOS the system tap can only exclude this process once the process has
    // an audio object, which playing gives it.
    Render = StartRender(config.Output, [this](float* frames, std::size_t count, int rate) {
        Mix(frames, count, rate);
    });
    if (config.Send)
    {
        const auto onFrames = [this](const float* frames, std::size_t count, int rate) {
            Stats.CaptureRate.store(rate, std::memory_order_relaxed);
            RecordPeak(Stats.CapturePeak, std::span(frames, count * Channels));
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
    for (auto slot = std::size_t{0}; const auto& peer : Current.Peers | std::views::take(MaxPeers))
    {
        const auto silentFor = now - Stats.Peers[slot++].LastListeningMs.load();
        listened = listened || (peer.Send && silentFor < ListeningTimeoutMs);
    }
    // Only the system mix is moved elsewhere; sending a microphone leaves the speakers alone.
    const auto streaming = Current.Send && Current.Source.isEmpty() && listened
                           && Capture != nullptr && Capture->Error().isEmpty();
    if (streaming && Mute == nullptr)
    {
        Mute = MuteLocalOutput();
        if (const auto error = Mute->Error(); !error.isEmpty()) emit Failed(error);
    }
    else if (!streaming && Mute != nullptr) Mute.reset();
}

void HIAudioStreamer::Bridge::ReleaseLocalMute()
{
    Mute.reset();
}

void HIAudioStreamer::Bridge::Mix(float* frames, std::size_t count, int rate)
{
    for (auto& playout : Incoming)
        playout->MixInto(frames, count, rate);
    for (auto& sample : std::span(frames, count * Channels))
        sample = std::clamp(sample, -1.0f, 1.0f);
}

bool HIAudioStreamer::Bridge::Active() const
{
    const auto now = QDateTime::currentMSecsSinceEpoch();
    const auto receiving = std::ranges::any_of(Stats.Peers | std::views::take(Current.Peers.size()),
                                               [now](const auto& peer) {
                                                   return now - peer.LastReceivedMs.load()
                                                          < SilentAfterMs;
                                               });
    return receiving || (Current.Send && Stats.CaptureRate.load() > 0);
}

QString HIAudioStreamer::Bridge::Status() const
{
    const auto now = QDateTime::currentMSecsSinceEpoch();
    auto lines = QStringList();
    if (Current.Send)
    {
        const auto rate = Stats.CaptureRate.load();
        const auto targets = std::ranges::count(Current.Peers, true, &Peer::Send);
        const auto source = Current.Source.isEmpty() ? QStringLiteral("system audio")
                                                     : QStringLiteral("input");
        lines.append(rate > 0 && targets > 0 ? QStringLiteral("Sending %1 to %2 at %3 kHz")
                                                   .arg(source)
                                                   .arg(targets)
                                                   .arg(rate / 1000.0)
                                             : QStringLiteral("Sending: nothing playing yet"));
    }
    for (auto slot = std::size_t{0}; const auto& peer : Current.Peers | std::views::take(MaxPeers))
    {
        const auto receiving = now - Stats.Peers[slot].LastReceivedMs.load() < SilentAfterMs;
        const auto buffered = Incoming[slot++]->BufferedMs.load();
        lines.append(
            receiving ? QStringLiteral("%1: playing, %2 ms buffered").arg(peer.Name).arg(buffered)
                      : QStringLiteral("%1: silent").arg(peer.Name));
    }
    if (Mute != nullptr && Mute->Error().isEmpty())
        lines.append(QStringLiteral("Local output muted while streaming"));
    lines += Errors;
    return lines.isEmpty() ? QStringLiteral("No connections") : lines.join(QLatin1Char('\n'));
}

QString HIAudioStreamer::Bridge::Diagnostics()
{
    const auto now = QDateTime::currentMSecsSinceEpoch();
    const auto ago = [now](qint64 ms) {
        return ms > 0 ? QStringLiteral("%1 ms ago").arg(now - ms) : QStringLiteral("never");
    };
    auto line = QStringLiteral("send %1 local %2 capture %3 Hz peak %4 sent %5 rejected %6")
                    .arg(Current.Send ? QStringLiteral("on") : QStringLiteral("off"))
                    .arg(Mute != nullptr ? QStringLiteral("muted") : QStringLiteral("playing"))
                    .arg(Stats.CaptureRate.load())
                    .arg(Stats.CapturePeak.exchange(0) / PeakScale, 0, 'f', 3)
                    .arg(Stats.PacketsSent.load())
                    .arg(Stats.PacketsRejected.load());
    for (auto slot = std::size_t{0}; const auto& peer : Current.Peers | std::views::take(MaxPeers))
    {
        auto& [lastReceivedMs, packetsReceived, lastListeningMs, receivedPeak] = Stats.Peers[slot];
        const auto& playout = *Incoming[slot++];
        line += QStringLiteral(" | %1 listening %2 received %3 peak %4 last %5 buffered %6 ms "
                               "underruns %7")
                    .arg(peer.Name)
                    .arg(ago(lastListeningMs.load()))
                    .arg(packetsReceived.load())
                    .arg(receivedPeak.exchange(0) / PeakScale, 0, 'f', 3)
                    .arg(ago(lastReceivedMs.load()))
                    .arg(playout.BufferedMs.load())
                    .arg(playout.Underruns.load());
    }
    if (!Errors.isEmpty())
        line += QStringLiteral(" | errors: ") + Errors.join(QStringLiteral("; "));
    return line;
}

void HIAudioStreamer::Bridge::StartPairing(const QString& address)
{
    OnNetwork([address](auto& network) { network.StartPairing(address); });
}

void HIAudioStreamer::Bridge::ConfirmPairing(const QString& code)
{
    OnNetwork([code](auto& network) { network.ConfirmPairing(code); });
}

void HIAudioStreamer::Bridge::CancelPairing()
{
    OnNetwork([](auto& network) { network.CancelPairing(); });
}
