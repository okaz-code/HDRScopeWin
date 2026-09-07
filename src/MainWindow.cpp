#include "MainWindow.h"
#include "Canvas.h"
#include "Capture.h"
#include "FloatTiff.h"
#include "HdrExport.h"
#include "ImageFile.h"
#include "PixelImage.h"
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <objbase.h>
#include <shellapi.h>
#include <winrt/base.h>
#include <algorithm>
#include <memory>
#include <thread>
#include <vector>

namespace {

enum : int {
    IDC_OPEN = 1001, IDC_CAPTURE, IDC_RECAPTURE, IDC_SAVE, IDC_REFRESH, IDC_FILTER,
    IDC_SHOW_ALL, IDC_TREE, IDC_PATTERN, IDC_PATTERN_HEADROOM, IDC_FIT, IDC_ACTUAL,
    IDC_TITLE, IDC_ZOOM, IDC_HINT, IDC_EDR, IDC_SAMPLE, IDC_DETAIL, IDC_STATUS, IDC_LABEL,
    IDM_OPEN = 2001, IDM_SAVE_TIFF, IDM_SAVE_JXR, IDM_EXIT, IDM_COPY, IDM_ABOUT,
};

// Must match Resources/HDRScope.rc, where being resource 1 is what makes the shell
// use this icon for the executable itself.
constexpr int kAppIcon = 1;

constexpr UINT WM_JOB_DONE = WM_APP + 1;
constexpr UINT_PTR kDisplayTimer = 1;

struct Job {
    enum class Kind { Capture, Load, Save } kind = Kind::Capture;
    bool ok = false;
    std::string error;
    std::string status;
    PixelImagePtr image;
    float maxRGB = 0;
    bool hasMaxRGB = false;
    std::wstring label;
    bool preserveView = false;
};

std::wstring TimestampForFilename() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t buffer[64];
    swprintf_s(buffer, L"HDRScope-%04d-%02d-%02d-%02d-%02d-%02d",
               now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    return buffer;
}

std::string ClockText() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    return Format("%02d:%02d:%02d", now.wHour, now.wMinute, now.wSecond);
}

class MainWindow {
public:
    int Run(HINSTANCE instance, const std::wstring& startupFile, StartupPattern pattern);

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    void CreateControls();
    void CreateFonts();
    void Layout();
    void SetBusy(bool busy);
    void UpdateButtons();
    void SetStatus(const std::string& text);
    void SetText(int id, const std::string& text);
    void ReportError(const std::string& message);

    void RefreshWindowList();
    void RebuildTree();
    void UpdateListStatus();
    const WindowItem* Selected() const;

    void StartCapture(bool repeatLast);
    void StartLoad(const std::wstring& path);
    void StartSave(SaveFormat format);
    void ShowTestPattern();
    void ShowHeadroomPattern();
    void OpenDialog();
    void ShowSaveMenu();
    void AdoptImage(PixelImagePtr image, float maxRGB, const std::wstring& label,
                    const std::string& status, bool preserveView);

    template <typename Work>
    void StartJob(Job::Kind kind, Work work);

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    HWND tree_ = nullptr;
    HFONT uiFont_ = nullptr;
    HFONT monoFont_ = nullptr;
    HFONT titleFont_ = nullptr;
    int dpi_ = 96;

    Canvas canvas_;
    std::vector<WindowItem> allWindows_;
    std::vector<const WindowItem*> shown_;
    HWND selectedWindow_ = nullptr;
    WindowItem lastTarget_{};
    bool hasLastTarget_ = false;
    bool showAll_ = false;
    std::wstring filter_;
    bool busy_ = false;
    PixelImagePtr image_;
};

MainWindow* g_main = nullptr;

int Scaled(int value, int dpi) { return MulDiv(value, dpi, 96); }

LRESULT CALLBACK MainWindow::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (!g_main) return DefWindowProcW(hwnd, msg, wp, lp);
    return (LRESULT)RunGuarded("メインウィンドウ",
                               [&] { return (long long)g_main->WndProc(hwnd, msg, wp, lp); });
}

