#pragma once

#include "../protocol/Protocol.h"

namespace dd::input
{

// Presses the keys for a dock shortcut (Ctrl+Z, Win+Tab, the media play/pause key, ...) with
// SendInput. The keys go to whichever window is active, like a keyboard's would. Volume actions
// aren't keys; they go through media::MediaControls. Returns false if nothing was sent.
bool SendShortcut(protocol::Action action);

} // namespace dd::input
