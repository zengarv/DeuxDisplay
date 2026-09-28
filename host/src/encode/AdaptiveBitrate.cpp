#include "AdaptiveBitrate.h"

#include <algorithm>

namespace dd::encode
{

AdaptiveBitrate::AdaptiveBitrate(unsigned startKbps, Limits limits) : m_limits(limits), m_targetKbps(0)
{
    m_targetKbps = Clamp(startKbps);
}

void AdaptiveBitrate::Reset(unsigned kbps)
{
    m_targetKbps = Clamp(kbps);
    m_windowStartUs = 0;
    m_delivery.clear();
    m_bytes = 0;
    m_budget = 0;
    m_increaseAfterUs = 0;
    m_decreaseAfterUs = 0;
}

unsigned AdaptiveBitrate::Clamp(uint64_t kbps) const
{
    return static_cast<unsigned>(std::clamp<uint64_t>(kbps, m_limits.minKbps, m_limits.maxKbps));
}

bool AdaptiveBitrate::OnFrame(uint64_t nowUs, uint64_t deliveryUs, size_t bytes, size_t budgetBytes)
{
    if (m_windowStartUs == 0)
    {
        m_windowStartUs = nowUs;
    }
    m_delivery.push_back(deliveryUs);
    m_bytes += bytes;
    m_budget += budgetBytes;
    if (nowUs - m_windowStartUs < kWindowUs)
    {
        return false;
    }
    const bool changed = m_delivery.size() >= kMinFrames && Evaluate(nowUs);
    m_windowStartUs = nowUs;
    m_delivery.clear();
    m_bytes = 0;
    m_budget = 0;
    return changed;
}

bool AdaptiveBitrate::Evaluate(uint64_t nowUs)
{
    auto middle = m_delivery.begin() + m_delivery.size() / 2;
    std::nth_element(m_delivery.begin(), middle, m_delivery.end());
    const uint64_t median = *middle;

    const bool warmingUp = m_medians.empty();
    const uint64_t baseline = warmingUp ? median : *std::min_element(m_medians.begin(), m_medians.end());
    m_medians.push_back(median);
    if (m_medians.size() > kBaselineWindows)
    {
        m_medians.pop_front();
    }

    const double utilization = m_budget ? static_cast<double>(m_bytes) / m_budget : 0.0;
    m_last = {median / 1000.0, baseline / 1000.0, utilization, static_cast<unsigned>(m_delivery.size())};
    if (warmingUp)
    {
        return false;
    }
    const uint64_t margin = std::max<uint64_t>(kMarginMinUs, static_cast<uint64_t>(baseline * kMarginFraction));
    const unsigned before = m_targetKbps;
    if (median > baseline + margin)
    {
        // Frames take longer to arrive or decode than they can: back off quickly.
        if (nowUs < m_decreaseAfterUs)
        {
            return false; // just cut; give queued frames time to drain before judging again
        }
        m_ceilingKbps = before;
        m_targetKbps = Clamp(uint64_t{before} * 3 / 4);
        m_decreaseAfterUs = nowUs + kDrainUs;
        m_increaseAfterUs = nowUs + kHoldAfterDecreaseUs;
    }
    else if (nowUs >= m_increaseAfterUs && utilization >= kBusyUtilization)
    {
        // Delivery is flat and the encoder spends its budget: more bits would show. Step up by
        // 10 %, or by 2 % once close to where delivery rose last time (still climbing past it if
        // things got better, but without big overshoots).
        uint64_t step = std::max<uint64_t>(2'000, before / 10);
        if (m_ceilingKbps && before + step > m_ceilingKbps * 9ull / 10)
        {
            step = std::max<uint64_t>(1'000, before / 50);
        }
        m_targetKbps = Clamp(before + step);
        m_increaseAfterUs = nowUs + kHoldAfterIncreaseUs;
    }
    return m_targetKbps != before;
}

} // namespace dd::encode
