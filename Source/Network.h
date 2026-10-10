#pragma once

#include "Config.h"
#include "Playout.h"
#include "RingBuffer.h"

#include <QtCore/QObject>
#include <QtCore/QTimer>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QUdpSocket>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace AudioBridge
{
    struct LinkStats
    {
        std::atomic<int> CaptureRate{0};
        std::atomic<quint64> PacketsSent{0};
        std::atomic<quint64> PacketsRejected{0};
        std::array<std::atomic<qint64>, MaxPeers> LastReceivedMs{};
    };

    // Lives on its own thread: on macOS the event loop stops while a tray menu is open, and audio
    // carried on that loop would drop out for as long as the menu stays up. Everything public is
    // called on that thread, through QMetaObject::invokeMethod.
    class Network : public QObject
    {
        Q_OBJECT

    public:
        Network(RingBuffer& outgoing, std::vector<std::unique_ptr<Playout>>& incoming,
            LinkStats& stats);

        void Configure(const AudioBridge::Config& config);
        void StartPairing(const QString& address);
        void ConfirmPairing(const QString& code);
        void CancelPairing();
        void Shutdown();

    signals:
        // Shown on the machine that was asked; typed in on the one that asked.
        void PairingCodeShown(const QString& name, const QString& address, const QString& code);
        void PairingCodeNeeded(const QString& name, const QString& address);
        void Paired(const AudioBridge::Peer& peer);
        void PairingFailed(const QString& message);
        void Failed(const QString& message);

    private:
        struct Remote
        {
            QByteArray KeyId;
            QByteArray Key;
            QHostAddress Address;
            quint16 Port = 0;
            bool Send = true;
            QByteArray Salt;
            quint64 LastSequence = 0;
        };

        struct Pending
        {
            QHostAddress Address;
            quint16 Port = 0;
            QString Entered;
            QString Name;
            QByteArray PublicKey;
            QByteArray Key;
            QString Code;
            bool Active = false;
        };

        void HandleReadyRead();
        void HandleTick();
        void HandlePairRequest(const QByteArray& body, const QHostAddress& from, quint16 port);
        void HandlePairResponse(const QByteArray& body, const QHostAddress& from, quint16 port);
        void HandlePairConfirm(const QByteArray& body, const QHostAddress& from);
        void HandleAudio(const QByteArray& header, const QByteArray& body);
        void SendHello(MessageType type, const QHostAddress& to, quint16 port);

        RingBuffer& Outgoing;
        std::vector<std::unique_ptr<Playout>>& Incoming;
        LinkStats& Stats;
        QUdpSocket Socket;
        QTimer Ticker;
        QTimer AskingTimeout;
        QTimer AskedTimeout;
        AudioBridge::Config Current;
        QByteArray OwnKeyId;
        std::vector<Remote> Remotes;
        Pending Asked;
        Pending Asking;
        QByteArray SendSalt;
        quint64 Sequence = 0;
        std::vector<float> Frames;
    };
} // namespace AudioBridge