void MainWindow::CreateFonts() {
    if (uiFont_) DeleteObject(uiFont_);
    if (monoFont_) DeleteObject(monoFont_);
    if (titleFont_) DeleteObject(titleFont_);
    NONCLIENTMETRICSW metrics{sizeof(NONCLIENTMETRICSW)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, (UINT)dpi_);
    uiFont_ = CreateFontIndirectW(&metrics.lfMessageFont);
    LOGFONTW title = metrics.lfMessageFont;
    title.lfHeight = (LONG)(title.lfHeight * 1.35);
    title.lfWeight = FW_BOLD;
    titleFont_ = CreateFontIndirectW(&title);
    LOGFONTW mono{};
    mono.lfHeight = -Scaled(15, dpi_);
    mono.lfWeight = FW_MEDIUM;
    mono.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(mono.lfFaceName, L"Consolas");
    monoFont_ = CreateFontIndirectW(&mono);
}

void MainWindow::CreateControls() {
    auto button = [&](int id, const wchar_t* text, DWORD extra = 0) {
        return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | extra,
                               0, 0, 10, 10, window_, (HMENU)(INT_PTR)id, instance_, nullptr);
    };
    auto label = [&](int id, const wchar_t* text, DWORD extra = 0) {
        return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | extra,
                               0, 0, 10, 10, window_, (HMENU)(INT_PTR)id, instance_, nullptr);
    };

    label(IDC_TITLE, L"HDRScope");
    button(IDC_OPEN, L"開く…");
    button(IDC_CAPTURE, L"キャプチャ");
    button(IDC_RECAPTURE, L"再キャプチャ");
    button(IDC_SAVE, L"保存…");
    label(IDC_LABEL, L"ウィンドウ");
    button(IDC_REFRESH, L"更新");
    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                    0, 0, 10, 10, window_, (HMENU)(INT_PTR)IDC_FILTER, instance_, nullptr);
    button(IDC_SHOW_ALL, L"非表示・小さなウィンドウも表示", BS_AUTOCHECKBOX);
    tree_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_LINESATROOT
                            | TVS_SHOWSELALWAYS | TVS_FULLROWSELECT,
                            0, 0, 10, 10, window_, (HMENU)(INT_PTR)IDC_TREE, instance_, nullptr);
    button(IDC_PATTERN, L"テストパターン ×30");
    button(IDC_PATTERN_HEADROOM, L"ヘッドルーム階調");
    label(IDC_ZOOM, L"—", SS_RIGHT);
    button(IDC_FIT, L"全体表示");
    button(IDC_ACTUAL, L"1:1");
    label(IDC_HINT, L"ホイール：ズーム　ドラッグ：移動　クリック：コピー", SS_ENDELLIPSIS);
    label(IDC_EDR, L"", SS_RIGHT | SS_ENDELLIPSIS);
    label(IDC_SAMPLE, L"カーソルを画像に重ねるとRGB値を表示します。");
    label(IDC_DETAIL, L"拡張リニアsRGB · 1.0 = SDR白");
    label(IDC_STATUS, L"ウィンドウを選択してキャプチャしてください。");

    EnumChildWindows(window_, [](HWND child, LPARAM font) {
        SendMessageW(child, WM_SETFONT, (WPARAM)font, TRUE);
        return TRUE;
    }, (LPARAM)uiFont_);
    SendMessageW(GetDlgItem(window_, IDC_TITLE), WM_SETFONT, (WPARAM)titleFont_, TRUE);
    SendMessageW(GetDlgItem(window_, IDC_SAMPLE), WM_SETFONT, (WPARAM)monoFont_, TRUE);
}

