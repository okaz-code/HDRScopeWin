#pragma once
#include "PixelImage.h"
#include <string>

// Two files for two jobs. Windows shows a float TIFF clipped to SDR and renders a float
// JPEG XR as HDR, so neither one alone both measures and previews.
enum class SaveFormat { FloatTiff, HdrJxr };

struct SaveFormatInfo {
    const wchar_t* extension;
    const char* menuTitle;
    const char* dialogTitle;
    const char* progressMessage;
    const char* doneMessage;
    const wchar_t* filter;
};

const SaveFormatInfo& FormatInfo(SaveFormat format);
void SaveImage(const PixelImage& image, SaveFormat format, const std::wstring& path);
