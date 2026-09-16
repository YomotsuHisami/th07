#pragma once

#include <cstdint>

namespace Netplay
{
// Presentation-side scheduling only. Never interrupt an atomic rollback or
// discard simulation debt: after a costly driver tick, draw and let browser
// input/network/audio callbacks run before attempting more catch-up work.
struct FrameBudget
{
    static constexpr std::uint32_t MaxCatchupTicks = 6;
    static constexpr std::uint64_t CatchupBudgetNs = 8'000'000;

    static constexpr bool CanStartTick(std::uint32_t completedTicks,
                                        std::uint64_t elapsedNs)
    {
        // Always permit one due tick so slow devices cannot starve forever.
        return completedTicks < MaxCatchupTicks &&
            (completedTicks == 0 || elapsedNs < CatchupBudgetNs);
    }
};
} // namespace Netplay
