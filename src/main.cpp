//
// light_mp3 — Win32 native MP3 player with full ID3v2 metadata display.
// Compiler: MSVC (cl.exe)  SDK: Windows 10 22621  Audio: MCI  Art: GDI+
//
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <objidl.h>      // IStream
#include <gdiplus.h>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <sstream>
#include <algorithm>
#include <set>

#include "id3v2.h"
#include "player.h"

// ---------------------------------------------------------------------------
// Layout constants (client-area pixels)
// ---------------------------------------------------------------------------
static const int COVER_X  = 10,  COVER_Y  = 10,  COVER_W  = 200, COVER_H  = 200;
static const int META_X   = 220, META_Y   = 10,  META_W   = 510, META_H   = 200;
static const int FILE_X   = 10,  FILE_Y   = 218, FILE_W   = 720, FILE_H   = 18;
static const int CTRL_Y   = 244;
static const int SEEK_X   = 10,  SEEK_Y   = 286, SEEK_W   = 720, SEEK_H   = 26;
static const int CLIENT_W = 740, CLIENT_H = 326;

// ---------------------------------------------------------------------------
// Control IDs
// ---------------------------------------------------------------------------
enum {
    IDC_BTN_OPEN = 101,
    IDC_BTN_PREV,
    IDC_BTN_PLAY,
    IDC_BTN_STOP,
    IDC_BTN_NEXT,
    IDC_SEEK,
    IDC_META_LIST,
    IDC_TIME_LABEL,
    IDC_FILE_LABEL,
    TIMER_UPDATE = 1,
};

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static HINSTANCE g_hInst;
static HWND g_hwndMain;
static HWND g_hwndCover;
static HWND g_hwndMeta;
static HWND g_hwndPlay;
static HWND g_hwndSeek;
static HWND g_hwndTime;
static HWND g_hwndFile;

static Player                         g_player;
static ID3Tags                        g_tags;
static std::unique_ptr<Gdiplus::Bitmap> g_coverBitmap;
static ULONG_PTR                      g_gdiplusToken;
static bool                           g_seekDragging = false;
static std::wstring                   g_currentPath;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static std::wstring FormatTime(DWORD ms) {
    DWORD s = ms / 1000;
    wchar_t buf[32];
    swprintf_s(buf, L"%u:%02u", s / 60, s % 60);
    return buf;
}

static std::wstring Basename(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return (p == std::wstring::npos) ? path : path.substr(p + 1);
}

// Load GDI+ Bitmap from raw bytes (JPEG or PNG cover art).
static std::unique_ptr<Gdiplus::Bitmap> BitmapFromMemory(const std::vector<uint8_t>& data) {
    if (data.empty()) return nullptr;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, data.size());
    if (!hMem) return nullptr;
    void* p = GlobalLock(hMem);
    memcpy(p, data.data(), data.size());
    GlobalUnlock(hMem);
    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(hMem, TRUE /*fDeleteOnRelease*/, &stream)))
        return nullptr;
    auto bmp = std::make_unique<Gdiplus::Bitmap>(stream);
    stream->Release();
    if (bmp->GetLastStatus() != Gdiplus::Ok) return nullptr;
    return bmp;
}

// Build display rows: { label, value } from parsed tags.
struct TagRow { std::wstring label, value; };

static std::vector<TagRow> BuildTagRows(const ID3Tags& t) {
    std::vector<TagRow> rows;
    auto push = [&](const wchar_t* lbl, const std::wstring& v) {
        if (!v.empty()) rows.push_back({lbl, v});
    };
    push(L"Title",   t.title);
    push(L"Artist",  t.artist);
    push(L"Album",   t.album);
    push(L"Year",    t.year);
    push(L"Genre",   t.genre);
    push(L"Track",   t.trackNumber);
    push(L"BPM",     t.bpm);

    // Custom TXXX tags (e.g. MPM, Energy, Key …)
    for (auto& [k, v] : t.customTags)
        if (!v.empty()) rows.push_back({k, v});

    // Remaining standard text frames not already shown
    static const std::set<std::wstring> shown = {
        L"TIT2",L"TPE1",L"TALB",L"TDRC",L"TYER",
        L"TCON",L"TRCK",L"TBPM"
    };
    for (auto& [k, v] : t.allTextFrames) {
        if (k.size() >= 5 && k.substr(0, 5) == L"TXXX:") continue;
        if (shown.count(k)) continue;
        if (!v.empty()) rows.push_back({k, v});
    }
    return rows;
}

static void PopulateMetadata(const ID3Tags& tags) {
    ListView_DeleteAllItems(g_hwndMeta);
    auto rows = BuildTagRows(tags);
    int i = 0;
    for (auto& [lbl, val] : rows) {
        LVITEMW li = {};
        li.mask    = LVIF_TEXT;
        li.iItem   = i;
        li.pszText = const_cast<wchar_t*>(lbl.c_str());
        ListView_InsertItem(g_hwndMeta, &li);
        ListView_SetItemText(g_hwndMeta, i, 1, const_cast<wchar_t*>(val.c_str()));
        ++i;
    }
}

