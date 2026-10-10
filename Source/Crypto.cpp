#include "Crypto.h"

#include "Protocol.h"
#include "ThirdParty/Monocypher/monocypher.h"

#include <QtCore/QRandomGenerator>
#include <QtCore/QtEndian>

#include <algorithm>

namespace
{
    constexpr auto KeyContext = "HIAudioStreamer pairing v1";
    constexpr auto CodeContext = "HIAudioStreamer code";
    constexpr auto ConfirmContext = "HIAudioStreamer confirm";

    const auto* Bytes(const QByteArray& data)
    {
        return reinterpret_cast<const std::uint8_t*>(data.constData());
    }

    auto* Bytes(QByteArray& data)
    {
        return reinterpret_cast<std::uint8_t*>(data.data());
    }

    QByteArray Keyed(const QByteArray& key, const QByteArray& message, int size)
    {
        auto hash = QByteArray(size, '\0');
        crypto_blake2b_keyed(Bytes(hash), hash.size(), Bytes(key), key.size(), Bytes(message),
            message.size());
        return hash;
    }
} // namespace

QByteArray HIAudioStreamer::RandomBytes(int size)
{
    auto bytes = QByteArray(size, '\0');
    QRandomGenerator::system()->generate(bytes.begin(), bytes.end());
    return bytes;
}

HIAudioStreamer::Identity HIAudioStreamer::CreateIdentity()
{
    const auto secret = RandomBytes(KeySize);
    return {secret, PublicKeyFor(secret)};
}

QByteArray HIAudioStreamer::PublicKeyFor(const QByteArray& secret)
{
    auto publicKey = QByteArray(KeySize, '\0');
    crypto_x25519_public_key(Bytes(publicKey), Bytes(secret));
    return publicKey;
}

QByteArray HIAudioStreamer::KeyId(const QByteArray& publicKey)
{
    auto hash = QByteArray(KeyIdSize, '\0');
    crypto_blake2b(Bytes(hash), hash.size(), Bytes(publicKey), publicKey.size());
    return hash;
}

QByteArray HIAudioStreamer::SharedKey(const Identity& own, const QByteArray& peerPublic)
{
    auto raw = QByteArray(KeySize, '\0');
    crypto_x25519(Bytes(raw), Bytes(own.Secret), Bytes(peerPublic));
    // Ordering the two public keys makes the transcript, and so the key, the same on both sides.
    const auto& low = std::min(own.Public, peerPublic);
    const auto& high = std::max(own.Public, peerPublic);
    const auto transcript = QByteArray(KeyContext) + raw + low + high;
    auto key = QByteArray(KeySize, '\0');
    crypto_blake2b(Bytes(key), key.size(), Bytes(transcript), transcript.size());
    crypto_wipe(Bytes(raw), raw.size());
    return key;
}

QString HIAudioStreamer::PairingCode(const QByteArray& sharedKey)
{
    const auto hash = Keyed(sharedKey, QByteArray(CodeContext), 8);
    const auto value = qFromLittleEndian<quint32>(hash.constData()) % 10000;
    return QStringLiteral("%1").arg(value, 4, 10, QLatin1Char('0'));
}

QByteArray HIAudioStreamer::ConfirmTag(const QByteArray& sharedKey, const QByteArray& senderPublic)
{
    return Keyed(sharedKey, QByteArray(ConfirmContext) + senderPublic, MacSize);
}

QByteArray HIAudioStreamer::Seal(const QByteArray& key, const QByteArray& nonce,
    const QByteArray& associated, const QByteArray& plain)
{
    auto sealed = QByteArray(MacSize + plain.size(), '\0');
    crypto_aead_lock(Bytes(sealed) + MacSize, Bytes(sealed), Bytes(key), Bytes(nonce),
        Bytes(associated), associated.size(), Bytes(plain), plain.size());
    return sealed;
}

std::optional<QByteArray> HIAudioStreamer::Open(const QByteArray& key, const QByteArray& nonce,
    const QByteArray& associated, const QByteArray& sealed)
{
    auto plain = std::optional<QByteArray>();
    if (sealed.size() >= MacSize)
    {
        auto text = QByteArray(sealed.size() - MacSize, '\0');
        if (crypto_aead_unlock(Bytes(text), Bytes(sealed), Bytes(key), Bytes(nonce),
                Bytes(associated), associated.size(), Bytes(sealed) + MacSize, text.size())
            == 0)
            plain = text;
    }
    return plain;
}
