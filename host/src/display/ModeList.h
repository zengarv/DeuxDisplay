#pragma once

// The modes a virtual monitor offers Windows. Header-only so the unit tests can cover it.

#include <cstdint>
#include <vector>

#include "../../../driver/DeuxDisplayIdd/Public.h"

namespace dd
{

// Every combination of the client's stream `sizes` and frame `rates` (largest first), with the
// preferred mode first, skipping invalid modes and duplicates, capped at driver::kMaxModes.
// Empty `sizes`/`rates` mean "just the preferred size/rate".
inline std::vector<driver::PlugMode> BuildModeList(driver::PlugMode preferred,
                                                   const std::vector<driver::PlugMode>& sizes,
                                                   std::vector<uint16_t> rates)
{
    std::vector<driver::PlugMode> modes;
    const auto add = [&](driver::PlugMode m) {
        if (modes.size() >= driver::kMaxModes || !driver::IsValidMode(m.width, m.height, m.refreshHz))
        {
            return;
        }
        for (const auto& existing : modes)
        {
            if (existing.width == m.width && existing.height == m.height && existing.refreshHz == m.refreshHz)
            {
                return;
            }
        }
        modes.push_back(m);
    };
    add(preferred);
    if (rates.empty())
    {
        rates.push_back(preferred.refreshHz);
    }
    // Highest rate first within each size, so Windows lists them the way it does real monitors.
    for (size_t i = 1; i < rates.size(); ++i)
    {
        for (size_t j = i; j > 0 && rates[j] > rates[j - 1]; --j)
        {
            std::swap(rates[j], rates[j - 1]);
        }
    }
    const std::vector<driver::PlugMode> fallback{preferred};
    for (const auto& size : sizes.empty() ? fallback : sizes)
    {
        for (uint16_t hz : rates)
        {
            add({size.width, size.height, hz});
        }
    }
    return modes;
}

} // namespace dd
