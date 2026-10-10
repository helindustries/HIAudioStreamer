#include "Network.h"

#include "Crypto.h"

#include <QtCore/QDateTime>
#include <QtCore/QtEndian>
#include <QtNetwork/QHostInfo>
#include <QtNetwork/QNetworkDatagram>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <ranges>
#include <span>
#include <utility>

// Wire format. Every datagram opens with Magic (u32) and a MessageType (u8); all integers are
// little-endian.
//
//   PairRequest, PairResponse   public key (32) | name length (u8) | name (UTF-8)
//   PairConfirm                 sender key id (8) | confirm tag (16)
//   Audio                       sender key id (8) | nonce (24) | tag (16) | ciphertext
//
// Audio plaintext is sample rate (u32) | frames (u16) | reserved (u16) | frames * 2 s16 samples.
// The nonce is a random per-run salt (16) followed by the packet sequence number (u64), and the
// first 13 bytes of the datagram are bound to the ciphertext as associated data, so a packet
// cannot be replayed under another sender's id.
namespace
{
    constexpr auto PacketMs = 5;
    // Only a backstop, so a stop request is noticed; sending itself is woken by the capture.
    constexpr auto SenderIdleWait = std::chrono::milliseconds(200);
    constexpr auto AskingTimeoutMs = 15000;
    constexpr auto AskedTimeoutMs = 120000;
    constexpr auto MessageHeaderSize = 5;
    constexpr auto TypeOffset = 4;
    constexpr auto AudioHeaderSize = MessageHeaderSize + HIAudioStreamer::KeyIdSize;
    constexpr auto AudioPlainHeaderSize = 8;
    constexpr auto FramesOffset = 4;
    constexpr auto SequenceSize = 8;
    constexpr auto SampleBytes = 2;
    constexpr auto MinRate = 8000;
    constexpr auto MaxRate = 192000;
    constexpr auto DecodeScale = 32768.0f;
    constexpr auto EncodeScale = 32767.0f;

    // The body of a PairRequest or PairResponse.
    struct Hello
    {
        QByteArray PublicKey;
        QString Name;
    };

    QByteArray MessageHeader(HIAudioStreamer::MessageType type)
    {
        auto header = QByteArray(MessageHeaderSize, '\0');
        qToLittleEndian<quint32>(HIAudioStreamer::Magic, header.data());
        header[TypeOffset] = static_cast<char>(type);
        return header;
    }

    // Nothing when the sizes in the body do not add up.
    std::optional<Hello> ParseHello(const QByteArray& body)
    {
        using HIAudioStreamer::KeySize;
        auto hello = std::optional<Hello>();
        if (body.size() > KeySize)
            if (const auto nameSize = static_cast<quint8>(body[KeySize]);
                body.size() == KeySize + 1 + nameSize)
            {
                const auto name = QString::fromUtf8(body.mid(KeySize + 1, nameSize));
                hello = Hello{body.left(KeySize), name};
            }
        return hello;
    }

    // "192.168.1.4" or "192.168.1.4:49710"; a host name is resolved, IPv4 only.
    std::pair<QHostAddress, quint16> ParseAddress(const QString& text, quint16 defaultPort)
    {
        auto host = text.trimmed();
        auto port = defaultPort;
        if (const auto colon = host.lastIndexOf(QLatin1Char(':')); colon > 0)
        {
            port = static_cast<quint16>(host.mid(colon + 1).toUInt());
            host = host.left(colon);
        }
        auto address = QHostAddress(host);
        if (address.isNull() && !host.isEmpty())
        {
            const auto candidates = QHostInfo::fromName(host).addresses();
            if (const auto found = std::ranges::find(candidates, QAbstractSocket::IPv4Protocol,
                                                     &QHostAddress::protocol);
                found != candidates.end())
                address = *found;
        }
        return {address, port};
    }

