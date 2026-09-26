#pragma once

// Pairing-code derivations and the Wi-Fi authentication MACs (docs/wire-protocol.md,
// "Pairing and authentication"). Keep in sync with android/.../protocol/Pairing.kt.

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "Protocol.h"

namespace dd::protocol
{

inline constexpr size_t kPairingCodeLength = 20; // Base32 characters, 100 bits

using AuthKey = std::array<uint8_t, 32>;

// Upper-cases and drops '-'/spaces. nullopt unless exactly 20 Base32 characters remain.
std::optional<std::string> NormalizePairingCode(std::string_view code);

// "XXXXX-XXXXX-XXXXX-XXXXX" for a normalized code.
std::string FormatPairingCode(const std::string& normalized);

// A fresh random code (normalized). Empty if the system RNG fails.
std::string GeneratePairingCode();

// Everything the two sides derive from a normalized code.
struct PairingSecrets
{
    std::string ssid;       // DeuxDisplay-xxxx
    std::string passphrase; // WPA2, 24 characters
    AuthKey authKey{};
};

std::optional<PairingSecrets> DerivePairingSecrets(const std::string& normalized);

// AUTH_RESPONSE mac (client proof) and AUTH_OK mac (host proof).
Mac ClientAuthMac(const AuthKey& key, const Nonce& hostNonce, const Nonce& clientNonce);
Mac HostAuthMac(const AuthKey& key, const Nonce& clientNonce, const Nonce& hostNonce);

// Constant-time comparison.
bool MacEquals(const Mac& a, const Mac& b);

// Cryptographically random nonce.
bool RandomNonce(Nonce& out);

} // namespace dd::protocol
