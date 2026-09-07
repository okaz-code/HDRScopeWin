#include "Diagnostics.h"
#include "Capture.h"
#include "Display.h"
#include "HdrExport.h"
#include "PixelImage.h"
#include <windows.h>
#include <winrt/base.h>
#include <cstdio>

namespace {

// Flushed per line: a diagnostic that stalls has to have already shown how far it got.
void Line(const std::string& text) {
    fputs(text.c_str(), stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

BOOL CALLBACK MonitorProc(HMONITOR monitor, HDC, LPRECT, LPARAM index) {
    DisplayInfo info = QueryDisplay(monitor);
    int* n = reinterpret_cast<int*>(index);
    if (!info.valid) {
        Line(Format("  ディスプレイ %d: 情報を取得できません", ++*n));
        return TRUE;
    }
    Line(Format("  ディスプレイ %d: %s", ++*n, Narrow(info.name).c_str()));
    Line(Format("    HDR              : %s（対応 %s）",
                info.hdrEnabled ? "ON" : "OFF", info.hdrCapable ? "あり" : "なし"));
    Line(Format("    パネルピーク     : %.0f nit", info.maxLuminanceNits));
    Line(Format("    全画面持続       : %.0f nit", info.maxFullFrameNits));
    Line(Format("    SDR白            : %.0f nit", info.sdrWhiteNits));
    Line(Format("    ヘッドルーム     : %.2f×", info.Headroom()));
    Line(Format("    正規化係数       : ×%.4f（キャプチャ値をこれで割ります）", info.ScrgbPerSdrWhite()));
    return TRUE;
}

}  // namespace

int RunDiagnostics(const std::wstring& captureTitle, const std::vector<std::wstring>& points,
                   const std::wstring& savePath) {
    Line("HDRScope 診断");
    OSVERSIONINFOEXW version{sizeof(version)};
    // The documented way to read the real build number without a manifest lie.
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOEXW*);
        if (auto getVersion = (RtlGetVersionFn)GetProcAddress(ntdll, "RtlGetVersion"))
            getVersion(&version);
    }
    Line(Format("  Windows %lu.%lu build %lu", version.dwMajorVersion, version.dwMinorVersion,
                version.dwBuildNumber));
    Line(Format("  ウィンドウキャプチャ（Windows.Graphics.Capture）: %s",
                CaptureSupported() ? "利用可能" : "利用できません"));
    Line("");
    Line("ディスプレイ");
    int index = 0;
    EnumDisplayMonitors(nullptr, nullptr, MonitorProc, reinterpret_cast<LPARAM>(&index));

    std::vector<WindowItem> windows = EnumerateWindows();
    size_t listed = 0;
    for (const WindowItem& item : windows)
        if (item.LikelyRealWindow()) ++listed;
    Line("");
    Line(Format("ウィンドウ: 全%zu個、一覧に出るもの%zu個", windows.size(), listed));

    if (captureTitle.empty()) return 0;

    const WindowItem* target = nullptr;
    for (const WindowItem& item : windows) {
        if (item.title.find(captureTitle) != std::wstring::npos
            || item.app.find(captureTitle) != std::wstring::npos) {
            target = &item;
            break;
        }
    }
    if (!target) {
        Line("");
        Line("該当するウィンドウが見つかりません：" + Narrow(captureTitle));
        return 1;
    }

    Line("");
    Line(Format("キャプチャ: %s — %s (%s)", Narrow(target->app).c_str(), Narrow(target->title).c_str(),
                Narrow(target->SizeText()).c_str()));
    try {
        CaptureOutcome outcome = CaptureWindow(*target);
        ContentSummary summary = outcome.image->Summary();
        Line(Format("  %d × %d px · 正規化 ×%.4f（SDR白 %.0f nit）",
                    outcome.image->Width(), outcome.image->Height(),
                    outcome.normalization, outcome.display.sdrWhiteNits));
        Line(Format("  RGB 最小 %.4f / 最大 %.4f · 1.0超 %.3f%% · アルファ最大 %.4f",
                    summary.minRGB, summary.maxRGB, summary.overOne * 100, summary.maxAlpha));
        if (summary.maxAlpha <= 0) Line("  全画素が完全に透明です。このウィンドウには描画内容がありません。");
        else if (summary.uniform) Line("  全画素が同一の値です。");

        for (const std::wstring& point : points) {
            int x = 0, y = 0;
            if (swscanf_s(point.c_str(), L"%d,%d", &x, &y) != 2) continue;
            auto sample = outcome.image->SampleRect(RectD{(double)x, (double)y, 1, 1});
            if (!sample) {
                Line(Format("  (%d,%d) 画像の範囲外", x, y));
                continue;
            }
            Line(Format("  (%4d,%4d) %s", x, y, sample->Text().c_str()));
        }

        if (!savePath.empty()) {
            // The extension picks the format, the same two the window offers.
            size_t dot = savePath.find_last_of(L'.');
            std::wstring ext = dot == std::wstring::npos ? L"" : savePath.substr(dot + 1);
            for (wchar_t& c : ext) c = (wchar_t)towlower(c);
            SaveFormat format = ext == L"jxr" ? SaveFormat::HdrJxr : SaveFormat::FloatTiff;
            SaveImage(*outcome.image, format, savePath);
            Line("  保存: " + Narrow(savePath) + "（" + FormatInfo(format).doneMessage + "）");
        }
    } catch (const std::exception& e) {
        Line(std::string("  キャプチャに失敗しました：") + e.what());
        return 1;
    }
    return 0;
}
