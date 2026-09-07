#include "Common.h"
#include "Diagnostics.h"
#include "ImageFile.h"
#include "MainWindow.h"
#include "SelfTests.h"
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <cstdio>
#include <string>
#include <vector>

// Visual styles for the common controls, and per-monitor DPI, without a separate .rc.
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

std::vector<std::wstring> g_arguments;

bool HasFlag(const wchar_t* name) {
    for (const std::wstring& argument : g_arguments)
        if (argument == name) return true;
    return false;
}

// Repeatable option, for --at x,y.
std::vector<std::wstring> ValuesFor(const wchar_t* name) {
    std::vector<std::wstring> values;
    for (size_t i = 0; i + 1 < g_arguments.size(); ++i)
        if (g_arguments[i] == name) values.push_back(g_arguments[i + 1]);
    return values;
}

std::wstring ValueFor(const wchar_t* name) {
    for (size_t i = 0; i + 1 < g_arguments.size(); ++i)
        if (g_arguments[i] == name) return g_arguments[i + 1];
    return {};
}

// A GUI subsystem binary has no console of its own. Attaching to the one that launched it
// is what makes --self-test and --inspect usable from a shell without a window flashing
// up for every normal launch. When the caller redirected stdout to a file or a pipe, the
// process already has a usable handle and reopening CONOUT$ would throw the output away.
void AttachToParentConsole() {
    HANDLE existing = GetStdHandle(STD_OUTPUT_HANDLE);
    bool redirected = existing && existing != INVALID_HANDLE_VALUE
                   && GetFileType(existing) != FILE_TYPE_UNKNOWN;
    if (redirected) {
        SetConsoleOutputCP(CP_UTF8);
        return;
    }
    if (!AttachConsole(ATTACH_PARENT_PROCESS) && !AllocConsole()) return;
    FILE* stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
    SetConsoleOutputCP(CP_UTF8);
}

void Print(const std::string& text) {
    fputs(text.c_str(), stdout);
    fputc('\n', stdout);
}

int RunInspect(const std::wstring& path, const std::vector<std::wstring>& points) {
    try {
        LoadedImage loaded = ImageFile::Load(path);
        Print(Narrow(path));
        Print(Format("  %d × %d px · %s", loaded.image->Width(), loaded.image->Height(),
                     loaded.FormatText().c_str()));
        const ContentSummary& s = loaded.summary;
        Print(Format("  RGB 最小 %.4f / 最大 %.4f · 1.0超 %.3f%% · アルファ最大 %.4f",
                     s.minRGB, s.maxRGB, s.overOne * 100, s.maxAlpha));
        if (!loaded.whiteNote.empty()) Print("  " + loaded.whiteNote);
        // Same picker the window uses, so a file can be measured at named points without
        // opening it - which is also how two files get compared to each other.
        for (const std::wstring& point : points) {
            int x = 0, y = 0;
            if (swscanf_s(point.c_str(), L"%d,%d", &x, &y) != 2) continue;
            auto sample = loaded.image->SampleRect(RectD{(double)x, (double)y, 1, 1});
            if (!sample) {
                Print(Format("  (%d,%d) 画像の範囲外", x, y));
                continue;
            }
            Print(Format("  (%4d,%4d) %s", x, y, sample->Text().c_str()));
        }
        return 0;
    } catch (const std::exception& e) {
        Print(e.what());
        return 1;
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    int count = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count)) {
        for (int i = 1; i < count; ++i) g_arguments.push_back(argv[i]);
        LocalFree(argv);
    }

    bool consoleMode = HasFlag(L"--self-test") || HasFlag(L"--diagnose") || !ValueFor(L"--inspect").empty();
    CoInitializeEx(nullptr, consoleMode ? COINIT_MULTITHREADED
                                        : (COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));

    if (HasFlag(L"--self-test")) {
        AttachToParentConsole();
        std::wstring directory = ValueFor(L"--test-output");
        if (directory.empty()) {
            wchar_t temp[MAX_PATH];
            GetTempPathW((DWORD)std::size(temp), temp);
            directory = std::wstring(temp) + L"HDRScope-tests";
        }
        try {
            return RunSelfTests(directory);
        } catch (const std::exception& e) {
            Print(e.what());
            return 1;
        }
    }

    // What the tool can see of this machine, and optionally a headless capture. This is
    // where to look when the window list is empty or a capture is not what was expected.
    if (HasFlag(L"--diagnose")) {
        AttachToParentConsole();
        return RunDiagnostics(ValueFor(L"--capture"), ValuesFor(L"--at"), ValueFor(L"--save"));
    }

    // Measuring a saved file without opening the window: the same reader the app uses.
    if (std::wstring path = ValueFor(L"--inspect"); !path.empty()) {
        AttachToParentConsole();
        return RunInspect(path, ValuesFor(L"--at"));
    }

    std::wstring startupFile;
    for (const std::wstring& argument : g_arguments) {
        if (argument.size() > 1 && argument[0] == L'-') continue;
        if (ImageFile::CanRead(argument)) {
            startupFile = argument;
            break;
        }
    }

    StartupPattern pattern = HasFlag(L"--headroom-pattern") ? StartupPattern::Headroom
                           : HasFlag(L"--test-pattern")     ? StartupPattern::Fixed
                                                            : StartupPattern::None;
    int result = RunApp(instance, startupFile, pattern);
    CoUninitialize();
    return result;
}
