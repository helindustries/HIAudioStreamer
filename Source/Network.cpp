#include "Network.h"

#include "Crypto.h"
#include "ThirdParty/Monocypher/monocypher.h"

#include <QtCore/QDateTime>
#include <QtCore/QtEndian>
#include <QtNetwork/QHostInfo>
#include <QtNetwork/QNetworkDatagram>

#include <algorithm>
#include <cmath>
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
    constexpr auto AudioHeaderSize = MessageHeaderSize + HIAudioStreamer::KeyIdSize;
    constexpr auto AudioPlainHeaderSize = 8;

    QByteArray MessageHeader(HIAudioStreamer::MessageType type)
    {
        auto header = QByteArray(MessageHeaderSize, '\0');
        qToLittleEndian<quint32>(HIAudioStreamer::Magic, header.data());
        header[4] = static_cast<char>(type);
        return header;
    }

    // "192.168.1.4" or "192.168.1.4:49710"; a host name is resolved, IPv4 only.
    std::pair<QHostAddress, quint16> ParseAddress(const QString& text, quint16 defaultPort)
    {
        auto host = text.trimmed();
        auto port = defaultPort;
        const auto colon = host.lastIndexOf(QLatin1Char(':'));
        if (colon > 0)
        {
            port = static_cast<quint16>(host.mid(colon + 1).toUInt());
            host = host.left(colon);
        }
        auto address = QHostAddress(host);
        if (address.isNull() && !host.isEmpty())
            for (const auto& candidate : QHostInfo::fromName(host).addresses())
                if (address.isNull() && candidate.protocol() == QAbstractSocket::IPv4Protocol)
                    address = candidate;
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
        return port == HIAudioStreamer::DefaultPort ? host : QStringLiteral("%1:%2").arg(host).arg(port);
    }
} // namespace

HIAudioStreamer::Network::Network(RingBuffer& outgoing, std::vector<std::unique_ptr<Playout>>& incoming,
    LinkStats& stats)
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
            emit Failed(QStringLiteral("Cannot listen on UDP port %1: %2")
                            .arg(config.Port)
                            .arg(Socket.errorString()));
    }

    auto targets = std::vector<Target>();
    for (auto i = std::size_t{0}; i < Remotes.size(); ++i)
        if (config.Peers[static_cast<qsizetype>(i)].Send && !Remotes[i].Address.isNull())
            targets.push_back({Remotes[i].Key, Remotes[i].Address, Remotes[i].Port});
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
    auto slot = -1;
    for (auto i = 0; i < std::min<int>(MaxPeers, static_cast<int>(Remotes.size())) && slot < 0; ++i)
        if (Remotes[i].KeyId == keyId) slot = i;
    return slot;
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
    const auto slot = SlotFor(header.mid(MessageHeaderSize));
    if (slot >= 0 && body.size() == NonceSize + MacSize
        && Open(Remotes[slot].Key, body.left(NonceSize), header, body.mid(NonceSize)))
        Stats.LastListeningMs[slot].store(QDateTime::currentMSecsSinceEpoch(),
            std::memory_order_relaxed);
    else
        Stats.PacketsRejected.fetch_add(1, std::memory_order_relaxed);
}

void HIAudioStreamer::Network::StartPairing(const QString& address)
{
    const auto [host, port] = ParseAddress(address, Current.Port);
    if (host.isNull())
        emit PairingFailed(QStringLiteral("Cannot resolve %1").arg(address));
    else
    {
        Asking = Pending();
        Asking.Address = host;
        Asking.Port = port;
        Asking.Entered = address.trimmed();
        Asking.Active = true;
        SendHello(MessageType::PairRequest, host, port);
        AskingTimeout.start();
    }
}

