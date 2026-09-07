#pragma once
#include <string>
#include <vector>

// Reports what the tool can see of the machine: every display's HDR state and the
// normalization factor that follows from it, whether window capture is available, and
// what the window list looks like. Optionally captures one window and measures it, which
// is the end-to-end check that the capture path returns the values it should.
int RunDiagnostics(const std::wstring& captureTitle, const std::vector<std::wstring>& points,
                   const std::wstring& savePath);
