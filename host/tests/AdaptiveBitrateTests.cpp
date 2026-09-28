#include "Check.h"

#include "../src/encode/AdaptiveBitrate.h"

#include <algorithm>
#include <functional>

using dd::encode::AdaptiveBitrate;

namespace
{

// Feeds `seconds` of 60 fps frames. `deliveryUs(kbps)` models the link + decoder at the current
// bitrate; `utilization` is how much of the CBR budget each frame uses.
void Run(AdaptiveBitrate& abr, uint64_t& nowUs, int seconds, const std::function<uint64_t(unsigned)>& deliveryUs,
         double utilization = 1.0, unsigned fps = 60)
{
    for (int i = 0; i < seconds * static_cast<int>(fps); ++i)
    {
        nowUs += 1'000'000 / fps;
        const size_t budget = abr.TargetKbps() * 1000 / 8 / fps;
        abr.OnFrame(nowUs, deliveryUs(abr.TargetKbps()), static_cast<size_t>(budget * utilization), budget);
    }
}

void ClimbsToMaxWhileDeliveryIsFlat()
{
    AdaptiveBitrate abr(30'000, {5'000, 100'000});
    uint64_t now = 1'000'000;
    Run(abr, now, 180, [](unsigned) { return 20'000; });
    CHECK(abr.TargetKbps() == 100'000);
}

void HoldsWhenTheEncoderDoesntNeedTheBits()
{
    // A mostly static desktop: small frames, nothing to gain from a bigger budget.
    AdaptiveBitrate abr(30'000, {5'000, 100'000});
    uint64_t now = 1'000'000;
    Run(abr, now, 30, [](unsigned) { return 20'000; }, 0.2);
    CHECK(abr.TargetKbps() == 30'000);
}

void HoldsWithTooFewFramesToJudge()
{
    AdaptiveBitrate abr(30'000, {5'000, 100'000});
    uint64_t now = 1'000'000;
    Run(abr, now, 30, [](unsigned) { return 20'000; }, 1.0, 10);
    CHECK(abr.TargetKbps() == 30'000);
}

void SettlesBelowWhereDeliverySlowsDown()
{
    // Delivery is flat up to 60 Mbit/s, then grows 1 ms per extra Mbit/s (the decoder can't keep up).
    const auto delivery = [](unsigned kbps) -> uint64_t {
        return 20'000 + (kbps > 60'000 ? (kbps - 60'000) : 0);
    };
    AdaptiveBitrate abr(30'000, {5'000, 150'000});
    uint64_t now = 1'000'000;
    Run(abr, now, 120, delivery);
    CHECK(abr.TargetKbps() <= 64'000);
    CHECK(abr.TargetKbps() >= 40'000);

    // Then it settles: over five minutes it changes only for re-probes, which back off as they
    // keep failing (1, 2, 4 minutes apart), never hunting every few seconds, and stays in band.
    unsigned lowest = abr.TargetKbps();
    unsigned highest = abr.TargetKbps();
    unsigned changes = 0;
    for (int i = 0; i < 300; ++i)
    {
        const unsigned before = abr.TargetKbps();
        Run(abr, now, 1, delivery);
        changes += abr.TargetKbps() != before;
        lowest = std::min(lowest, abr.TargetKbps());
        highest = std::max(highest, abr.TargetKbps());
    }
    CHECK(changes <= 12);
    CHECK(lowest >= 40'000);
    CHECK(highest <= 72'000);
}

void BacksOffWhenDeliverySuddenlyRises()
{
    AdaptiveBitrate abr(50'000, {5'000, 100'000});
    uint64_t now = 1'000'000;
    Run(abr, now, 5, [](unsigned) { return 20'000; }, 0.3); // baseline at 20 ms, no climbing
    CHECK(abr.TargetKbps() == 50'000);
    Run(abr, now, 2, [](unsigned) { return 40'000; }, 0.3); // e.g. the tablet got hot
    CHECK(abr.TargetKbps() < 50'000);
    CHECK(abr.LastWindow().deliveryMs > abr.LastWindow().baselineMs);
}

void CutsHardWhenDeliveryStaysSlow()
{
    AdaptiveBitrate abr(30'000, {5'000, 100'000});
    uint64_t now = 1'000'000;
    Run(abr, now, 3, [](unsigned) { return 10'000; }, 0.3);
    Run(abr, now, 10, [](unsigned) { return 60'000; }, 0.3); // e.g. the decoder is overwhelmed
    CHECK(abr.TargetKbps() <= 10'000);
}

void ResetForgetsTheOldFormatsBaseline()
{
    // Learned at a small size (fast decode), then the stream goes full size (slower decode, and
    // that's normal): after Reset it must not read the slower delivery as congestion.
    AdaptiveBitrate abr(30'000, {5'000, 100'000});
    uint64_t now = 1'000'000;
    Run(abr, now, 10, [](unsigned) { return 27'000; }, 0.3);
    abr.Reset(abr.TargetKbps());
    Run(abr, now, 10, [](unsigned) { return 39'000; }, 0.3);
    CHECK(abr.TargetKbps() == 30'000);
}

void NeverLeavesItsLimits()
{
    AdaptiveBitrate abr(1'000, {5'000, 40'000});
    CHECK(abr.TargetKbps() == 5'000);
    abr.Reset(1'000'000);
    CHECK(abr.TargetKbps() == 40'000);
    uint64_t now = 1'000'000;
    Run(abr, now, 30, [](unsigned) { return 10'000; });
    CHECK(abr.TargetKbps() == 40'000);
}

} // namespace

void RunAdaptiveBitrateTests()
{
    ClimbsToMaxWhileDeliveryIsFlat();
    HoldsWhenTheEncoderDoesntNeedTheBits();
    HoldsWithTooFewFramesToJudge();
    SettlesBelowWhereDeliverySlowsDown();
    BacksOffWhenDeliverySuddenlyRises();
    CutsHardWhenDeliveryStaysSlow();
    ResetForgetsTheOldFormatsBaseline();
    NeverLeavesItsLimits();
}
