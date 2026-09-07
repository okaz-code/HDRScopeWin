#pragma once
#include <string>
#include <stdexcept>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <vector>

// Messages are authored in UTF-8 and only widened at the Win32 boundary, so the same
// string reaches a message box, the status line and stderr without a second encoding.
class ScopeError : public std::runtime_error {
public:
    explicit ScopeError(std::string message) : std::runtime_error(std::move(message)) {}
};

std::wstring Widen(const std::string& utf8);
std::string Narrow(const std::wstring& wide);

// snprintf into a std::string. Every formatted readout in the app goes through this so
// the number formatting matches the macOS build's String(format:) call sites exactly.
template <typename... Args>
std::string Format(const char* fmt, Args... args) {
    int n = std::snprintf(nullptr, 0, fmt, args...);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    std::snprintf(out.data(), static_cast<size_t>(n) + 1, fmt, args...);
    return out;
}

std::string HresultMessage(long hr);

// A C++ exception that escapes a window procedure unwinds through a kernel callback and
// kills the process with no message at all. Every message handler runs inside this so a
// failure says what it was instead of the window simply vanishing.
long long RunGuarded(const char* where, const std::function<long long()>& body);