    int PacketFrames(int rate)
    {
        return std::clamp(rate * PacketMs / 1000, 1, HIAudioStreamer::MaxPacketFrames);
    }

    bool SameHost(const QHostAddress& a, const QHostAddress& b)
    {
        return a.isEqual(b, QHostAddress::ConvertV4MappedToIPv4);
    }

    QString AddressText(const QHostAddress& address, quint16 port)
    {
        const auto host = QHostAddress(address.toIPv4Address()).toString();
        return port == HIAudioStreamer::DefaultPort ? host
                                                    : QStringLiteral("%1:%2").arg(host).arg(port);
    }
} // namespace

HIAudioStreamer::Network::Network(RingBuffer& outgoing,
                                  std::vector<std::unique_ptr<Playout>>& incoming, LinkStats& stats)
    : Outgoing(outgoing)
    , Incoming(incoming)
    , Stats(stats)
    , Socket(this)
    , AskingTimeout(this)
    , AskedTimeout(this)
    , Heartbeat(this)
    , Frames(MaxPacketFrames * Channels)
    , SendFrames(MaxPacketFrames * Channels)
{
    AskingTimeout.setSingleShot(true);
    AskingTimeout.setInterval(AskingTimeoutMs);
    AskedTimeout.setSingleShot(true);
    AskedTimeout.setInterval(AskedTimeoutMs);
    Heartbeat.setInterval(HeartbeatMs);
    connect(&Socket, &QUdpSocket::readyRead, this, &Network::HandleReadyRead);
    connect(&Heartbeat, &QTimer::timeout, this, &Network::SendHeartbeats);
    connect(&AskingTimeout, &QTimer::timeout, this, [this] {
        Asking.Active = false;
        emit PairingFailed(QStringLiteral("No answer from %1. Is HIAudioStreamer running there?")
                               .arg(Asking.Entered));
    });
    connect(&AskedTimeout, &QTimer::timeout, this, [this] {
        Asked.Active = false;
        emit PairingFailed(QStringLiteral("Pairing with %1 timed out").arg(Asked.Name));
    });
    Sender = std::thread([this] { SendLoop(); });
}

HIAudioStreamer::Network::~Network()
{
    StopSending = true;
    Wake.release();
    if (Sender.joinable()) Sender.join();
}

void HIAudioStreamer::Network::NotifyCaptured(int rate)
{
    if (Outgoing.Available() >= static_cast<std::size_t>(PacketFrames(rate))) Wake.release();
}

void HIAudioStreamer::Network::Configure(const HIAudioStreamer::Config& config)
{
    Current = config;
    OwnKeyId = KeyId(config.Self.Public);
    Remotes.clear();
    for (const auto& peer : config.Peers)
    {
        const auto [address, port] = ParseAddress(peer.Address, config.Port);
        if (address.isNull()) emit Failed(QStringLiteral("Cannot resolve %1").arg(peer.Address));
        Remotes.push_back({KeyId(peer.PublicKey), peer.Key, address, port, peer.Send, {}, 0});
    }
    if (Socket.state() != QAbstractSocket::BoundState || Socket.localPort() != config.Port)
    {
        Socket.close();
        if (!Socket.bind(QHostAddress::AnyIPv4, config.Port))
        {
            emit Failed(QStringLiteral("Cannot listen on UDP port %1: %2")
                            .arg(config.Port)
                            .arg(Socket.errorString()));
        }
    }

    auto targets = std::vector<Target>();
    for (const auto& remote : Remotes)
        if (remote.Send && !remote.Address.isNull())
            targets.push_back({remote.Key, remote.Address, remote.Port});
    {
        const auto lock = std::scoped_lock(SendMutex);
        Targets = std::move(targets);
        SendKeyId = OwnKeyId;
        // A fresh salt per configuration keeps every nonce unique without resetting the sequence.
        SendSalt = RandomBytes(SaltSize);
        SendEnabled = config.Send;
    }
    Heartbeat.start();
    SendHeartbeats();
}

