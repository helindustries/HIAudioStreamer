#pragma once

#include "Crypto.h"
#include "Protocol.h"

#include <QtCore/QList>
#include <QtCore/QMetaType>
#include <QtCore/QString>

namespace HIAudioStreamer
{
    // A paired machine. Address is entered by hand ("192.168.1.4" or "192.168.1.4:49710"); the
    // key is what lets its packets in, so an address on its own admits nothing.
    struct Peer
    {
        QString Name;
        QString Address;
        QByteArray PublicKey;
        QByteArray Key;
        bool Send = true;
    };

    struct Config
    {
        Identity Self;
        QString Name;
        quint16 Port = DefaultPort;
        bool Send = true;
        // Empty means this machine's own output, everything but HIAudioStreamer itself.
        QString Source;
        // Empty means the system default output.
        QString Output;
        int BufferMs = 40;
        QList<Peer> Peers;
    };

    [[nodiscard]] Config LoadConfig();
    void SaveConfig(const Config& config);
} // namespace HIAudioStreamer

Q_DECLARE_METATYPE(HIAudioStreamer::Peer)
Q_DECLARE_METATYPE(HIAudioStreamer::Config)