void MainWindow::Layout() {
    RECT client{};
    GetClientRect(window_, &client);
    int width = client.right, height = client.bottom;
    const int pad = Scaled(10, dpi_);
    const int row = Scaled(26, dpi_);
    const int sidebar = std::min(Scaled(300, dpi_), width / 3);

    auto place = [&](int id, int x, int y, int w, int h) {
        SetWindowPos(GetDlgItem(window_, id), nullptr, x, y, w, h, SWP_NOZORDER);
    };

    int x = pad, y = pad;
    place(IDC_TITLE, x, y + Scaled(2, dpi_), Scaled(110, dpi_), row);
    x += Scaled(118, dpi_);
    const struct { int id; int width; } toolbar[] = {
        {IDC_OPEN, 84}, {IDC_CAPTURE, 100}, {IDC_RECAPTURE, 100}, {IDC_SAVE, 84},
    };
    for (const auto& item : toolbar) {
        place(item.id, x, y, Scaled(item.width, dpi_), row);
        x += Scaled(item.width + 8, dpi_);
    }

    int top = y + row + pad;
    int bottomHeight = Scaled(84, dpi_);
    int bodyHeight = height - top - bottomHeight - pad;

    // Sidebar: the window list and what filters it.
    int sx = pad, sy = top;
    place(IDC_LABEL, sx, sy + Scaled(4, dpi_), sidebar - Scaled(70, dpi_), row);
    place(IDC_REFRESH, sx + sidebar - Scaled(64, dpi_), sy, Scaled(64, dpi_), row);
    sy += row + Scaled(6, dpi_);
    place(IDC_FILTER, sx, sy, sidebar, row);
    sy += row + Scaled(6, dpi_);
    place(IDC_SHOW_ALL, sx, sy, sidebar, Scaled(22, dpi_));
    sy += Scaled(28, dpi_);
    int patternTop = top + bodyHeight - row;
    place(IDC_TREE, sx, sy, sidebar, std::max(Scaled(60, dpi_), patternTop - sy - Scaled(8, dpi_)));
    int half = (sidebar - Scaled(6, dpi_)) / 2;
    place(IDC_PATTERN, sx, patternTop, half, row);
    place(IDC_PATTERN_HEADROOM, sx + half + Scaled(6, dpi_), patternTop, sidebar - half - Scaled(6, dpi_), row);

    // Image side: label row, canvas, hint row.
    int cx = pad * 2 + sidebar;
    int cw = std::max(Scaled(200, dpi_), width - cx - pad);
    int cy = top;
    place(IDC_ZOOM, cx + cw - Scaled(220, dpi_), cy + Scaled(4, dpi_), Scaled(70, dpi_), row);
    place(IDC_FIT, cx + cw - Scaled(146, dpi_), cy, Scaled(80, dpi_), row);
    place(IDC_ACTUAL, cx + cw - Scaled(62, dpi_), cy, Scaled(62, dpi_), row);
    cy += row + Scaled(6, dpi_);
    int hintTop = top + bodyHeight - Scaled(20, dpi_);
    SetWindowPos(canvas_.Window(), nullptr, cx, cy, cw, std::max(Scaled(60, dpi_), hintTop - cy - Scaled(6, dpi_)),
                 SWP_NOZORDER);
    int hintWidth = std::min(Scaled(300, dpi_), cw / 2);
    place(IDC_HINT, cx, hintTop, hintWidth, Scaled(20, dpi_));
    place(IDC_EDR, cx + hintWidth, hintTop, cw - hintWidth, Scaled(20, dpi_));

    int by = height - bottomHeight;
    place(IDC_SAMPLE, pad, by, width - pad * 2, Scaled(24, dpi_));
    place(IDC_DETAIL, pad, by + Scaled(26, dpi_), width - pad * 2, Scaled(18, dpi_));
    place(IDC_STATUS, pad, by + Scaled(46, dpi_), width - pad * 2, Scaled(34, dpi_));
}

void MainWindow::SetText(int id, const std::string& text) {
    SetWindowTextW(GetDlgItem(window_, id), Widen(text).c_str());
}

void MainWindow::SetStatus(const std::string& text) { SetText(IDC_STATUS, text); }

void MainWindow::ReportError(const std::string& message) {
    SetStatus("処理に失敗しました。");
    MessageBoxW(window_, Widen(message).c_str(), L"HDRScope", MB_OK | MB_ICONWARNING);
}

void MainWindow::SetBusy(bool busy) {
    busy_ = busy;
    UpdateButtons();
    SetCursor(LoadCursorW(nullptr, busy ? IDC_WAIT : IDC_ARROW));
}

void MainWindow::UpdateButtons() {
    EnableWindow(GetDlgItem(window_, IDC_OPEN), !busy_);
    EnableWindow(GetDlgItem(window_, IDC_REFRESH), !busy_);
    EnableWindow(GetDlgItem(window_, IDC_PATTERN), !busy_);
    EnableWindow(GetDlgItem(window_, IDC_PATTERN_HEADROOM), !busy_);
    EnableWindow(GetDlgItem(window_, IDC_CAPTURE), !busy_ && Selected() != nullptr);
    EnableWindow(GetDlgItem(window_, IDC_RECAPTURE), !busy_ && hasLastTarget_);
    EnableWindow(GetDlgItem(window_, IDC_SAVE), !busy_ && image_ != nullptr);
    EnableWindow(GetDlgItem(window_, IDC_FIT), image_ != nullptr);
    EnableWindow(GetDlgItem(window_, IDC_ACTUAL), image_ != nullptr);
}

