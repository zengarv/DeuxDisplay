#pragma once

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cwchar>

namespace dd
{

// Prefix for this thread's log lines, e.g. L"[2] " for the second concurrent session.
// Empty by default, so a lone session logs exactly as before.
inline thread_local wchar_t t_logTag[16] = L"";

inline void SetLogTag(const wchar_t* tag)
{
    wcsncpy_s(t_logTag, tag, _TRUNCATE);
}

// One line per call, written with a single stdio call so concurrent sessions don't interleave.
inline void Log(const wchar_t* format, ...)
{
    wchar_t line[1024];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = swprintf_s(line, L"%02u:%02u:%02u.%03u %s", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, t_logTag);
    if (n < 0)
    {
        n = 0;
    }
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(line + n, sizeof(line) / sizeof(line[0]) - n, _TRUNCATE, format, args); // truncates long lines
    va_end(args);
    std::fwprintf(stderr, L"%s\n", line);
}

inline unsigned long Hr(HRESULT hr)
{
    return static_cast<unsigned long>(hr);
}

} // namespace dd
