#pragma once
#include "Display.h"
#include "PixelImage.h"
#include <windows.h>
#include <string>
#include <vector>

struct WindowItem {
    // Below this, a top level window is a helper rather than something worth measuring.
    static constexpr int kMinimumSide = 40;

    HWND hwnd = nullptr;
    DWORD pid = 0;
    std::wstring app;
    std::wstring title;
    bool hasTitle = false;
    bool onScreen = false;
    bool toolWindow = false;
    int width = 0;
    int height = 0;
    uint64_t processStart = 0;   // FILETIME, to notice a recycled window handle

    // A desktop session is full of top level windows that are not windows anyone sees:
    // message-only helpers, cloaked shell surfaces, tool palettes parked off screen.
    // Those are what fill the list with duplicates of one app and what capture into an
    // empty image, so keep them out of the default listing.
    bool LikelyRealWindow() const {
        return onScreen && !toolWindow && width >= kMinimumSide && height >= kMinimumSide;
    }
    std::wstring SizeText() const;
    // The window ID shown in the list. Handles are per-session, which is exactly what
    // the list needs: a stable way to name one window while the app is running.
    unsigned Id() const { return (unsigned)(uintptr_t)hwnd; }
};

std::vector<WindowItem> EnumerateWindows();
// Re-reads one window, so a capture can confirm it is still the same window of the same
// still-running process rather than a handle the system has since reused.
bool RefreshWindow(WindowItem& item);

struct CaptureOutcome {
    PixelImagePtr image;
    DisplayInfo display;
    // scRGB per SDR white at capture time. Captured values were divided by this, so
    // multiplying a measurement by it gives scRGB, and by 80x that gives nits.
    double normalization = 1.0;
};

// Captures one window as float16 scRGB through Windows.Graphics.Capture, converts to
// straight alpha and normalizes so that 1.0 is SDR reference white.
CaptureOutcome CaptureWindow(const WindowItem& item);
bool CaptureSupported();
