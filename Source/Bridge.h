#pragma once

#include "AudioBackend.h"
#include "Config.h"
#include "Network.h"
#include "Playout.h"
#include "RingBuffer.h"

#include <QtCore/QObject>
#include <QtCore/QStringList>
#include <QtCore/QThread>

#include <memory>
#include <vector>

namespace HIAudioStreamer
{
    // Owns the audio streams and the network thread, and rebuilds the streams whenever the
    // configuration changes. Every paired peer has a fixed Playout slot, and whatever arrives
    // from any of them is mixed into the one output.
    class Bridge : public QObject
    {
        Q_OBJECT

    public:
        Bridge();
        ~Bridge() override;

        void Apply(const Config& config);
        // Called once a second: silences this machine's own output while its system audio is
        // being played somewhere else, and restores it as soon as no peer confirms listening.
        void UpdateLocalMute();
        // For a session ending: lets the output go before the process may be killed outright.
        void ReleaseLocalMute();
        [[nodiscard]] QString Status() const;
        // One line of counters for the log: enough to tell "nothing arrives" from "arrives but is
        // rejected" from "accepted but not playing".
        // Not const: reading the peak levels resets them for the next interval.
        [[nodiscard]] QString Diagnostics();
        [[nodiscard]] bool Active() const;

        // Passed on to the network thread, so the tray never touches it directly.
        void StartPairing(const QString& address);
        void ConfirmPairing(const QString& code);
        void CancelPairing();

        QStringList Errors;

    signals:
        // Forwarded from Network, which lives on another thread.
        void PairingCodeShown(const QString& name, const QString& address, const QString& code);
        void PairingCodeNeeded(const QString& name, const QString& address);
        void Paired(const HIAudioStreamer::Peer& peer);
        void PairingFailed(const QString& message);
        void Failed(const QString& message);

    private:
        void Mix(float* frames, std::size_t count, int rate);

        RingBuffer Outgoing;
        std::vector<std::unique_ptr<Playout>> Incoming;
        std::vector<float> MixScratch;
        LinkStats Stats;
        QThread NetworkThread;
        std::unique_ptr<Network> NetworkObject;
        std::unique_ptr<AudioStream> Capture;
        std::unique_ptr<AudioStream> Render;
        std::unique_ptr<LocalMute> Mute;
        Config Current;
    };
} // namespace HIAudioStreamer