void HIAudioStreamer::Network::ConfirmPairing(const QString& code)
{
    if (Asking.Active && !Asking.Code.isEmpty())
    {
        Asking.Active = false;
        if (code.trimmed() == Asking.Code)
        {
            auto datagram = MessageHeader(MessageType::PairConfirm) + OwnKeyId
                            + ConfirmTag(Asking.Key, Current.Self.Public);
            Socket.writeDatagram(datagram, Asking.Address, Asking.Port);
            emit Paired({Asking.Name, Asking.Entered, Asking.PublicKey, Asking.Key, true});
        }
        else
            emit PairingFailed(QStringLiteral("The code does not match. Nothing was paired."));
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
        const auto valid = data.size() >= MessageHeaderSize
                           && qFromLittleEndian<quint32>(data.constData()) == Magic;
        const auto type = valid ? static_cast<MessageType>(data[4]) : MessageType{};
        const auto body = data.mid(MessageHeaderSize);
        const auto from = datagram.senderAddress();
        const auto port = static_cast<quint16>(datagram.senderPort());
        switch (type)
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
    const auto nameSize = body.size() > KeySize ? static_cast<quint8>(body[KeySize]) : 0;
    // One pairing at a time, so nobody on the network can stack up prompts.
    if (!Asked.Active && body.size() >= KeySize + 1 && body.size() == KeySize + 1 + nameSize)
    {
        Asked = Pending();
        Asked.Address = from;
        Asked.Port = port;
        Asked.PublicKey = body.left(KeySize);
        Asked.Name = QString::fromUtf8(body.mid(KeySize + 1, nameSize));
        Asked.Key = SharedKey(Current.Self, Asked.PublicKey);
        Asked.Code = PairingCode(Asked.Key);
        Asked.Active = true;
        SendHello(MessageType::PairResponse, from, port);
        AskedTimeout.start();
        emit PairingCodeShown(Asked.Name, AddressText(from, port), Asked.Code);
    }
}

void HIAudioStreamer::Network::HandlePairResponse(const QByteArray& body, const QHostAddress& from,
    quint16 port)
{
    const auto nameSize = body.size() > KeySize ? static_cast<quint8>(body[KeySize]) : 0;
    if (Asking.Active && Asking.Code.isEmpty() && SameHost(from, Asking.Address)
        && body.size() >= KeySize + 1 && body.size() == KeySize + 1 + nameSize)
    {
        AskingTimeout.stop();
        Asking.Port = port;
        Asking.PublicKey = body.left(KeySize);
        Asking.Name = QString::fromUtf8(body.mid(KeySize + 1, nameSize));
        Asking.Key = SharedKey(Current.Self, Asking.PublicKey);
        Asking.Code = PairingCode(Asking.Key);
        emit PairingCodeNeeded(Asking.Name, Asking.Entered);
    }
}

void HIAudioStreamer::Network::HandlePairConfirm(const QByteArray& body, const QHostAddress& from)
{
    if (Asked.Active && SameHost(from, Asked.Address) && body.size() == KeyIdSize + MacSize
        && body.left(KeyIdSize) == KeyId(Asked.PublicKey))
    {
        const auto expected = ConfirmTag(Asked.Key, Asked.PublicKey);
        const auto* tag = reinterpret_cast<const std::uint8_t*>(body.constData() + KeyIdSize);
        if (crypto_verify16(tag, reinterpret_cast<const std::uint8_t*>(expected.constData())) == 0)
        {
            Asked.Active = false;
            AskedTimeout.stop();
            emit Paired({Asked.Name, AddressText(Asked.Address, Asked.Port), Asked.PublicKey,
                Asked.Key, true});
        }
    }
}

void HIAudioStreamer::Network::HandleAudio(const QByteArray& header, const QByteArray& body)
{
    const auto slot = SlotFor(header.mid(MessageHeaderSize));

    const auto nonce = body.left(NonceSize);
    const auto plain = slot >= 0 && slot < MaxPeers && body.size() > NonceSize + MacSize
                           ? Open(Remotes[slot].Key, nonce, header, body.mid(NonceSize))
                           : std::nullopt;
    const auto frames = plain && plain->size() >= AudioPlainHeaderSize
                            ? qFromLittleEndian<quint16>(plain->constData() + 4)
                            : 0;
    const auto rate = plain ? static_cast<int>(qFromLittleEndian<quint32>(plain->constData())) : 0;
    const auto sized = plain && frames > 0 && frames <= MaxPacketFrames
                       && plain->size() == AudioPlainHeaderSize + frames * Channels * 2
                       && rate >= 8000 && rate <= 192000;

    // A new salt means the sender restarted; within one run, an old sequence number is a replay.
    auto fresh = false;
    if (sized)
    {
        auto& remote = Remotes[slot];
        const auto salt = nonce.left(SaltSize);
        const auto sequence = qFromLittleEndian<quint64>(nonce.constData() + SaltSize);
        fresh = salt != remote.Salt || sequence > remote.LastSequence;
        if (fresh)
        {
            remote.Salt = salt;
            remote.LastSequence = sequence;
        }
    }

    if (fresh)
    {
        const auto* samples = plain->constData() + AudioPlainHeaderSize;
        for (auto i = 0; i < frames * Channels; ++i)
            Frames[i] = qFromLittleEndian<qint16>(samples + i * 2) / 32768.0f;
        Incoming[slot]->Push(Frames.data(), frames, rate);
        RecordPeak(Stats.ReceivedPeak[slot], Frames.data(), static_cast<std::size_t>(frames) * Channels);
        Stats.LastReceivedMs[slot].store(QDateTime::currentMSecsSinceEpoch(),
            std::memory_order_relaxed);
        Stats.PacketsReceived[slot].fetch_add(1, std::memory_order_relaxed);
    }
    else
        Stats.PacketsRejected.fetch_add(1, std::memory_order_relaxed);
}

void HIAudioStreamer::Network::SendLoop()
{
    // Its own socket, created on this thread: QUdpSocket may not be shared across threads, and a
    // plain write needs no event loop. It sends from an ephemeral port, which nothing checks;
    // the receiver identifies senders by key, not by address.
    auto socket = QUdpSocket();
    while (!StopSending)
    {
        if (Wake.try_acquire_for(SenderIdleWait)) SendPending(socket);
    }
}

void HIAudioStreamer::Network::SendPending(QUdpSocket& socket)
{
    const auto rate = Stats.CaptureRate.load(std::memory_order_relaxed);
    const auto packetFrames = PacketFrames(rate);
    const auto lock = std::scoped_lock(SendMutex);
    if (!SendEnabled || rate <= 0 || Targets.empty())
        Outgoing.Skip(Outgoing.Available());
    else
        while (Outgoing.Available() >= static_cast<std::size_t>(packetFrames))
        {
            Outgoing.Read(SendFrames.data(), packetFrames);
            auto plain = QByteArray(AudioPlainHeaderSize + packetFrames * Channels * 2, '\0');
            auto* bytes = plain.data();
            qToLittleEndian<quint32>(static_cast<quint32>(rate), bytes);
            qToLittleEndian<quint16>(static_cast<quint16>(packetFrames), bytes + 4);
            for (auto i = 0; i < packetFrames * Channels; ++i)
            {
                const auto sample = std::clamp(SendFrames[i], -1.0f, 1.0f);
                qToLittleEndian<qint16>(static_cast<qint16>(std::lround(sample * 32767.0f)),
                    bytes + AudioPlainHeaderSize + i * 2);
            }

            auto nonce = SendSalt + QByteArray(8, '\0');
            qToLittleEndian<quint64>(++Sequence, nonce.data() + SaltSize);
            const auto header = MessageHeader(MessageType::Audio) + SendKeyId;
            for (const auto& target : Targets)
            {
                const auto datagram = header + nonce + Seal(target.Key, nonce, header, plain);
                socket.writeDatagram(datagram, target.Address, target.Port);
                Stats.PacketsSent.fetch_add(1, std::memory_order_relaxed);
            }
        }
}
