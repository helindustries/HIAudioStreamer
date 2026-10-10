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
#include <mutex>
#include <semaphore>
#include <thread>
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
    // carried on that loop would drop out for as long as the menu stays up. Everything public
    // except NotifyCaptured is called on that thread, through QMetaObject::invokeMethod.
    //
    // Sending runs on a further thread of its own, woken by the capture callback whenever a full
    // packet is waiting. Polling for it instead took a 2 ms timer, which on Windows means a
    // multimedia timer thread and an event loop turning 500 times a second - most of the CPU
    // this used, whether or not there was anything to send.
    class Network : public QObject
    {
        Q_OBJECT

    public:
        Network(RingBuffer& outgoing, std::vector<std::unique_ptr<Playout>>& incoming,
            LinkStats& stats);
        ~Network() override;

        // Real-time safe: called from the capture callback after it wrote to the outgoing ring.
        void NotifyCaptured(int rate);

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

        struct Target
        {
            QByteArray Key;
            QHostAddress Address;
            quint16 Port = 0;
        };

        void HandleReadyRead();
        void SendLoop();
        void SendPending(QUdpSocket& socket);
        void HandlePairRequest(const QByteArray& body, const QHostAddress& from, quint16 port);
        void HandlePairResponse(const QByteArray& body, const QHostAddress& from, quint16 port);
        void HandlePairConfirm(const QByteArray& body, const QHostAddress& from);
        void HandleAudio(const QByteArray& header, const QByteArray& body);
        void SendHello(MessageType type, const QHostAddress& to, quint16 port);

        RingBuffer& Outgoing;
        std::vector<std::unique_ptr<Playout>>& Incoming;
        LinkStats& Stats;
        QUdpSocket Socket;
        QTimer AskingTimeout;
        QTimer AskedTimeout;
        AudioBridge::Config Current;
        QByteArray OwnKeyId;
        std::vector<Remote> Remotes;
        Pending Asked;
        Pending Asking;
        std::vector<float> Frames;

        // Shared with the sender thread; Configure replaces them under the mutex.
        std::mutex SendMutex;
        std::vector<Target> Targets;
        QByteArray SendKeyId;
        QByteArray SendSalt;
        bool SendEnabled = false;

        // Sender thread only.
        quint64 Sequence = 0;
        std::vector<float> SendFrames;

        std::counting_semaphore<> Wake{0};
        std::atomic<bool> StopSending{false};
        std::thread Sender;
    };
} // namespace AudioBridge
