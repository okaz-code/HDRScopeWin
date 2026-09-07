#include "Common.h"
#include <windows.h>
#include <exception>

std::wstring Widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), out.data(), n);
    return out;
}

std::string Narrow(const std::wstring& wide) {
    if (wide.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), out.data(), n, nullptr, nullptr);
    return out;
}

std::string HresultMessage(long hr) {
    LPWSTR buffer = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, (DWORD)hr, 0, (LPWSTR)&buffer, 0, nullptr);
    std::string text = n ? Narrow(std::wstring(buffer, n)) : std::string();
    if (buffer) LocalFree(buffer);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    return Format("0x%08X", (unsigned)hr) + (text.empty() ? "" : " " + text);
}

long long RunGuarded(const char* where, const std::function<long long()>& body) {
    try {
        return body();
    } catch (const std::exception& e) {
        std::string message = std::string(where) + " で処理を続けられません：\n" + e.what();
        MessageBoxW(nullptr, Widen(message).c_str(), L"HDRScope", MB_OK | MB_ICONERROR);
    } catch (...) {
        std::string message = std::string(where) + " で不明な例外が発生しました。";
        MessageBoxW(nullptr, Widen(message).c_str(), L"HDRScope", MB_OK | MB_ICONERROR);
    }
    return 0;
}
