#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <optional>

namespace AudioBridge
{
    // A machine's long-term X25519 key pair, created on first start and kept in the settings.
    struct Identity
    {
        QByteArray Secret;
        QByteArray Public;
    };

    [[nodiscard]] Identity CreateIdentity();
    [[nodiscard]] QByteArray PublicKeyFor(const QByteArray& secret);
    [[nodiscard]] QByteArray KeyId(const QByteArray& publicKey);
    [[nodiscard]] QByteArray RandomBytes(int size);

    // The same key on both sides, whichever of them started the pairing.
    [[nodiscard]] QByteArray SharedKey(const Identity& own, const QByteArray& peerPublic);

    // The four digits one side shows and the other types in. They are derived from the shared
    // key, so a man in the middle - who necessarily ends up with a different key towards each
    // side - produces two different codes, and gets through only by a 1 in 10 000 guess.
    [[nodiscard]] QString PairingCode(const QByteArray& sharedKey);

    // Proof, sent after the code was typed in correctly, that the sender holds the shared key.
    [[nodiscard]] QByteArray ConfirmTag(const QByteArray& sharedKey, const QByteArray& senderPublic);

    // XChaCha20-Poly1305. Seal returns the 16-byte tag followed by the ciphertext; Open returns
    // nothing when the tag does not verify.
    [[nodiscard]] QByteArray Seal(const QByteArray& key, const QByteArray& nonce,
        const QByteArray& associated, const QByteArray& plain);
    [[nodiscard]] std::optional<QByteArray> Open(const QByteArray& key, const QByteArray& nonce,
        const QByteArray& associated, const QByteArray& sealed);
} // namespace AudioBridge
