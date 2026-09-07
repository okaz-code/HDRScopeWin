#pragma once
#include "Common.h"
#include <windows.h>

// What the display is currently doing, which is what decides both how a capture is
// normalized and how much of an HDR image the panel can actually show right now.
//
// Windows composites in scRGB, where 1.0 is fixed at 80 nit. SDR white is not there: it
// sits at whatever the SDR content brightness slider is set to, reported as the SDR white
// level. So a plain white SDR window captures as sdrWhiteNits/80, not as 1.0. Dividing by
// that is what puts a capture on this tool's scale, where 1.0 is SDR reference white.
struct DisplayInfo {
    bool valid = false;
    bool hdrCapable = false;       // the panel and connection can do advanced colour
    bool hdrEnabled = false;       // HDR is switched on right now
    double maxLuminanceNits = 0;   // panel peak, small highlight
    double maxFullFrameNits = 0;   // what it sustains over the whole screen
    double minLuminanceNits = 0;
    double sdrWhiteNits = 80.0;    // where SDR white lands, from the brightness slider
    std::wstring name;

    // Multiply a value on this tool's scale by this to get scRGB, divide to come back.
    double ScrgbPerSdrWhite() const { return sdrWhiteNits / 80.0; }
    // How far above SDR white the panel can still go, the counterpart of the EDR
    // headroom the macOS build reports. 1.0 means everything above SDR white clips.
    double Headroom() const {
        return hdrEnabled && sdrWhiteNits > 0 ? maxLuminanceNits / sdrWhiteNits : 1.0;
    }
};

DisplayInfo QueryDisplay(HMONITOR monitor);
DisplayInfo QueryDisplayForWindow(HWND window);
