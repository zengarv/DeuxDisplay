#pragma once

#include <cstdint>
#include <string>

namespace dd
{

// Sets a display's orientation like Settings > Display > Orientation: `degrees` clockwise from its
// native landscape (0 landscape, 90 portrait, 180/270 flipped). Needs no admin rights and isn't
// saved, so the next plug-in starts from what the client asks for again. Returns false if Windows
// refused the change (logged).
bool SetDisplayOrientation(const std::wstring& deviceName, uint16_t degrees);

} // namespace dd