int HIAudioStreamer::Network::SlotFor(const QByteArray& keyId) const
{
    const auto candidates = Remotes | std::views::take(MaxPeers);
    const auto found = std::ranges::find(candidates, keyId, &Remote::KeyId);
    return found != candidates.end() ? static_cast<int>(found - candidates.begin()) : -1;
}

// Sent whether or not anything plays, to every peer: it says this side is up and would play,
// which is all the sender needs to decide whether to silence its own output.
void HIAudioStreamer::Network::SendHeartbeats()
{
    const auto header = MessageHeader(MessageType::Listening) + OwnKeyId;
    for (const auto& remote : Remotes)
        if (!remote.Address.isNull())
        {
            const auto nonce = RandomBytes(NonceSize);
            Socket.writeDatagram(header + nonce + Seal(remote.Key, nonce, header, QByteArray()),
                                 remote.Address, remote.Port);
        }
}

void HIAudioStreamer::Network::HandleListening(const QByteArray& header, const QByteArray& body)
{
    if (const auto slot = SlotFor(header.mid(MessageHeaderSize));
        slot >= 0 && body.size() == NonceSize + MacSize
        && Open(Remotes[slot].Key, body.left(NonceSize), header, body.mid(NonceSize)))
    {
        Stats.Peers[slot].LastListeningMs.store(QDateTime::currentMSecsSinceEpoch(),
                                                std::memory_order_relaxed);
    }
    else Stats.PacketsRejected.fetch_add(1, std::memory_order_relaxed);
}

void HIAudioStreamer::Network::StartPairing(const QString& address)
{
    if (const auto [host, port] = ParseAddress(address, Current.Port); !host.isNull())
    {
        Asking =
            Pending{.Address = host, .Port = port, .Entered = address.trimmed(), .Active = true};
        SendHello(MessageType::PairRequest, host, port);
        AskingTimeout.start();
    }
    else emit PairingFailed(QStringLiteral("Cannot resolve %1").arg(address));
}

void HIAudioStreamer::Network::ConfirmPairing(const QString& code)
{
    if (Asking.Active && !Asking.Code.isEmpty())
    {
        Asking.Active = false;
        if (code.trimmed() == Asking.Code)
        {
            const auto datagram = MessageHeader(MessageType::PairConfirm) + OwnKeyId
                                  + ConfirmTag(Asking.Key, Current.Self.Public);
            Socket.writeDatagram(datagram, Asking.Address, Asking.Port);
            emit Paired({Asking.Name, Asking.Entered, Asking.PublicKey, Asking.Key, true});
        }
        else emit PairingFailed(QStringLiteral("The code does not match. Nothing was paired."));
    }
}

void HIAudioStreamer::Network::CancelPairing()
{
    Asking.Active = false;
    Asked.Active = false;
    AskingTimeout.stop();
    AskedTimeout.stop();
}

void HIAudioStreamer::Network::Shutdown()
{
    StopSending = true;
    Wake.release();
    if (Sender.joinable()) Sender.join();
    Heartbeat.stop();
    AskingTimeout.stop();
    AskedTimeout.stop();
    Socket.close();
}

void HIAudioStreamer::Network::SendHello(MessageType type, const QHostAddress& to, quint16 port)
{
    const auto name = Current.Name.toUtf8().left(MaxNameBytes);
    auto datagram = MessageHeader(type) + Current.Self.Public;
    datagram.append(static_cast<char>(name.size()));
    datagram.append(name);
    Socket.writeDatagram(datagram, to, port);
}

