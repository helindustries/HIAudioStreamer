#include "Config.h"

#include <QtCore/QSettings>
#include <QtCore/QSysInfo>
#include <QtCore/QtGlobal>

#include <ranges>

namespace
{
    constexpr auto IdentityKey = "Identity";
    constexpr auto NameKey = "Name";
    constexpr auto PortKey = "Port";
    constexpr auto SendKey = "Send";
    constexpr auto SourceKey = "Source";
    constexpr auto OutputKey = "Output";
    constexpr auto BufferMsKey = "BufferMs";
    constexpr auto PeersKey = "Peers";
    constexpr auto AddressKey = "Address";
    constexpr auto PublicKeyKey = "PublicKey";
    constexpr auto SharedKeyKey = "Key";
} // namespace

// Anything missing from the settings keeps the default the structs declare.
HIAudioStreamer::Config HIAudioStreamer::LoadConfig()
{
    auto settings = QSettings();
    auto config = Config();
    if (const auto secret = QByteArray::fromBase64(settings.value(IdentityKey).toByteArray());
        secret.size() == KeySize)
        config.Self = {secret, PublicKeyFor(secret)};
    else config.Self = CreateIdentity();

    config.Name = settings.value(NameKey, QSysInfo::machineHostName()).toString();
    config.Port = static_cast<quint16>(settings.value(PortKey, config.Port).toUInt());
    config.Send = settings.value(SendKey, config.Send).toBool();
    config.Source = settings.value(SourceKey).toString();
    config.Output = settings.value(OutputKey).toString();
    config.BufferMs = settings.value(BufferMsKey, config.BufferMs).toInt();

    for (const auto i : std::views::iota(0, settings.beginReadArray(PeersKey)))
    {
        settings.setArrayIndex(i);
        const auto peer = Peer{
            .Name = settings.value(NameKey).toString(),
            .Address = settings.value(AddressKey).toString(),
            .PublicKey = QByteArray::fromBase64(settings.value(PublicKeyKey).toByteArray()),
            .Key = QByteArray::fromBase64(settings.value(SharedKeyKey).toByteArray()),
            .Send = settings.value(SendKey, Peer().Send).toBool(),
        };
        if (peer.PublicKey.size() == KeySize && peer.Key.size() == KeySize)
            config.Peers.append(peer);
        else
        {
            qWarning("Ignoring stored connection %d (%s): its keys are not %d bytes", i,
                     qUtf8Printable(peer.Name), KeySize);
        }
    }
    settings.endArray();
    return config;
}

void HIAudioStreamer::SaveConfig(const Config& config)
{
    auto settings = QSettings();
    settings.setValue(IdentityKey, config.Self.Secret.toBase64());
    settings.setValue(NameKey, config.Name);
    settings.setValue(PortKey, config.Port);
    settings.setValue(SendKey, config.Send);
    settings.setValue(SourceKey, config.Source);
    settings.setValue(OutputKey, config.Output);
    settings.setValue(BufferMsKey, config.BufferMs);

    settings.remove(PeersKey);
    settings.beginWriteArray(PeersKey, static_cast<int>(config.Peers.size()));
    for (auto i = 0; const auto& [name, address, publicKey, key, send] : config.Peers)
    {
        settings.setArrayIndex(i++);
        settings.setValue(NameKey, name);
        settings.setValue(AddressKey, address);
        settings.setValue(PublicKeyKey, publicKey.toBase64());
        settings.setValue(SharedKeyKey, key.toBase64());
        settings.setValue(SendKey, send);
    }
    settings.endArray();
}
