#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace dd::encode
{

// Picks the encoder bitrate from how long frames take to reach the client (FRAME_STATS).
//
// "Delivery" is host send -> decoder output on the client: the transfer plus the decode, the two
// stages the bitrate drives. The controller keeps the lowest recent per-second median delivery as
// the baseline. It raises the bitrate while the encoder actually uses its budget (content that
// would look better with more bits) and delivery stays at the baseline; it cuts it back as soon
// as delivery rises above it (the link or the decoder is falling behind). Idle desktops send
// nothing, so the bitrate only moves while there's motion to measure it on.
class AdaptiveBitrate
{
  public:
    struct Limits
    {
        unsigned minKbps = 5'000;
        unsigned maxKbps = 100'000;
    };

    // What the last evaluated window saw, for logging.
    struct Window
    {
        double deliveryMs = 0; // median
        double baselineMs = 0;
        double utilization = 0; // encoded bytes / CBR budget
        unsigned frames = 0;
    };

    AdaptiveBitrate(unsigned startKbps, Limits limits);

    // One delivered frame, times in µs. `bytes` is its encoded size and `budgetBytes` what CBR
    // allowed for it (bitrate / fps). Returns true when the target changed.
    bool OnFrame(uint64_t nowUs, uint64_t deliveryUs, size_t bytes, size_t budgetBytes);

    // The bitrate changed from outside (e.g. the user switched back to adaptive): start from it.
    void Reset(unsigned kbps);

    unsigned TargetKbps() const { return m_targetKbps; }
    const Window& LastWindow() const { return m_last; }

    static constexpr uint64_t kWindowUs = 1'000'000;
    static constexpr unsigned kMinFrames = 15;       // fewer in a window: too little to judge
    static constexpr size_t kBaselineWindows = 20;   // baseline = min median over this many windows
    static constexpr uint64_t kMarginMinUs = 3'000;  // delivery this far above baseline = congested
    static constexpr double kMarginFraction = 0.2;   // ... or this fraction of the baseline
    static constexpr double kBusyUtilization = 0.6;  // frames use this much of the budget: bits help
    static constexpr uint64_t kDrainUs = 2'000'000;             // no second cut before this
    static constexpr uint64_t kHoldAfterDecreaseUs = 4'000'000; // no increase before this
    static constexpr uint64_t kHoldAfterIncreaseUs = 2'000'000;

  private:
    bool Evaluate(uint64_t nowUs);
    unsigned Clamp(uint64_t kbps) const;

    Limits m_limits;
    unsigned m_targetKbps;
    uint64_t m_windowStartUs = 0;
    std::vector<uint64_t> m_delivery;
    uint64_t m_bytes = 0;
    uint64_t m_budget = 0;
    std::deque<uint64_t> m_medians; // recent window medians, newest last
    uint64_t m_increaseAfterUs = 0;
    uint64_t m_decreaseAfterUs = 0;
    unsigned m_ceilingKbps = 0; // where delivery last rose; probe gently near and past it
    Window m_last;
};

} // namespace dd::encode
