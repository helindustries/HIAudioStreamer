#pragma once

#include "Protocol.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <vector>

namespace HIAudioStreamer
{
    // Single producer, single consumer, interleaved stereo float frames. One side is always a
    // real-time audio thread, so it takes no lock and allocates nothing after construction. The
    // indices only ever grow and are reduced modulo the capacity on access, which keeps full and
    // empty apart without a spare slot.
    class RingBuffer
    {
    public:
        explicit RingBuffer(std::size_t capacityFrames)
            : Samples(capacityFrames * Channels)
            , Capacity(capacityFrames)
        {}

        [[nodiscard]] std::size_t Available() const
        {
            return WriteIndex.load(std::memory_order_acquire)
                   - ReadIndex.load(std::memory_order_acquire);
        }

        // Producer only. Frames that do not fit are dropped and the count written is returned.
        std::size_t Write(const float* frames, std::size_t count)
        {
            const auto write = WriteIndex.load(std::memory_order_relaxed);
            const auto used = write - ReadIndex.load(std::memory_order_acquire);
            const auto accepted = std::min(count, Capacity - used);
            for (auto i = std::size_t{0}; i < accepted; ++i)
            {
                const auto slot = ((write + i) % Capacity) * Channels;
                std::copy_n(frames + i * Channels, Channels, Samples.begin() + slot);
            }
            WriteIndex.store(write + accepted, std::memory_order_release);
            return accepted;
        }

        // Consumer only.
        std::size_t Read(float* frames, std::size_t count)
        {
            const auto read = ReadIndex.load(std::memory_order_relaxed);
            const auto taken = std::min(count, WriteIndex.load(std::memory_order_acquire) - read);
            for (auto i = std::size_t{0}; i < taken; ++i)
            {
                const auto slot = ((read + i) % Capacity) * Channels;
                std::copy_n(Samples.begin() + slot, Channels, frames + i * Channels);
            }
            ReadIndex.store(read + taken, std::memory_order_release);
            return taken;
        }

        // Consumer only.
        void Skip(std::size_t count)
        {
            const auto read = ReadIndex.load(std::memory_order_relaxed);
            const auto skipped = std::min(count, WriteIndex.load(std::memory_order_acquire) - read);
            ReadIndex.store(read + skipped, std::memory_order_release);
        }

    private:
        std::vector<float> Samples;
        std::size_t Capacity;
        std::atomic<std::size_t> WriteIndex{0};
        std::atomic<std::size_t> ReadIndex{0};
    };
} // namespace HIAudioStreamer
