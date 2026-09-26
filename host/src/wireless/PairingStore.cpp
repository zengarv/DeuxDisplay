#include "PairingStore.h"

#include <windows.h>

#include <dpapi.h>
#include <shlobj.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include "../common/Log.h"
#include "../protocol/Pairing.h"

namespace dd::wireless
{
namespace
{

std::filesystem::path StorePath()
{
    PWSTR folder = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder)))
    {
        return {};
    }
    std::filesystem::path path = std::filesystem::path(folder) / L"DeuxDisplay" / L"pairing.bin";
    CoTaskMemFree(folder);
    return path;
}

std::optional<std::string> Load(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return std::nullopt;
    }
    std::vector<char> sealed((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    DATA_BLOB in{static_cast<DWORD>(sealed.size()), reinterpret_cast<BYTE*>(sealed.data())};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
    {
        Log(L"pairing: can't decrypt %s (0x%08lX)", path.c_str(), GetLastError());
        return std::nullopt;
    }
    std::string code(reinterpret_cast<const char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return protocol::NormalizePairingCode(code);
}

bool Save(const std::filesystem::path& path, const std::string& code)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    DATA_BLOB in{static_cast<DWORD>(code.size()), reinterpret_cast<BYTE*>(const_cast<char*>(code.data()))};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"DeuxDisplay pairing code", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                          &out))
    {
        return false;
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    return static_cast<bool>(file);
}

} // namespace

std::optional<std::string> LoadOrCreatePairingCode(bool reset)
{
    const auto path = StorePath();
    if (path.empty())
    {
        return std::nullopt;
    }
    if (!reset)
    {
        if (auto code = Load(path))
        {
            return code;
        }
    }
    std::string code = protocol::GeneratePairingCode();
    if (code.empty() || !Save(path, code))
    {
        Log(L"pairing: can't create %s", path.c_str());
        return std::nullopt;
    }
    Log(L"pairing: new pairing code created%s", reset ? L" (tablets must pair again)" : L"");
    return code;
}

std::string ComputerNameUtf8()
{
    wchar_t name[256] = {};
    DWORD size = ARRAYSIZE(name);
    if (!GetComputerNameExW(ComputerNameDnsHostname, name, &size))
    {
        return "PC";
    }
    char utf8[512] = {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, name, static_cast<int>(size), utf8, sizeof(utf8), nullptr,
                                           nullptr);
    return std::string(utf8, length > 0 ? static_cast<size_t>(length) : 0);
}

} // namespace dd::wireless