void HIAudioStreamer::Network::HandleReadyRead()
{
    while (Socket.hasPendingDatagrams())
    {
        const auto datagram = Socket.receiveDatagram();
        const auto data = datagram.data();
        const auto body = data.mid(MessageHeaderSize);
        const auto from = datagram.senderAddress();
        const auto port = static_cast<quint16>(datagram.senderPort());
        switch (const auto valid = data.size() >= MessageHeaderSize
                                   && qFromLittleEndian<quint32>(data.constData()) == Magic;
                valid ? static_cast<MessageType>(data[TypeOffset]) : MessageType{})
        {
            case MessageType::PairRequest:
                HandlePairRequest(body, from, port);
                break;
            case MessageType::PairResponse:
                HandlePairResponse(body, from, port);
                break;
            case MessageType::PairConfirm:
                HandlePairConfirm(body, from);
                break;
            case MessageType::Audio:
                HandleAudio(data.left(AudioHeaderSize), data.mid(AudioHeaderSize));
                break;
            case MessageType::Listening:
                HandleListening(data.left(AudioHeaderSize), data.mid(AudioHeaderSize));
                break;
            default:
                Stats.PacketsRejected.fetch_add(1, std::memory_order_relaxed);
                break;
        }
    }
}

void HIAudioStreamer::Network::HandlePairRequest(const QByteArray& body, const QHostAddress& from,
                                                 quint16 port)
{
    // One pairing at a time, so nobody on the network can stack up prompts.
    if (const auto hello = ParseHello(body); hello && !Asked.Active)
    {
        const auto& [publicKey, name] = *hello;
        const auto key = SharedKey(Current.Self, publicKey);
        Asked = Pending{.Address = from,
                        .Port = port,
                        .Name = name,
                        .PublicKey = publicKey,
                        .Key = key,
                        .Code = PairingCode(key),
                        .Active = true};
        SendHello(MessageType::PairResponse, from, port);
        AskedTimeout.start();
        emit PairingCodeShown(name, AddressText(from, port), Asked.Code);
    }
    else Stats.PacketsRejected.fetch_add(1, std::memory_order_relaxed);
}

void HIAudioStreamer::Network::HandlePairResponse(const QByteArray& body, const QHostAddress& from,
                                                  quint16 port)
{
    if (const auto hello = ParseHello(body);
        hello && Asking.Active && Asking.Code.isEmpty() && SameHost(from, Asking.Address))
    {
        const auto& [publicKey, name] = *hello;
        AskingTimeout.stop();
        Asking.Port = port;
        Asking.PublicKey = publicKey;
        Asking.Name = name;
        Asking.Key = SharedKey(Current.Self, publicKey);
        Asking.Code = PairingCode(Asking.Key);
        emit PairingCodeNeeded(name, Asking.Entered);
    }
    else Stats.PacketsRejected.fetch_add(1, std::memory_order_relaxed);
}

void HIAudioStreamer::Network::HandlePairConfirm(const QByteArray& body, const QHostAddress& from)
{
    if (Asked.Active && SameHost(from, Asked.Address) && body.size() == KeyIdSize + MacSize
        && body.left(KeyIdSize) == KeyId(Asked.PublicKey)
        && ConfirmTagValid(Asked.Key, Asked.PublicKey, body.mid(KeyIdSize)))
    {
        Asked.Active = false;
        AskedTimeout.stop();
        emit Paired(
            {Asked.Name, AddressText(Asked.Address, Asked.Port), Asked.PublicKey, Asked.Key, true});
    }
    else Stats.PacketsRejected.fetch_add(1, std::memory_order_relaxed);
}

