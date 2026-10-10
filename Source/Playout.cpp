#include "Playout.h"

#include <algorithm>

namespace
{
    // The correction is a fraction of a percent at most: clock drift between two machines is
    // in the tens of parts per million, so anything larger would only be audible as pitch wobble.
    constexpr auto MaxCorrection = 0.005;
    constexpr auto CorrectionGain = 0.002;
    constexpr auto FillSmoothing = 0.02;
    // Past this many targets of backlog the audio is too late to be worth playing; skip ahead.
    constexpr auto OverflowFactor = 4.0;

    float Hermite(float y0, float y1, float y2, float y3, float t)
    {
        const auto c1 = 0.5f * (y2 - y0);
        const auto c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const auto c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * t + c2) * t + c1) * t + y1;
    }
} // namespace

void AudioBridge::Playout::Push(const float* frames, std::size_t count, int sampleRate)
{
    StreamRate.store(sampleRate, std::memory_order_relaxed);
    Ring.Write(frames, count);
}

void AudioBridge::Playout::MixInto(float* frames, std::size_t count, int outputRate)
{
    const auto streamRate = StreamRate.load(std::memory_order_relaxed);
    const auto target = TargetMs.load(std::memory_order_relaxed) * streamRate / 1000.0;
    auto available = static_cast<double>(Ring.Available());
    if (!Primed && streamRate > 0 && target > 0.0 && available >= target)
    {
        Primed = true;
        SmoothedFill = available;
    }

    if (Primed)
    {
        if (available > target * OverflowFactor)
        {
            Ring.Skip(static_cast<std::size_t>(available - target));
            available = target;
            SmoothedFill = target;
        }
        SmoothedFill += (available - SmoothedFill) * FillSmoothing;
        const auto error = (SmoothedFill - target) / target;
        const auto correction = std::clamp(error * CorrectionGain, -MaxCorrection, MaxCorrection);
        const auto ratio = static_cast<double>(streamRate) / outputRate * (1.0 + correction);

        auto starved = false;
        for (auto produced = std::size_t{0}; produced < count && !starved; ++produced)
        {
            Position += ratio;
            while (Position >= 1.0 && !starved)
            {
                auto next = std::array<float, 2>{};
                if (Ring.Read(next.data(), 1) == 1)
                {
                    History = {History[1], History[2], History[3], next};
                    Position -= 1.0;
                }
                else
                    starved = true;
            }
            if (!starved)
            {
                const auto t = static_cast<float>(Position);
                for (auto channel = 0; channel < 2; ++channel)
                    frames[produced * 2 + channel] += Hermite(History[0][channel],
                        History[1][channel], History[2][channel], History[3][channel], t);
            }
        }
        if (starved)
        {
            Primed = false;
            Underruns.fetch_add(1, std::memory_order_relaxed);
        }
    }

    const auto buffered = streamRate > 0 ? Ring.Available() * 1000 / streamRate : 0;
    BufferedMs.store(static_cast<int>(buffered), std::memory_order_relaxed);
}