const WindowItem* MainWindow::Selected() const {
    if (!selectedWindow_) return nullptr;
    for (const WindowItem* item : shown_)
        if (item->hwnd == selectedWindow_) return item;
    return nullptr;
}

void MainWindow::RefreshWindowList() {
    allWindows_ = EnumerateWindows();
    RebuildTree();
}

void MainWindow::RebuildTree() {
    std::wstring needle = filter_;
    for (wchar_t& c : needle) c = (wchar_t)towlower(c);
    while (!needle.empty() && needle.back() == L' ') needle.pop_back();

    shown_.clear();
    for (const WindowItem& item : allWindows_) {
        if (!showAll_ && !item.LikelyRealWindow()) continue;
        if (!needle.empty()) {
            std::wstring app = item.app, title = item.title;
            for (wchar_t& c : app) c = (wchar_t)towlower(c);
            for (wchar_t& c : title) c = (wchar_t)towlower(c);
            bool match = app.find(needle) != std::wstring::npos
                      || title.find(needle) != std::wstring::npos
                      || std::to_wstring(item.Id()) == needle;
            if (!match) continue;
        }
        shown_.push_back(&item);
    }

    SendMessageW(tree_, WM_SETREDRAW, FALSE, 0);
    TreeView_DeleteAllItems(tree_);
    HTREEITEM group = nullptr;
    HTREEITEM selectItem = nullptr;
    std::wstring currentApp;
    for (const WindowItem* item : shown_) {
        if (!group || item->app != currentApp) {
            currentApp = item->app;
            TVINSERTSTRUCTW insert{};
            insert.hParent = TVI_ROOT;
            insert.hInsertAfter = TVI_LAST;
            insert.item.mask = TVIF_TEXT | TVIF_PARAM;
            insert.item.pszText = const_cast<wchar_t*>(currentApp.c_str());
            insert.item.lParam = 0;
            group = TreeView_InsertItem(tree_, &insert);
        }
        std::wstring text = item->title + L"  —  " + item->SizeText() + L" · ID " + std::to_wstring(item->Id());
        if (!item->onScreen) text += L" · 非表示";
        TVINSERTSTRUCTW insert{};
        insert.hParent = group;
        insert.hInsertAfter = TVI_LAST;
        insert.item.mask = TVIF_TEXT | TVIF_PARAM;
        insert.item.pszText = const_cast<wchar_t*>(text.c_str());
        insert.item.lParam = (LPARAM)item->hwnd;
        HTREEITEM inserted = TreeView_InsertItem(tree_, &insert);
        if (item->hwnd == selectedWindow_) selectItem = inserted;
    }
    for (HTREEITEM root = TreeView_GetRoot(tree_); root; root = TreeView_GetNextSibling(tree_, root))
        TreeView_Expand(tree_, root, TVE_EXPAND);
    SendMessageW(tree_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(tree_, nullptr, TRUE);
    if (selectItem) TreeView_SelectItem(tree_, selectItem);
    else selectedWindow_ = nullptr;
    UpdateListStatus();
    UpdateButtons();
}

void MainWindow::UpdateListStatus() {
    size_t shown = shown_.size(), hidden = allWindows_.size() - shown;
    SetStatus(hidden > 0
        ? Format("%zu個のウィンドウを表示中。画面に出ていない・小さなウィンドウ%zu個を除外しています。", shown, hidden)
        : Format("%zu個のウィンドウ。", shown));
}

template <typename Work>
void MainWindow::StartJob(Job::Kind kind, Work work) {
    if (busy_) return;
    SetBusy(true);
    HWND target = window_;
    std::thread([target, kind, work]() mutable {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        auto* job = new Job();
        job->kind = kind;
        try {
            work(*job);
            job->ok = true;
        } catch (const std::exception& e) {
            job->error = e.what();
        } catch (const winrt::hresult_error& e) {
            job->error = Narrow(std::wstring(e.message()));
        } catch (...) {
            job->error = "不明なエラーが発生しました。";
        }
        PostMessageW(target, WM_JOB_DONE, 0, (LPARAM)job);
    }).detach();
}

void MainWindow::AdoptImage(PixelImagePtr image, float maxRGB, const std::wstring& label,
                            const std::string& status, bool preserveView) {
    image_ = image;
    canvas_.imageMaxRGB = maxRGB;
    canvas_.hasImageMaxRGB = true;
    canvas_.SetImage(image_, preserveView);
    SetText(IDC_SAMPLE, "カーソルを画像に重ねるとRGB値を表示します。");
    SetText(IDC_DETAIL, "拡張リニアsRGB · 1.0 = SDR白");
    SetStatus(status);
    std::wstring title = L"HDRScope — " + label;
    SetWindowTextW(window_, title.c_str());
    UpdateButtons();
}

void MainWindow::StartCapture(bool repeatLast) {
    WindowItem wanted;
    if (repeatLast) {
        if (!hasLastTarget_) return;
        wanted = lastTarget_;
    } else {
        const WindowItem* selected = Selected();
        if (!selected) return;
        wanted = *selected;
    }
    SetStatus(Narrow(wanted.app) + "をHDRキャプチャ中…");
    bool preserve = repeatLast && image_ != nullptr;
    StartJob(Job::Kind::Capture, [wanted, preserve](Job& job) mutable {
        // The handle and the process both have to still be the ones that were listed,
        // or this is a different window wearing the same number.
        if (!RefreshWindow(wanted))
            throw ScopeError("対象ウィンドウは閉じられたか、現在取得できません。"
                             "一覧を更新して選び直してください。最後の画像は保持しています。");
        CaptureOutcome outcome = CaptureWindow(wanted);
        ContentSummary summary = outcome.image->Summary();
        job.image = outcome.image;
        job.maxRGB = summary.maxRGB;
        job.hasMaxRGB = true;
        job.label = wanted.app + L" — " + wanted.title;
        job.preserveView = preserve;

        std::string size = Format("%d × %d px", outcome.image->Width(), outcome.image->Height());
        std::string normalization = Format("正規化 ×%.3f（SDR白 %.0f nit）",
                                           outcome.normalization, outcome.display.sdrWhiteNits);
        if (summary.maxAlpha <= 0) {
            job.status = size + " · " + normalization + " · " + ClockText()
                       + " · 全画素が完全に透明です。このウィンドウには描画内容がありません。";
        } else if (summary.uniform) {
            job.status = size + " · " + normalization + " · " + ClockText() + " · 全画素が同一の値です。";
        } else {
            job.status = size + " · " + normalization + " · " + summary.HdrText() + " · " + ClockText();
        }
    });
    if (!repeatLast) {
        lastTarget_ = wanted;
        hasLastTarget_ = true;
    }
}

void MainWindow::StartLoad(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    SetStatus(Narrow(name) + " を読み込み中…");
    StartJob(Job::Kind::Load, [path, name](Job& job) {
        LoadedImage loaded = ImageFile::Load(path);
        job.image = loaded.image;
        job.maxRGB = loaded.summary.maxRGB;
        job.hasMaxRGB = true;
        job.label = name;
        job.status = Format("%d × %d px · %s · %s",
                            loaded.image->Width(), loaded.image->Height(),
                            loaded.FormatText().c_str(), loaded.summary.HdrText().c_str());
        if (!loaded.whiteNote.empty()) job.status += " · " + loaded.whiteNote;
    });
}

void MainWindow::StartSave(SaveFormat format) {
    if (!image_ || busy_) return;
    const SaveFormatInfo& info = FormatInfo(format);
    std::wstring name = TimestampForFilename() + L"." + info.extension;
    std::vector<wchar_t> buffer(name.begin(), name.end());
    buffer.resize(MAX_PATH * 2, L'\0');

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window_;
    dialog.lpstrFilter = info.filter;
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = (DWORD)buffer.size();
    dialog.lpstrDefExt = info.extension;
    std::wstring title = Widen(info.dialogTitle);
    dialog.lpstrTitle = title.c_str();
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetSaveFileNameW(&dialog)) return;

    std::wstring path = buffer.data();
    PixelImagePtr image = image_;
    SetStatus(info.progressMessage);
    std::string done = info.doneMessage;
    StartJob(Job::Kind::Save, [image, format, path, done](Job& job) {
        SaveImage(*image, format, path);
        size_t slash = path.find_last_of(L"\\/");
        job.status = done + "：" + Narrow(slash == std::wstring::npos ? path : path.substr(slash + 1));
    });
}