void HIAudioStreamer::Network::HandleAudio(const QByteArray& header, const QByteArray& body)
{
    auto accepted = false;
    if (const auto slot = SlotFor(header.mid(MessageHeaderSize));
        slot >= 0 && body.size() > NonceSize + MacSize)
    {
        const auto nonce = body.left(NonceSize);
        if (const auto plain = Open(Remotes[slot].Key, nonce, header, body.mid(NonceSize));
            plain && plain->size() >= AudioPlainHeaderSize)
        {
            const auto rate = static_cast<int>(qFromLittleEndian<quint32>(plain->constData()));
            const auto frames = qFromLittleEndian<quint16>(plain->constData() + FramesOffset);
            const auto samples = static_cast<std::size_t>(frames) * Channels;
            const auto salt = nonce.left(SaltSize);
            const auto sequence = qFromLittleEndian<quint64>(nonce.constData() + SaltSize);
            auto& remote = Remotes[slot];
            // A new salt means the sender restarted; within one run, an old sequence number is
            // a replay.
            if (frames > 0 && frames <= MaxPacketFrames && rate >= MinRate && rate <= MaxRate
                && static_cast<std::size_t>(plain->size())
                       == AudioPlainHeaderSize + samples * SampleBytes
                && (salt != remote.Salt || sequence > remote.LastSequence))
            {
                remote.Salt = salt;
                remote.LastSequence = sequence;
                const auto wire = std::span(reinterpret_cast<const qint16_le*>(
                                                plain->constData() + AudioPlainHeaderSize),
                                            samples);
                const auto decoded = std::span(Frames).first(samples);
                std::ranges::transform(wire, decoded.begin(),
                                       [](qint16 sample) { return sample / DecodeScale; });
                Incoming[slot]->Push(decoded.data(), frames, rate);

                auto& [lastReceivedMs, packetsReceived, lastListeningMs, receivedPeak] =
                    Stats.Peers[slot];
                RecordPeak(receivedPeak, decoded);
                lastReceivedMs.store(QDateTime::currentMSecsSinceEpoch(),
                                     std::memory_order_relaxed);
                packetsReceived.fetch_add(1, std::memory_order_relaxed);
                accepted = true;
            }
        }
    }
    if (!accepted) Stats.PacketsRejected.fetch_add(1, std::memory_order_relaxed);
}

void HIAudioStreamer::Network::SendLoop()
{
    // Its own socket, created on this thread: QUdpSocket may not be shared across threads, and a
    // plain write needs no event loop. It sends from an ephemeral port, which nothing checks;
    // the receiver identifies senders by key, not by address.
    auto socket = QUdpSocket();
    while (!StopSending)
        if (Wake.try_acquire_for(SenderIdleWait)) SendPending(socket);
}

void HIAudioStreamer::Network::SendPending(QUdpSocket& socket)
{
    const auto rate = Stats.CaptureRate.load(std::memory_order_relaxed);
    const auto packetFrames = PacketFrames(rate);
    const auto samples = static_cast<std::size_t>(packetFrames) * Channels;
    const auto lock = std::scoped_lock(SendMutex);
    if (SendEnabled && rate > 0 && !Targets.empty())
    {
        while (Outgoing.Available() >= static_cast<std::size_t>(packetFrames))
        {
            Outgoing.Read(SendFrames.data(), packetFrames);
            auto plain =
                QByteArray(static_cast<qsizetype>(AudioPlainHeaderSize + samples * SampleBytes),
                           '\0');
            qToLittleEndian<quint32>(static_cast<quint32>(rate), plain.data());
            qToLittleEndian<quint16>(static_cast<quint16>(packetFrames),
                                     plain.data() + FramesOffset);
            std::ranges::transform(std::span(SendFrames).first(samples),
                                   reinterpret_cast<qint16_le*>(plain.data()
                                                                + AudioPlainHeaderSize),
                                   [](float sample) {
                                       return static_cast<qint16>(std::lround(
                                           std::clamp(sample, -1.0f, 1.0f) * EncodeScale));
                                   });

            auto nonce = SendSalt + QByteArray(SequenceSize, '\0');
            qToLittleEndian<quint64>(++Sequence, nonce.data() + SaltSize);
            const auto header = MessageHeader(MessageType::Audio) + SendKeyId;
            for (const auto& [key, address, port] : Targets)
            {
                socket.writeDatagram(header + nonce + Seal(key, nonce, header, plain), address,
                                     port);
                Stats.PacketsSent.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    else Outgoing.Skip(Outgoing.Available());
}
