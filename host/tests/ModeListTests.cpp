#include "Check.h"

#include "../src/display/ModeList.h"

using dd::BuildModeList;
using dd::driver::PlugMode;
using dd::driver::PlugRequest;

namespace
{

bool Same(const PlugMode& a, const PlugMode& b)
{
    return a.width == b.width && a.height == b.height && a.refreshHz == b.refreshHz;
}

void OffersEverySizeAndRatePreferredFirst()
{
    // Pad Go: native plus scaled sizes, panel rates plus 30.
    const auto modes = BuildModeList({2408, 1720, 60}, {{2408, 1720, 0}, {1806, 1290, 0}, {1204, 860, 0}},
                                     {60, 90, 30});
    CHECK(modes.size() == 9);
    CHECK(Same(modes[0], {2408, 1720, 60}));
    CHECK(Same(modes[1], {2408, 1720, 90})); // highest rate first after the preferred mode
    CHECK(Same(modes[2], {2408, 1720, 30}));
    CHECK(Same(modes[3], {1806, 1290, 90}));
    CHECK(Same(modes[8], {1204, 860, 30}));
}

void SkipsInvalidModesAndCaps()
{
    // Odd width, too small, too fast: dropped. 20 sizes x 3 rates: capped at kMaxModes.
    const auto modes = BuildModeList({1920, 1080, 60}, {{1921, 1080, 0}, {600, 400, 0}}, {60, 300});
    CHECK(modes.size() == 1 && Same(modes[0], {1920, 1080, 60}));

    std::vector<PlugMode> sizes;
    for (uint16_t i = 0; i < 20; ++i)
    {
        sizes.push_back({static_cast<uint16_t>(1000 + 2 * i), 800, 0});
    }
    CHECK(BuildModeList({1920, 1080, 60}, sizes, {60, 90, 30}).size() == dd::driver::kMaxModes);
}

void NoListsMeansThePreferredSize()
{
    const auto modes = BuildModeList({2408, 1720, 60}, {}, {60, 90});
    CHECK(modes.size() == 2 && Same(modes[0], {2408, 1720, 60}) && Same(modes[1], {2408, 1720, 90}));
}

void PlugRequestValidation()
{
    PlugRequest r;
    r.width = 2408;
    r.height = 1720;
    r.refreshHz[0] = 60;
    CHECK(!dd::driver::IsValidPlugRequest(r)); // needs a mode list
    r.modeCount = 2;
    r.modes[0] = {2408, 1720, 60};
    r.modes[1] = {1204, 860, 90};
    CHECK(dd::driver::IsValidPlugRequest(r));
    r.modes[0].refreshHz = 90; // the list must start with the preferred mode
    CHECK(!dd::driver::IsValidPlugRequest(r));
    r.modes[0].refreshHz = 60;
    r.modes[1].width = 1205; // odd
    CHECK(!dd::driver::IsValidPlugRequest(r));
    r.modes[1].width = 1204;
    r.modeCount = dd::driver::kMaxModes + 1;
    CHECK(!dd::driver::IsValidPlugRequest(r));
}

} // namespace

void RunModeListTests()
{
    OffersEverySizeAndRatePreferredFirst();
    SkipsInvalidModesAndCaps();
    NoListsMeansThePreferredSize();
    PlugRequestValidation();
}