static void UpdateTimeLabel() {
    std::wstring t = g_player.IsOpen()
        ? FormatTime(g_player.GetPosition()) + L" / " + FormatTime(g_player.GetLength())
        : L"--:-- / --:--";
    SetWindowTextW(g_hwndTime, t.c_str());
}

static void UpdateSeekBar() {
    if (g_seekDragging || !g_player.IsOpen()) return;
    DWORD len = g_player.GetLength();
    DWORD pos = g_player.GetPosition();
    int   val = (len > 0) ? (int)((LONGLONG)pos * 1000 / len) : 0;
    SendMessage(g_hwndSeek, TBM_SETPOS, TRUE, val);
}

static void SetPlayButtonText() {
    SetWindowTextW(g_hwndPlay, g_player.IsPlaying() ? L"Pause" : L"Play");
}

// ---------------------------------------------------------------------------
// Cover art child window
// ---------------------------------------------------------------------------
static void PaintCoverArt(HWND hwnd, HDC hdc) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom;

    Gdiplus::Graphics gfx(hdc);
    gfx.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);

    if (g_coverBitmap) {
        gfx.DrawImage(g_coverBitmap.get(), 0, 0, w, h);
    } else {
        // Placeholder: dark grey background + "No Cover" text
        gfx.FillRectangle(
            &Gdiplus::SolidBrush(Gdiplus::Color(255, 45, 45, 45)),
            Gdiplus::Rect(0, 0, w, h));
        Gdiplus::Font font(L"Segoe UI", 11.0f);
        Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, 160, 160, 160));
        Gdiplus::StringFormat fmt;
        fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
        fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        gfx.DrawString(L"No Cover", -1, &font,
                       Gdiplus::RectF(0, 0, (float)w, (float)h),
                       &fmt, &textBrush);
    }
}