void MainWindow::OpenDialog() {
    if (busy_) return;
    std::vector<wchar_t> buffer(MAX_PATH * 2, L'\0');
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window_;
    dialog.lpstrFilter = ImageFile::FilterSpec();
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = (DWORD)buffer.size();
    dialog.lpstrTitle = L"TIFF / JXR / HEIC などを開いて測定します";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (GetOpenFileNameW(&dialog)) StartLoad(buffer.data());
}

void MainWindow::ShowSaveMenu() {
    if (busy_ || !image_) return;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_SAVE_TIFF, Widen(FormatInfo(SaveFormat::FloatTiff).menuTitle).c_str());
    AppendMenuW(menu, MF_STRING, IDM_SAVE_JXR, Widen(FormatInfo(SaveFormat::HdrJxr).menuTitle).c_str());
    RECT button{};
    GetWindowRect(GetDlgItem(window_, IDC_SAVE), &button);
    TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN, button.left, button.bottom, 0, window_, nullptr);
    DestroyMenu(menu);
}

void MainWindow::ShowTestPattern() {
    if (busy_) return;
    auto pattern = PixelImage::TestPattern();
    AdoptImage(pattern, PixelImage::TestPatternLevels().back(), L"HDRテストパターン ×30（既知のリニアRGB値）",
               "上から白・赤・緑・青の階調。左から 0 / 0.09 / 0.18 / 0.5、以降は1刻みで30まで。"
               "最下段は色域外・負値。明るさが変わらなくなる段が現在の表示上限です（右下の表示を参照）。",
               false);
}

