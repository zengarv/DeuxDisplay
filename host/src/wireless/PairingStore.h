#pragma once

#include <optional>
#include <string>

namespace dd::wireless
{

// The host's pairing code (normalized), kept DPAPI-encrypted for the current user in
// %LOCALAPPDATA%\DeuxDisplay\pairing.bin. Created on first use; `reset` replaces it, which
// unpairs every tablet (they need to pair again over USB or by typing the new code).
std::optional<std::string> LoadOrCreatePairingCode(bool reset = false);

// This PC's name as UTF-8, sent to the client in PAIRING.
std::string ComputerNameUtf8();

} // namespace dd::wireless