static LRESULT CALLBACK CoverArtProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        PaintCoverArt(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Open a file: parse tags, load cover, populate UI, start playback
// ---------------------------------------------------------------------------
static void OpenFile(const std::wstring& path) {
    g_player.Stop();
    g_player.Close();
    g_coverBitmap.reset();
    g_currentPath = path;

    g_tags = ParseID3v2(path);

    if (g_tags.hasCoverArt)
        g_coverBitmap = BitmapFromMemory(g_tags.coverArt.data);

    InvalidateRect(g_hwndCover, nullptr, FALSE);
    PopulateMetadata(g_tags);
    SetWindowTextW(g_hwndFile, Basename(path).c_str());

    SendMessage(g_hwndSeek, TBM_SETPOS, TRUE, 0);

    if (g_player.Open(path, g_hwndMain)) {
        g_player.Play();
        SetTimer(g_hwndMain, TIMER_UPDATE, 500, nullptr);
    }
    SetPlayButtonText();
    UpdateTimeLabel();
}

// ---------------------------------------------------------------------------
// Main window procedure
// ---------------------------------------------------------------------------
static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    // -----------------------------------------------------------------------
    case WM_CREATE: {
        // Cover art child window
        g_hwndCover = CreateWindowExW(WS_EX_STATICEDGE, L"CoverArtClass", L"",
            WS_CHILD | WS_VISIBLE,
            COVER_X, COVER_Y, COVER_W, COVER_H,
            hwnd, nullptr, g_hInst, nullptr);

        // Metadata ListView (two columns: Tag | Value)
        g_hwndMeta = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL,
            META_X, META_Y, META_W, META_H,
            hwnd, (HMENU)IDC_META_LIST, g_hInst, nullptr);
        ListView_SetExtendedListViewStyle(g_hwndMeta,
            LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        {
            LVCOLUMNW col = {};
            col.mask    = LVCF_TEXT | LVCF_WIDTH;
            col.cx      = 140;
            col.pszText = const_cast<wchar_t*>(L"Tag");
            ListView_InsertColumn(g_hwndMeta, 0, &col);
            col.cx      = 355;
            col.pszText = const_cast<wchar_t*>(L"Value");
            ListView_InsertColumn(g_hwndMeta, 1, &col);
        }

        // File label (basename below cover art)
        g_hwndFile = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_ENDELLIPSIS | SS_NOPREFIX,
            FILE_X, FILE_Y, FILE_W, FILE_H,
            hwnd, (HMENU)IDC_FILE_LABEL, g_hInst, nullptr);

        // Control buttons
        auto mkBtn = [&](const wchar_t* t, int id, int x, int w) {
            return CreateWindowExW(0, L"BUTTON", t,
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                x, CTRL_Y, w, 28, hwnd, (HMENU)(UINT_PTR)id, g_hInst, nullptr);
        };
        mkBtn(L"Open",   IDC_BTN_OPEN, 10,  65);
        mkBtn(L"|<<",    IDC_BTN_PREV, 220, 38);
        g_hwndPlay = mkBtn(L"Play", IDC_BTN_PLAY, 263, 65);
        mkBtn(L"Stop",   IDC_BTN_STOP, 333, 55);
        mkBtn(L">>|",    IDC_BTN_NEXT, 393, 38);

        // Time label
        g_hwndTime = CreateWindowExW(0, L"STATIC", L"--:-- / --:--",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            445, CTRL_Y, 150, 28, hwnd, (HMENU)IDC_TIME_LABEL, g_hInst, nullptr);

        // Seek trackbar
        g_hwndSeek = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS | TBS_TOOLTIPS,
            SEEK_X, SEEK_Y, SEEK_W, SEEK_H,
            hwnd, (HMENU)IDC_SEEK, g_hInst, nullptr);
        SendMessage(g_hwndSeek, TBM_SETRANGE, TRUE, MAKELPARAM(0, 1000));

        // Accept drag-and-drop
        DragAcceptFiles(hwnd, TRUE);
        return 0;
    }

    // -----------------------------------------------------------------------
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_BTN_OPEN: {
            wchar_t buf[MAX_PATH] = {};
            OPENFILENAMEW ofn = {};
            ofn.lStructSize  = sizeof(ofn);
            ofn.hwndOwner    = hwnd;
            ofn.lpstrFilter  = L"MP3 files\0*.mp3\0All files\0*.*\0";
            ofn.lpstrFile    = buf;
            ofn.nMaxFile     = MAX_PATH;
            ofn.Flags        = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) OpenFile(buf);
            break;
        }
        case IDC_BTN_PLAY:
            if (g_player.IsOpen()) {
                g_player.Pause();
                SetPlayButtonText();
            } else if (!g_currentPath.empty()) {
                OpenFile(g_currentPath);
            }
            break;
        case IDC_BTN_STOP:
            g_player.Stop();
            SendMessage(g_hwndSeek, TBM_SETPOS, TRUE, 0);
            SetPlayButtonText();
            UpdateTimeLabel();
            break;
        case IDC_BTN_PREV:
        case IDC_BTN_NEXT:
            // Placeholder: no playlist yet — restart current track
            if (g_player.IsOpen()) {
                g_player.SeekTo(0);
                if (!g_player.IsPlaying()) { g_player.Play(); SetPlayButtonText(); }
            }
            break;
        }
        return 0;

    // -----------------------------------------------------------------------
    case WM_HSCROLL: {
        if ((HWND)lParam != g_hwndSeek) break;
        WORD req = LOWORD(wParam);
        if (req == TB_THUMBTRACK) {
            g_seekDragging = true;
        } else if (req == TB_ENDTRACK || req == TB_THUMBPOSITION) {
            g_seekDragging = false;
            DWORD len = g_player.GetLength();
            if (len > 0) {
                DWORD pos = (DWORD)SendMessage(g_hwndSeek, TBM_GETPOS, 0, 0);
                g_player.SeekTo((DWORD)((LONGLONG)pos * len / 1000));
            }
        }
        UpdateTimeLabel();
        return 0;
    }

    // -----------------------------------------------------------------------
    case WM_TIMER:
        if (wParam == TIMER_UPDATE && g_player.IsOpen()) {
            UpdateSeekBar();
            UpdateTimeLabel();
        }
        return 0;

    // -----------------------------------------------------------------------
    // End-of-track notification from player (waveOut finished all PCM data)
    case WM_PLAYER_DONE:
        g_player.Stop();
        SendMessage(g_hwndSeek, TBM_SETPOS, TRUE, 0);
        SetPlayButtonText();
        UpdateTimeLabel();
        return 0;

    // -----------------------------------------------------------------------
    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wParam;
        wchar_t path[MAX_PATH] = {};
        if (DragQueryFileW(hDrop, 0, path, MAX_PATH))
            OpenFile(path);
        DragFinish(hDrop);
        return 0;
    }

    // -----------------------------------------------------------------------
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_UPDATE);
        g_player.Close();
        g_coverBitmap.reset();
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// WinMain
// ---------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow) {
    g_hInst = hInst;

    // GDI+ startup
    Gdiplus::GdiplusStartupInput gdipInput;
    Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdipInput, nullptr);

    // Common controls (ListView, Trackbar)
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    // Register cover art child window class
    {
        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = CoverArtProc;
        wc.hInstance     = hInst;
        wc.lpszClassName = L"CoverArtClass";
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        RegisterClassExW(&wc);
    }

    // Register main window class
    {
        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = MainWndProc;
        wc.hInstance     = hInst;
        wc.lpszClassName = L"LightMP3";
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(100));
        RegisterClassExW(&wc);
    }

    // Calculate window size to get the desired client area
    RECT rc = { 0, 0, CLIENT_W, CLIENT_H };
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);

    g_hwndMain = CreateWindowExW(
        WS_EX_ACCEPTFILES,
        L"LightMP3",
        L"Light MP3 Player v1.0.3",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInst, nullptr);

    if (!g_hwndMain) return 1;

    ShowWindow(g_hwndMain, nCmdShow);
    UpdateWindow(g_hwndMain);

    // If launched with a file argument, open it immediately
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc >= 2) OpenFile(argv[1]);
    LocalFree(argv);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