// The fixed ladder spends most of its columns above anything a panel can show. This one
// is built for the display in front of you, so the step where the plateau starts is the
// headroom to within one step instead of to within a whole multiple of SDR white.
void MainWindow::ShowHeadroomPattern() {
    if (busy_) return;
    const DisplayInfo& display = canvas_.Display();
    PixelImage::HeadroomLadder ladder = PixelImage::HeadroomLevels(display.Headroom());
    AdoptImage(PixelImage::Pattern(ladder.levels), ladder.top,
               L"ヘッドルーム階調（既知のリニアRGB値）",
               Format("0 から %.4g まで %.4g 刻み（%zu段）。現在のヘッドルーム ×%.2f", ladder.top,
                      ladder.step, ladder.levels.size(), ladder.headroom)
                   + (display.valid && display.hdrEnabled
                          ? Format("（パネル %.0f nit / SDR白 %.0f nit）", display.maxLuminanceNits,
                                   display.sdrWhiteNits)
                          : std::string("（HDRがOFFのため1.00×として作成しました）"))
                   + "。明るさが変わらなくなる段が現在の表示上限です。"
                   + (ladder.reachesHeadroom
                          ? "ヘッドルームの先にも4段あるので、頭打ちが段の消失として見えます。"
                          : "ヘッドルームが大きすぎて階調が届いていません。"
                            "この階調では頭打ちは見えないので、テストパターン ×30 を使ってください。")
                   + "輝度設定を変えたら押し直してください。",
               false);
}

