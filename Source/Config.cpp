#include "Config.h"

#include <QtCore/QSettings>
#include <QtCore/QSysInfo>

HIAudioStreamer::Config HIAudioStreamer::LoadConfig()
{
    auto settings = QSettings();
    auto config = Config();
    config.Self.Secret = QByteArray::fromBase64(settings.value("Identity").toByteArray());
    if (config.Self.Secret.size() != KeySize)
        config.Self = CreateIdentity();
    else
        config.Self.Public = PublicKeyFor(config.Self.Secret);

    config.Name = settings.value("Name", QSysInfo::machineHostName()).toString();
    config.Port = static_cast<quint16>(settings.value("Port", DefaultPort).toUInt());
    config.Send = settings.value("Send", true).toBool();
    config.Source = settings.value("Source").toString();
    config.Output = settings.value("Output").toString();
    config.BufferMs = settings.value("BufferMs", 40).toInt();

    const auto count = settings.beginReadArray("Peers");
    for (auto i = 0; i < count; ++i)
    {
        settings.setArrayIndex(i);
        auto peer = Peer();
        peer.Name = settings.value("Name").toString();
        peer.Address = settings.value("Address").toString();
        peer.PublicKey = QByteArray::fromBase64(settings.value("PublicKey").toByteArray());
        peer.Key = QByteArray::fromBase64(settings.value("Key").toByteArray());
        peer.Send = settings.value("Send", true).toBool();
        if (peer.PublicKey.size() == KeySize && peer.Key.size() == KeySize)
            config.Peers.append(peer);
    }
    settings.endArray();
    return config;
}

void HIAudioStreamer::SaveConfig(const Config& config)
{
    auto settings = QSettings();
    settings.setValue("Identity", config.Self.Secret.toBase64());
    settings.setValue("Name", config.Name);
    settings.setValue("Port", config.Port);
    settings.setValue("Send", config.Send);
    settings.setValue("Source", config.Source);
    settings.setValue("Output", config.Output);
    settings.setValue("BufferMs", config.BufferMs);

    settings.remove("Peers");
    settings.beginWriteArray("Peers", static_cast<int>(config.Peers.size()));
    for (auto i = 0; i < config.Peers.size(); ++i)
    {
        const auto& peer = config.Peers[i];
        settings.setArrayIndex(i);
        settings.setValue("Name", peer.Name);
        settings.setValue("Address", peer.Address);
        settings.setValue("PublicKey", peer.PublicKey.toBase64());
        settings.setValue("Key", peer.Key.toBase64());
        settings.setValue("Send", peer.Send);
    }
    settings.endArray();
}
