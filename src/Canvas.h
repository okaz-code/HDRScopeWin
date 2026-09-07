#pragma once
#include "Display.h"
#include "PixelImage.h"
#include <windows.h>
#include <functional>
#include <memory>
#include <optional>
#include <string>

// The image view: a child window with its own scRGB float16 swap chain, so values above
// SDR white reach the panel instead of being clipped on the way to the screen.
//
// Zoom is output pixels per source pixel. At 1:1 one image pixel is one physical display
// pixel; below 1.0 the shader area-averages, above it reads pixels without interpolation
// so that individual pixels stay distinguishable.
class Canvas {
public:
    Canvas();
    ~Canvas();

    bool Create(HWND parent, HINSTANCE instance, std::string& error);
    HWND Window() const { return window_; }

    void SetImage(PixelImagePtr image, bool preserveView);
    void Fit();
    void ZoomToActualPixels();
    void Redraw();
    // Headroom moves with the brightness slider without anything being drawn, so this is
    // polled rather than only recomputed when the view changes.
    void RefreshDisplay();
    const DisplayInfo& Display() const { return display_; }

    // The two lines shown under the image, or nothing when the pointer is off the image.
    const std::optional<std::string>& CopyableSample() const { return copyable_; }
    void CopySample();

    std::function<void(const std::string& sample, const std::string& detail)> onSample;
    std::function<void(const std::string& zoom)> onZoom;
    std::function<void(const std::string& status)> onStatus;
    std::function<void(const std::wstring& path)> onFileDropped;
    // Reports the display readout line, already formatted, including whether the current
    // image is brighter than the panel can show.
    std::function<void(const std::string& edr)> onDisplayReadout;
    // The maximum RGB of the image on screen, used to say whether the display or the
    // data is what is clipping.
    float imageMaxRGB = 0;
    bool hasImageMaxRGB = false;

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);
    bool CreateDeviceResources(std::string& error);
    void ResizeSwapChain();
    void UploadTexture();
    void Render();
    void SetZoom(double zoom, POINT anchor);
    void ViewChanged();
    void UpdateSample();

    struct Impl;
    std::unique_ptr<Impl> impl_;

    HWND window_ = nullptr;
    PixelImagePtr image_;
    DisplayInfo display_;

    double zoom_ = 1.0;
    double originX_ = 0, originY_ = 0;
    bool fitMode_ = true;
    bool tracking_ = false;
    bool dragging_ = false;
    POINT dragPoint_{};
    double dragTravel_ = 0;
    std::optional<POINT> cursor_;
    std::optional<std::string> copyable_;
    std::string lastReadout_;

    // Left click copies, left drag pans. Accumulated travel separates the two so a click
    // with a few pixels of hand tremor still counts as a click.
    static constexpr double kClickSlop = 3.0;
};