LRESULT MainWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wp);
        switch (id) {
        case IDC_OPEN: case IDM_OPEN: OpenDialog(); return 0;
        case IDC_CAPTURE: StartCapture(false); return 0;
        case IDC_RECAPTURE: StartCapture(true); return 0;
        case IDC_SAVE: ShowSaveMenu(); return 0;
        case IDM_SAVE_TIFF: StartSave(SaveFormat::FloatTiff); return 0;
        case IDM_SAVE_JXR: StartSave(SaveFormat::HdrJxr); return 0;
        case IDC_REFRESH: RefreshWindowList(); return 0;
        case IDC_PATTERN: ShowTestPattern(); return 0;
        case IDC_PATTERN_HEADROOM: ShowHeadroomPattern(); return 0;
        case IDC_FIT: canvas_.Fit(); return 0;
        case IDC_ACTUAL: canvas_.ZoomToActualPixels(); return 0;
        case IDM_COPY: canvas_.CopySample(); return 0;
        case IDM_EXIT: DestroyWindow(hwnd); return 0;
        case IDM_ABOUT:
            // The version comes from the project definition, the same place the file's
            // version resource reads, so the two cannot disagree.
            MessageBoxW(hwnd,
                Widen(Format("HDRScope for Windows %s\n\n", HDRSCOPE_VERSION)
                      + "HDRウィンドウキャプチャ・表示・RGB測定ツール。\n"
                        "Windows.Graphics.Capture で scRGB float16 のまま取得し、\n"
                        "SDR基準白で正規化した拡張リニアsRGBで測定します。\n\n"
                        "MIT License").c_str(),
                L"HDRScopeについて", MB_OK | MB_ICONINFORMATION);
            return 0;
        case IDC_SHOW_ALL:
            showAll_ = Button_GetCheck(GetDlgItem(hwnd, IDC_SHOW_ALL)) == BST_CHECKED;
            RebuildTree();
            return 0;
        case IDC_FILTER:
            if (HIWORD(wp) == EN_CHANGE) {
                wchar_t text[256];
                GetWindowTextW(GetDlgItem(hwnd, IDC_FILTER), text, (int)std::size(text));
                filter_ = text;
                RebuildTree();
            }
            return 0;
        default:
            break;
        }
        break;
    }
    case WM_NOTIFY: {
        auto* header = (LPNMHDR)lp;
        if (header->idFrom == IDC_TREE && header->code == TVN_SELCHANGEDW) {
            auto* change = (LPNMTREEVIEWW)lp;
            selectedWindow_ = (HWND)change->itemNew.lParam;
            UpdateButtons();
        } else if (header->idFrom == IDC_TREE && header->code == NM_DBLCLK) {
            if (Selected()) StartCapture(false);
        }
        break;
    }
    case WM_JOB_DONE: {
        std::unique_ptr<Job> job((Job*)lp);
        SetBusy(false);
        if (!job->ok) {
            ReportError(job->error);
            return 0;
        }
        if (job->image) AdoptImage(job->image, job->maxRGB, job->label, job->status, job->preserveView);
        else SetStatus(job->status);
        return 0;
    }
    case WM_TIMER:
        if (wp == kDisplayTimer) canvas_.RefreshDisplay();
        return 0;
    case WM_SIZE:
        Layout();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = (MINMAXINFO*)lp;
        info->ptMinTrackSize.x = Scaled(980, dpi_);
        info->ptMinTrackSize.y = Scaled(640, dpi_);
        return 0;
    }
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wp);
        CreateFonts();
        EnumChildWindows(hwnd, [](HWND child, LPARAM font) {
            SendMessageW(child, WM_SETFONT, (WPARAM)font, TRUE);
            return TRUE;
        }, (LPARAM)uiFont_);
        SendMessageW(GetDlgItem(hwnd, IDC_TITLE), WM_SETFONT, (WPARAM)titleFont_, TRUE);
        SendMessageW(GetDlgItem(hwnd, IDC_SAMPLE), WM_SETFONT, (WPARAM)monoFont_, TRUE);
        RECT* suggested = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        return 0;
    }
    case WM_DROPFILES: {
        HDROP drop = (HDROP)wp;
        wchar_t path[MAX_PATH * 2];
        if (DragQueryFileW(drop, 0, path, (UINT)std::size(path))) StartLoad(path);
        DragFinish(drop);
        return 0;
    }
    case WM_SETCURSOR:
        if (busy_) {
            SetCursor(LoadCursorW(nullptr, IDC_WAIT));
            return TRUE;
        }
        break;
    case WM_DESTROY:
        KillTimer(hwnd, kDisplayTimer);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int MainWindow::Run(HINSTANCE instance, const std::wstring& startupFile, StartupPattern pattern) {
    instance_ = instance;
    g_main = this;

    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_TREEVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProcThunk;
    wc.hInstance = instance;
    wc.lpszClassName = L"HDRScopeMain";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    // Loaded at both sizes so the title bar and the Alt+Tab list each get the entry
    // drawn for them, rather than one scaled copy of the other.
    wc.hIcon = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(kAppIcon), IMAGE_ICON,
                                 GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(kAppIcon), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!RegisterClassExW(&wc)) {
        MessageBoxW(nullptr, L"ウィンドウクラスを登録できません。", L"HDRScope", MB_ICONERROR);
        return 1;
    }

    window_ = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"HDRScope",
                              WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                              1280, 860, nullptr, nullptr, instance, nullptr);
    if (!window_) {
        MessageBoxW(nullptr, L"ウィンドウを作成できません。", L"HDRScope", MB_ICONERROR);
        return 1;
    }
    dpi_ = (int)GetDpiForWindow(window_);
    CreateFonts();
    CreateControls();

    HMENU fileMenu = CreatePopupMenu();
    AppendMenuW(fileMenu, MF_STRING, IDM_OPEN, L"開く…\tCtrl+O");
    AppendMenuW(fileMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(fileMenu, MF_STRING, IDM_SAVE_TIFF, Widen(FormatInfo(SaveFormat::FloatTiff).menuTitle).c_str());
    AppendMenuW(fileMenu, MF_STRING, IDM_SAVE_JXR, Widen(FormatInfo(SaveFormat::HdrJxr).menuTitle).c_str());
    AppendMenuW(fileMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(fileMenu, MF_STRING, IDM_EXIT, L"終了");
    HMENU editMenu = CreatePopupMenu();
    AppendMenuW(editMenu, MF_STRING, IDM_COPY, L"測定値をコピー\tCtrl+C");
    HMENU helpMenu = CreatePopupMenu();
    AppendMenuW(helpMenu, MF_STRING, IDM_ABOUT, L"HDRScopeについて");
    HMENU menuBar = CreateMenu();
    AppendMenuW(menuBar, MF_POPUP, (UINT_PTR)fileMenu, L"ファイル(&F)");
    AppendMenuW(menuBar, MF_POPUP, (UINT_PTR)editMenu, L"編集(&E)");
    AppendMenuW(menuBar, MF_POPUP, (UINT_PTR)helpMenu, L"ヘルプ(&H)");
    SetMenu(window_, menuBar);

    std::string canvasError;
    if (!canvas_.Create(window_, instance, canvasError)) {
        MessageBoxW(window_, Widen(canvasError).c_str(), L"HDRScope", MB_ICONERROR);
        return 1;
    }
    canvas_.onSample = [this](const std::string& sample, const std::string& detail) {
        SetText(IDC_SAMPLE, sample);
        SetText(IDC_DETAIL, detail);
    };
    canvas_.onZoom = [this](const std::string& zoom) { SetText(IDC_ZOOM, zoom); };
    canvas_.onStatus = [this](const std::string& status) { SetStatus(status); };
    canvas_.onDisplayReadout = [this](const std::string& edr) { SetText(IDC_EDR, edr); };
    canvas_.onFileDropped = [this](const std::wstring& path) { StartLoad(path); };

    ShowWindow(window_, SW_SHOW);
    Layout();
    // Headroom and the SDR white level move without anything being drawn, so poll them
    // rather than leaving a stale number on screen while the user changes brightness.
    SetTimer(window_, kDisplayTimer, 1000, nullptr);

    if (!CaptureSupported())
        SetStatus("このWindowsではウィンドウキャプチャを利用できません。ファイルを開いての測定は可能です。");
    else
        RefreshWindowList();

    if (pattern == StartupPattern::Fixed) ShowTestPattern();
    else if (pattern == StartupPattern::Headroom) ShowHeadroomPattern();
    else if (!startupFile.empty()) StartLoad(startupFile);

    ACCEL accelerators[] = {
        {FVIRTKEY | FCONTROL, 'O', IDM_OPEN},
        {FVIRTKEY | FCONTROL, 'C', IDM_COPY},
        {FVIRTKEY | FCONTROL, 'S', IDM_SAVE_TIFF},
    };
    HACCEL accelerator = CreateAcceleratorTableW(accelerators, (int)std::size(accelerators));

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!TranslateAcceleratorW(window_, accelerator, &message)
            && !IsDialogMessageW(window_, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    g_main = nullptr;
    return (int)message.wParam;
}

}  // namespace

int RunApp(HINSTANCE instance, const std::wstring& startupFile, StartupPattern pattern) {
    MainWindow window;
    return window.Run(instance, startupFile, pattern);
}
