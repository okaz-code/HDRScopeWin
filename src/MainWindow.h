#pragma once
#include <windows.h>
#include <string>

// Which known-value pattern to show at startup, if any. Both need no capture at all.
enum class StartupPattern { None, Fixed, Headroom };

// Runs the application window. `startupFile` opens a saved image instead of listing windows.
int RunApp(HINSTANCE instance, const std::wstring& startupFile, StartupPattern pattern);
