#include "Display.h"
#include <dxgi1_6.h>
#include <winrt/base.h>
#include <vector>

using winrt::com_ptr;

namespace {

// The SDR white level lives in the display configuration rather than in DXGI, and it is
// what moves when the user drags the SDR content brightness slider.
bool QuerySdrWhiteLevel(const std::wstring& gdiDeviceName, double& nits, bool& advancedColorEnabled,
                        bool& advancedColorSupported, std::wstring& friendlyName) {
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) return false;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr)
        != ERROR_SUCCESS) return false;
    paths.resize(pathCount);

    for (const auto& path : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;
        if (gdiDeviceName != source.viewGdiDeviceName) continue;

        DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof(white);
        white.header.adapterId = path.targetInfo.adapterId;
        white.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS && white.SDRWhiteLevel > 0)
            nits = white.SDRWhiteLevel / 1000.0 * 80.0;

        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO advanced{};
        advanced.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
        advanced.header.size = sizeof(advanced);
        advanced.header.adapterId = path.targetInfo.adapterId;
        advanced.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&advanced.header) == ERROR_SUCCESS) {
            advancedColorSupported = advanced.advancedColorSupported != 0;
            advancedColorEnabled = advanced.advancedColorEnabled != 0;
        }

        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = path.targetInfo.adapterId;
        target.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS)
            friendlyName = target.monitorFriendlyDeviceName;
        return true;
    }
    return false;
}

}  // namespace

DisplayInfo QueryDisplay(HMONITOR monitor) {
    DisplayInfo info;
    if (!monitor) return info;

    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo)) return info;

    com_ptr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.put_void()))) return info;
    com_ptr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, adapter.put()) != DXGI_ERROR_NOT_FOUND; ++a) {
        com_ptr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, output.put()) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC desc{};
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor) {
                if (auto output6 = output.try_as<IDXGIOutput6>()) {
                    DXGI_OUTPUT_DESC1 desc1{};
                    if (SUCCEEDED(output6->GetDesc1(&desc1))) {
                        info.valid = true;
                        info.maxLuminanceNits = desc1.MaxLuminance;
                        info.maxFullFrameNits = desc1.MaxFullFrameLuminance;
                        info.minLuminanceNits = desc1.MinLuminance;
                        // PQ on the wire is what HDR being switched on looks like here.
                        info.hdrEnabled = desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                        info.hdrCapable = info.hdrEnabled;
                        info.name = desc.DeviceName;
                    }
                }
            }
            output = nullptr;
            if (info.valid) break;
        }
        adapter = nullptr;
        if (info.valid) break;
    }
    if (!info.valid) return info;

    bool enabled = info.hdrEnabled, supported = info.hdrCapable;
    std::wstring friendlyName;
    if (QuerySdrWhiteLevel(monitorInfo.szDevice, info.sdrWhiteNits, enabled, supported, friendlyName)) {
        info.hdrEnabled = enabled;
        info.hdrCapable = supported;
        if (!friendlyName.empty()) info.name = friendlyName;
    }
    // With HDR off there is no headroom and SDR white is the scRGB reference by
    // definition, so a capture needs no rescaling at all.
    if (!info.hdrEnabled) info.sdrWhiteNits = 80.0;
    return info;
}

DisplayInfo QueryDisplayForWindow(HWND window) {
    return QueryDisplay(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY));
}
