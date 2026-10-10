#pragma once

#include "RingBuffer.h"

#include <array>
#include <atomic>
#include <cstddef>

namespace HIAudioStreamer
{
    // Sits between the network and the output device for one peer. It holds TargetMs of audio
    // against network jitter and plays it out at whatever rate the device runs, resampling as it
    // goes. The resampling ratio is nudged by how far the buffer sits from its target, which also
    // absorbs the drift between the sender's and the receiver's audio clocks: without that the
    // buffer would slowly run dry or overflow however generous the target.
    class Playout
    {
    public:
        explicit Playout(std::size_t capacityFrames)
            : Ring(capacityFrames)
        {}

        // Network thread.
        void Push(const float* frames, std::size_t count, int sampleRate);
        // Render thread. Adds into frames rather than overwriting, so several peers mix.
        void MixInto(float* frames, std::size_t count, int outputRate);

        std::atomic<int> TargetMs{40};
        std::atomic<int> BufferedMs{0};
        std::atomic<unsigned> Underruns{0};

    private:
        RingBuffer Ring;
        std::atomic<int> StreamRate{0};
        bool Primed = false;
        double Position = 0.0;
        double SmoothedFill = 0.0;
        std::array<std::array<float, 2>, 4> History{};
    };
} // namespace HIAudioStreamer
