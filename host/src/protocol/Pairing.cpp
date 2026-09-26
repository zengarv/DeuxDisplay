#include "Pairing.h"

#include <windows.h>

#include <bcrypt.h>

#include <vector>

namespace dd::protocol
{
namespace
{

constexpr char kBase32[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

using Digest = std::array<uint8_t, 32>;

std::optional<Digest> HmacSha256(std::span<const uint8_t> key, std::span<const uint8_t> message)
{
    Digest out{};
    const NTSTATUS status =
        BCryptHash(BCRYPT_HMAC_SHA256_ALG_HANDLE, const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()),
                   const_cast<PUCHAR>(message.data()), static_cast<ULONG>(message.size()), out.data(),
                   static_cast<ULONG>(out.size()));
    if (!BCRYPT_SUCCESS(status))
    {
        return std::nullopt;
    }
    return out;
}

std::span<const uint8_t> Bytes(std::string_view s)
{
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

// RFC 4648 Base32 without padding; callers pass multiples of 5 bytes.
std::string Base32(std::span<const uint8_t> data)
{
    std::string out;
    uint32_t buffer = 0;
    int bits = 0;
    for (uint8_t byte : data)
    {
        buffer = (buffer << 8) | byte;
        bits += 8;
        while (bits >= 5)
        {
            out.push_back(kBase32[(buffer >> (bits - 5)) & 31]);
            bits -= 5;
        }
    }
    return out;
}

Mac AuthMac(const AuthKey& key, std::string_view label, const Nonce& first, const Nonce& second)
{
    std::vector<uint8_t> message(label.begin(), label.end());
    message.insert(message.end(), first.begin(), first.end());
    message.insert(message.end(), second.begin(), second.end());
    return HmacSha256(key, message).value_or(Mac{});
}

} // namespace

std::optional<std::string> NormalizePairingCode(std::string_view code)
{
    std::string out;
    for (char c : code)
    {
        if (c == '-' || c == ' ')
        {
            continue;
        }
        if (c >= 'a' && c <= 'z')
        {
            c = static_cast<char>(c - 'a' + 'A');
        }
        if (!((c >= 'A' && c <= 'Z') || (c >= '2' && c <= '7')))
        {
            return std::nullopt;
        }
        out.push_back(c);
    }
    if (out.size() != kPairingCodeLength)
    {
        return std::nullopt;
    }
    return out;
}

std::string FormatPairingCode(const std::string& normalized)
{
    std::string out;
    for (size_t i = 0; i < normalized.size(); ++i)
    {
        if (i > 0 && i % 5 == 0)
        {
            out.push_back('-');
        }
        out.push_back(normalized[i]);
    }
    return out;
}

std::string GeneratePairingCode()
{
    std::array<uint8_t, kPairingCodeLength> random{};
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
                                        BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
    {
        return {};
    }
    std::string code;
    for (uint8_t b : random)
    {
        code.push_back(kBase32[b & 31]); // 256 is a multiple of 32, so this stays uniform
    }
    return code;
}

std::optional<PairingSecrets> DerivePairingSecrets(const std::string& normalized)
{
    const auto key = Bytes(normalized);
    const auto ssid = HmacSha256(key, Bytes("deuxdisplay ssid"));
    const auto wpa = HmacSha256(key, Bytes("deuxdisplay wpa2"));
    const auto auth = HmacSha256(key, Bytes("deuxdisplay auth"));
    if (!ssid || !wpa || !auth)
    {
        return std::nullopt;
    }

    PairingSecrets secrets;
    constexpr char kHex[] = "0123456789abcdef";
    secrets.ssid = "DeuxDisplay-";
    for (size_t i = 0; i < 2; ++i)
    {
        secrets.ssid.push_back(kHex[(*ssid)[i] >> 4]);
        secrets.ssid.push_back(kHex[(*ssid)[i] & 15]);
    }
    secrets.passphrase = Base32(std::span(*wpa).first(15));
    secrets.authKey = *auth;
    return secrets;
}

Mac ClientAuthMac(const AuthKey& key, const Nonce& hostNonce, const Nonce& clientNonce)
{
    return AuthMac(key, "DXDP-C", hostNonce, clientNonce);
}

Mac HostAuthMac(const AuthKey& key, const Nonce& clientNonce, const Nonce& hostNonce)
{
    return AuthMac(key, "DXDP-H", clientNonce, hostNonce);
}

bool MacEquals(const Mac& a, const Mac& b)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
    {
        diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    }
    return diff == 0;
}

bool RandomNonce(Nonce& out)
{
    return BCRYPT_SUCCESS(
        BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(out.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

} // namespace dd::protocol
