#pragma once

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cwchar>

namespace dd
{

inline void Log(const wchar_t* format, ...)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fwprintf(stderr, L"%02u:%02u:%02u.%03u ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, format);
    std::vfwprintf(stderr, format, args);
    va_end(args);
    std::fputwc(L'\n', stderr);
}

inline unsigned long Hr(HRESULT hr)
{
    return static_cast<unsigned long>(hr);
}

} // namespace dd
