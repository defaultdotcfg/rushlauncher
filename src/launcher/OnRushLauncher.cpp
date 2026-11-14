// ============================================================================
// ONRUSH (Xbox One ERA) - PC Native Launcher & Monitor
//
// Title ID: 071190C6 | Version: 1.0.64980.34 | Arch: AMD64 (x64)
//
// Modern native Win32 Graphical User Interface (GUI) with dark theme,
// configuration editor (onrush.ini), hardware telemetry, prerequisite auditing,
// and real-time multi-threaded engine supervision with integrated terminal log.
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <intrin.h>
#include <vector>
#include <string>
#include <algorithm>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

// Window IDs
#define ID_RES_COMBO        1001
#define ID_MODE_COMBO       1002
#define ID_VSYNC_CHK        1003
#define ID_D3DDEBUG_CHK     1004
#define ID_VERBOSE_CHK      1005
#define ID_SAVE_EDIT        1006
#define ID_SAVE_BROWSE      1007
#define ID_BTN_LAUNCH       1008
#define ID_BTN_KILL         1009
#define ID_BTN_OPEN_GAME    1010
#define ID_BTN_OPEN_SAVE    1011
#define ID_BTN_CLEAR_LOG    1012
#define ID_BTN_COPY_LOG     1013
#define ID_LOG_EDIT         1014
#define ID_GAME_PATH_EDIT   1015
#define ID_GAME_PATH_BROWSE 1016

#define WM_APPEND_LOG       (WM_USER + 101)
#define WM_GAME_STARTED     (WM_USER + 102)
#define WM_GAME_EXITED      (WM_USER + 103)

// Launcher colours: a graphite dark theme (default) and a light one ("Tema" in the settings, onrush.ini [Launcher] Theme).
struct Palette
{
    bool dark;
    COLORREF bgMain, bgCard, bgInput, bgLog, border, accentOrange, accentGreen, accentRed, textPrimary, textMuted, logText;
    COLORREF heroDim;      // full-cover background of the tabs that hide the art (Fotos, Controles)
    COLORREF homeCard, navActive, toggleOff, knob, btnFill, btnPressed, launchPressed, closePressed, disabledFill;
    COLORREF comboSel, accentText, onAccent, track;
};
static const Palette kPaletteDark = {
    true,
    RGB(13, 14, 17), RGB(22, 23, 28), RGB(30, 32, 38), RGB(11, 12, 14), RGB(46, 48, 56),
    RGB(240, 128, 52), RGB(120, 200, 140), RGB(232, 84, 72), RGB(238, 239, 243), RGB(146, 151, 163), RGB(150, 210, 160),
    RGB(18, 19, 23),
    RGB(28, 30, 36), RGB(34, 36, 44), RGB(62, 65, 76), RGB(250, 250, 252), RGB(34, 36, 43), RGB(46, 49, 58), RGB(206, 104, 36),
    RGB(196, 64, 54), RGB(44, 46, 54),
    RGB(64, 44, 30), RGB(255, 160, 100), RGB(255, 255, 255), RGB(48, 51, 60),
};
static const Palette kPaletteLight = {
    false,
    RGB(240, 241, 245), RGB(255, 255, 255), RGB(240, 242, 246), RGB(250, 250, 252), RGB(216, 219, 227),
    RGB(226, 110, 34), RGB(40, 140, 80), RGB(206, 58, 46), RGB(24, 26, 32), RGB(104, 110, 124), RGB(32, 116, 64),
    RGB(247, 248, 250),
    RGB(250, 251, 253), RGB(229, 232, 239), RGB(196, 200, 210), RGB(255, 255, 255), RGB(242, 243, 247), RGB(226, 229, 236), RGB(196, 90, 24),
    RGB(184, 50, 40), RGB(230, 232, 237),
    RGB(253, 232, 214), RGB(190, 84, 20), RGB(255, 255, 255), RGB(226, 229, 235),
};
static Palette g_pal = kPaletteDark;
#define COLOR_BG_MAIN       g_pal.bgMain
#define COLOR_BG_CARD       g_pal.bgCard
#define COLOR_BG_INPUT      g_pal.bgInput
#define COLOR_BG_LOG        g_pal.bgLog
#define COLOR_BORDER        g_pal.border
#define COLOR_ACCENT_ORANGE g_pal.accentOrange
#define COLOR_ACCENT_GREEN  g_pal.accentGreen
#define COLOR_ACCENT_RED    g_pal.accentRed
#define COLOR_TEXT_PRIMARY  g_pal.textPrimary
#define COLOR_TEXT_MUTED    g_pal.textMuted
#define COLOR_LOG_TEXT      g_pal.logText
// GDI+ colour from a palette entry
#define GPC(c)        Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c))
#define GPCA(a, c)    Gdiplus::Color((BYTE)(a), GetRValue(c), GetGValue(c), GetBValue(c))

// Configuration structure
struct LauncherConfig
{
    int width;
    int height;
    wchar_t windowMode[32]; // Windowed, Borderless, Fullscreen
    int vsync;
    int gfxPreset;          // in-game "Graphics quality": 0 console, 1..5 PC ultra low .. ultra
    int fpsLimit;           // in-game "Frame rate limit" (fps, 0 = unlimited)
    wchar_t gamePath[MAX_PATH];
    wchar_t savePath[MAX_PATH];
    int diagnostics;
    int verboseLog;
    int d3d12Debug;
    int autoRestart;
    int skipIntro;       // skip the legal-screen minimum time and the Codemasters/intro video (boots to the title)
    wchar_t uiLang[8];     // launcher language: pt, en, es
    wchar_t textLang[16];  // game text language (locale, e.g. pt-BR); empty = system
    wchar_t voiceLang[16]; // game voice language (locale); empty = same as text
    wchar_t playerName[32]; // in-game name (gamertag); also the save sub-folder
    wchar_t photoFormat[8]; // photo mode export (F12): png or jpg
    int musicEnabled;       // play the player's own playlist in game instead of the game music
    int musicShuffle;
    int musicVolume;        // 0..100
    wchar_t musicPlaylist[64]; // playlist name (file <launcher dir>\playlists\<name>.m3u8)
    int minimizeOnPlay;     // minimize the launcher when the game starts (restored when it closes)
    wchar_t theme[8];       // launcher colours: dark or light
};

static LauncherConfig g_config;
static wchar_t g_exeDir[MAX_PATH] = {};
static HWND g_hMainWnd = nullptr;
static HWND g_hLogEdit = nullptr;
static HWND g_hBtnLaunch = nullptr;
static HWND g_hBtnKill = nullptr;
static HWND g_hResCombo = nullptr;
static HWND g_hModeCombo = nullptr;
static HWND g_hVsyncChk = nullptr;
static HWND g_hD3DDebugChk = nullptr;
static HWND g_hVerboseChk = nullptr;
static HWND g_hGamePathEdit = nullptr;
static HWND g_hSaveEdit = nullptr;

static HBRUSH g_hBrushMain = nullptr;
static HBRUSH g_hBrushCard = nullptr;
static HBRUSH g_hBrushInput = nullptr;
static HBRUSH g_hBrushLog = nullptr;

static HFONT g_hFontTitle = nullptr;
static HFONT g_hFontSubtitle = nullptr;
static HFONT g_hFontUI = nullptr;
static HFONT g_hFontUIBold = nullptr;
static HFONT g_hFontLog = nullptr;

static HANDLE g_hGameProcess = nullptr;
static DWORD g_gamePid = 0;
static bool g_gameRunning = false;
static bool g_debugMode = false;      // launcher debug option: full logs + frame captures (set per game launch)
static int g_firstChanceLogged = 0;   // first-chance exceptions logged in this game session
static HANDLE g_hWorkerThread = nullptr;

// ============================================================================
// INI Config Management
// ============================================================================

static void SetDefaultConfig(LauncherConfig *cfg)
{
    cfg->width = 1920;
    cfg->height = 1080;
    wcscpy_s(cfg->windowMode, 32, L"Borderless");
    cfg->vsync = 1;
    cfg->gfxPreset = 0;
    cfg->fpsLimit = 60;
    cfg->gamePath[0] = L'\0';
    wcscpy_s(cfg->savePath, MAX_PATH, L".\\save");
    cfg->diagnostics = 1;
    cfg->verboseLog = 0;  // debug mode: full logs + frame captures (ini key DebugMode)
    cfg->d3d12Debug = 0;
    cfg->autoRestart = 0;
    cfg->skipIntro = 0;
    wcscpy_s(cfg->uiLang, 8, L"pt");
    cfg->textLang[0] = L'\0';
    cfg->voiceLang[0] = L'\0';
    wcscpy_s(cfg->playerName, 32, L"Player 1");
    wcscpy_s(cfg->photoFormat, 8, L"png");
    cfg->musicEnabled = 0;
    cfg->musicShuffle = 0;
    cfg->musicVolume = 70;
    cfg->musicPlaylist[0] = L'\0';
    cfg->minimizeOnPlay = 0;
    wcscpy_s(cfg->theme, 8, L"dark");
}

static void LoadConfigFile(const wchar_t *iniPath, LauncherConfig *cfg)
{
    DWORD attr = GetFileAttributesW(iniPath);
    if (attr == INVALID_FILE_ATTRIBUTES)
    {
        SetDefaultConfig(cfg);
        return;
    }

    cfg->width = GetPrivateProfileIntW(L"Display", L"Width", cfg->width, iniPath);
    cfg->height = GetPrivateProfileIntW(L"Display", L"Height", cfg->height, iniPath);
    GetPrivateProfileStringW(L"Display", L"WindowMode", cfg->windowMode, cfg->windowMode, 32, iniPath);
    cfg->vsync = GetPrivateProfileIntW(L"Display", L"VSync", cfg->vsync, iniPath);
    cfg->gfxPreset = GetPrivateProfileIntW(L"Graphics", L"Preset", cfg->gfxPreset, iniPath);
    cfg->fpsLimit = GetPrivateProfileIntW(L"Graphics", L"FpsLimit", cfg->fpsLimit, iniPath);
    GetPrivateProfileStringW(L"System", L"GamePath", cfg->gamePath, cfg->gamePath, MAX_PATH, iniPath);
    GetPrivateProfileStringW(L"System", L"SavePath", cfg->savePath, cfg->savePath, MAX_PATH, iniPath);
    cfg->diagnostics = GetPrivateProfileIntW(L"System", L"Diagnostics", cfg->diagnostics, iniPath);
    cfg->verboseLog = GetPrivateProfileIntW(L"System", L"DebugMode", cfg->verboseLog, iniPath);
    cfg->d3d12Debug = GetPrivateProfileIntW(L"Graphics", L"D3D12Debug", cfg->d3d12Debug, iniPath);
    cfg->skipIntro = GetPrivateProfileIntW(L"System", L"SkipIntro", cfg->skipIntro, iniPath);
    cfg->autoRestart = GetPrivateProfileIntW(L"System", L"AutoRestart", cfg->autoRestart, iniPath);
    GetPrivateProfileStringW(L"Language", L"Launcher", cfg->uiLang, cfg->uiLang, 8, iniPath);
    GetPrivateProfileStringW(L"Language", L"Text", cfg->textLang, cfg->textLang, 16, iniPath);
    GetPrivateProfileStringW(L"Language", L"Voice", cfg->voiceLang, cfg->voiceLang, 16, iniPath);
    GetPrivateProfileStringW(L"Player", L"Name", cfg->playerName, cfg->playerName, 32, iniPath);
    if (!cfg->playerName[0]) wcscpy_s(cfg->playerName, 32, L"Player 1");
    GetPrivateProfileStringW(L"Photo", L"Format", cfg->photoFormat, cfg->photoFormat, 8, iniPath);
    if (_wcsicmp(cfg->photoFormat, L"jpg") != 0) wcscpy_s(cfg->photoFormat, 8, L"png");
    cfg->musicEnabled = GetPrivateProfileIntW(L"Music", L"Enabled", cfg->musicEnabled, iniPath);
    cfg->musicShuffle = GetPrivateProfileIntW(L"Music", L"Shuffle", cfg->musicShuffle, iniPath);
    cfg->musicVolume = GetPrivateProfileIntW(L"Music", L"Volume", cfg->musicVolume, iniPath);
    if (cfg->musicVolume < 0 || cfg->musicVolume > 100) cfg->musicVolume = 70;
    GetPrivateProfileStringW(L"Music", L"Playlist", cfg->musicPlaylist, cfg->musicPlaylist, 64, iniPath);
    cfg->minimizeOnPlay = GetPrivateProfileIntW(L"Launcher", L"MinimizeOnPlay", cfg->minimizeOnPlay, iniPath);
    GetPrivateProfileStringW(L"Launcher", L"Theme", cfg->theme, cfg->theme, 8, iniPath);
    if (_wcsicmp(cfg->theme, L"light") != 0) wcscpy_s(cfg->theme, 8, L"dark");
}

static void SaveConfigFile(const wchar_t *iniPath, const LauncherConfig *cfg)
{
    wchar_t buf[64];
    swprintf_s(buf, 64, L"%d", cfg->width);
    WritePrivateProfileStringW(L"Display", L"Width", buf, iniPath);

    swprintf_s(buf, 64, L"%d", cfg->height);
    WritePrivateProfileStringW(L"Display", L"Height", buf, iniPath);

    WritePrivateProfileStringW(L"Display", L"WindowMode", cfg->windowMode, iniPath);

    swprintf_s(buf, 64, L"%d", cfg->vsync);
    WritePrivateProfileStringW(L"Display", L"VSync", buf, iniPath);
    swprintf_s(buf, 64, L"%d", cfg->gfxPreset);
    WritePrivateProfileStringW(L"Graphics", L"Preset", buf, iniPath);
    swprintf_s(buf, 64, L"%d", cfg->fpsLimit);
    WritePrivateProfileStringW(L"Graphics", L"FpsLimit", buf, iniPath);

    WritePrivateProfileStringW(L"System", L"GamePath", cfg->gamePath, iniPath);
    WritePrivateProfileStringW(L"System", L"SavePath", cfg->savePath, iniPath);

    swprintf_s(buf, 64, L"%d", cfg->diagnostics);
    WritePrivateProfileStringW(L"System", L"Diagnostics", buf, iniPath);

    swprintf_s(buf, 64, L"%d", cfg->verboseLog);
    WritePrivateProfileStringW(L"System", L"DebugMode", buf, iniPath);
    WritePrivateProfileStringW(L"System", L"FrameDumps", nullptr, iniPath);  // v0.1.1 key
    WritePrivateProfileStringW(L"System", L"VerboseLog", nullptr, iniPath);  // old key (was always 1, unused)

    swprintf_s(buf, 64, L"%d", cfg->d3d12Debug);
    WritePrivateProfileStringW(L"Graphics", L"D3D12Debug", buf, iniPath);
    WritePrivateProfileStringW(L"System", L"QuickGameplay", nullptr, iniPath);  // option removed: drop the old key
    swprintf_s(buf, 64, L"%d", cfg->skipIntro);
    WritePrivateProfileStringW(L"System", L"SkipIntro", buf, iniPath);
    WritePrivateProfileStringW(L"Language", L"Launcher", cfg->uiLang, iniPath);
    WritePrivateProfileStringW(L"Language", L"Text", cfg->textLang, iniPath);
    WritePrivateProfileStringW(L"Language", L"Voice", cfg->voiceLang, iniPath);
    WritePrivateProfileStringW(L"Player", L"Name", cfg->playerName, iniPath);
    WritePrivateProfileStringW(L"Photo", L"Format", cfg->photoFormat, iniPath);
    swprintf_s(buf, 64, L"%d", cfg->musicEnabled);
    WritePrivateProfileStringW(L"Music", L"Enabled", buf, iniPath);
    swprintf_s(buf, 64, L"%d", cfg->musicShuffle);
    WritePrivateProfileStringW(L"Music", L"Shuffle", buf, iniPath);
    swprintf_s(buf, 64, L"%d", cfg->musicVolume);
    WritePrivateProfileStringW(L"Music", L"Volume", buf, iniPath);
    WritePrivateProfileStringW(L"Music", L"Playlist", cfg->musicPlaylist, iniPath);
    swprintf_s(buf, 64, L"%d", cfg->minimizeOnPlay);
    WritePrivateProfileStringW(L"Launcher", L"MinimizeOnPlay", buf, iniPath);
    WritePrivateProfileStringW(L"Launcher", L"Theme", cfg->theme, iniPath);
}

static void GetEffectiveGameDir(const LauncherConfig *cfg, wchar_t *outPath, size_t maxLen)
{
    if (cfg && cfg->gamePath[0] != L'\0')
    {
        if (PathIsRelativeW(cfg->gamePath))
        {
            wchar_t combined[MAX_PATH];
            swprintf_s(combined, MAX_PATH, L"%ls\\%ls", g_exeDir, cfg->gamePath);
            wchar_t normalized[MAX_PATH];
            if (GetFullPathNameW(combined, MAX_PATH, normalized, nullptr) > 0)
            {
                wcscpy_s(outPath, maxLen, normalized);
                return;
            }
        }
        else
        {
            wcscpy_s(outPath, maxLen, cfg->gamePath);
            return;
        }
    }

    // 1. Check local directory where OnRushLauncher is located
    wchar_t localExe[MAX_PATH];
    swprintf_s(localExe, MAX_PATH, L"%ls\\game_release_xbox.exe", g_exeDir);
    if (GetFileAttributesW(localExe) != INVALID_FILE_ATTRIBUTES)
    {
        wcscpy_s(outPath, maxLen, g_exeDir);
        return;
    }

    // 2. Check candidate directories
    const wchar_t *candidates[] = {
        L"..\\onrush\\stage\\Mount",
        L"..\\..\\onrush\\stage\\Mount",
        L"stage\\Mount",
        L"..\\stage\\Mount",
        L"..\\..\\analysis\\onrush\\stage\\Mount",
        L"..\\onrush"
    };

    for (const wchar_t *cand : candidates)
    {
        wchar_t candidateFull[MAX_PATH];
        swprintf_s(candidateFull, MAX_PATH, L"%ls\\%ls", g_exeDir, cand);
        wchar_t normalized[MAX_PATH];
        if (GetFullPathNameW(candidateFull, MAX_PATH, normalized, nullptr) > 0)
        {
            wchar_t testExe[MAX_PATH];
            swprintf_s(testExe, MAX_PATH, L"%ls\\game_release_xbox.exe", normalized);
            if (GetFileAttributesW(testExe) != INVALID_FILE_ATTRIBUTES)
            {
                wcscpy_s(outPath, maxLen, normalized);
                return;
            }
        }
    }

    // Fallback to g_exeDir
    wcscpy_s(outPath, maxLen, g_exeDir);
}

// ============================================================================
// UI Log Appender
// ============================================================================

static void AppendLogText(const wchar_t *text)
{
    if (!g_hLogEdit || !IsWindow(g_hLogEdit))
        return;

    int len = GetWindowTextLengthW(g_hLogEdit);
    SendMessageW(g_hLogEdit, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(g_hLogEdit, EM_REPLACESEL, FALSE, (LPARAM)text);
    SendMessageW(g_hLogEdit, EM_SCROLLCARET, 0, 0);
}

static void LogPrintf(const wchar_t *fmt, ...)
{
    wchar_t buf[2048];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, 2048, _TRUNCATE, fmt, args);
    va_end(args);

    if (g_hMainWnd && IsWindow(g_hMainWnd))
    {
        wchar_t *heapMsg = _wcsdup(buf);
        PostMessageW(g_hMainWnd, WM_APPEND_LOG, 0, (LPARAM)heapMsg);
    }

    if (g_exeDir[0] != L'\0')
    {
        wchar_t logPath[MAX_PATH];
        swprintf_s(logPath, MAX_PATH, L"%ls\\onrush_launcher.log", g_exeDir);
        // one log per launcher run; the previous one is kept as onrush_launcher.prev.log
        static volatile LONG s_rotated = 0;
        if (!s_rotated && InterlockedExchange(&s_rotated, 1) == 0)
        {
            wchar_t prevPath[MAX_PATH];
            swprintf_s(prevPath, MAX_PATH, L"%ls\\onrush_launcher.prev.log", g_exeDir);
            MoveFileExW(logPath, prevPath, MOVEFILE_REPLACE_EXISTING);
        }
        HANDLE hFile = CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE)
        {
            int utf8Len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
            if (utf8Len > 1)
            {
                std::vector<char> utf8Buf(utf8Len);
                WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8Buf.data(), utf8Len, nullptr, nullptr);
                DWORD written = 0;
                WriteFile(hFile, utf8Buf.data(), utf8Len - 1, &written, nullptr);
            }
            FlushFileBuffers(hFile);
            CloseHandle(hFile);
        }
    }
}

// ============================================================================
// System Telemetry Helper
// ============================================================================

struct SysSpecs
{
    DWORD cores;
    DWORDLONG ramTotalMB;
    DWORDLONG ramAvailMB;
    bool hasSSE42;
    bool hasAVX;
    bool hasAVX2;
};

static SysSpecs GetHostSpecs()
{
    SysSpecs specs = {};
    SYSTEM_INFO sysInfo = {};
    GetSystemInfo(&sysInfo);
    specs.cores = sysInfo.dwNumberOfProcessors;

    MEMORYSTATUSEX mem = { sizeof(mem) };
    GlobalMemoryStatusEx(&mem);
    specs.ramTotalMB = mem.ullTotalPhys / (1024 * 1024);
    specs.ramAvailMB = mem.ullAvailPhys / (1024 * 1024);

    int cpuInfo[4] = {};
    __cpuid(cpuInfo, 1);
    specs.hasSSE42 = (cpuInfo[2] & (1 << 20)) != 0;
    specs.hasAVX   = (cpuInfo[2] & (1 << 28)) != 0;

    __cpuid(cpuInfo, 7);
    specs.hasAVX2  = (cpuInfo[1] & (1 << 5)) != 0;
    return specs;
}

// ============================================================================
// Game Supervisor Worker Thread
// ============================================================================

struct ModuleRecord
{
    ULONG_PTR base;
    ULONG_PTR size;
    wchar_t name[MAX_PATH];
    wchar_t path[MAX_PATH];
};

// The playlist feature is parked until it is better integrated in the game: no tab, no drag-and-drop and
// nothing is passed to the game. Set to 0 to bring it back (all the code is still in playlist_ui.inc).
#define PLAYLIST_HIDDEN   1
static bool ProfUnlocked(const std::wstring &name);  // profiles_ui.inc
static bool ProfAllUnlocked(const std::wstring &name);
static void KbSetGameEnv();                          // controls_ui.inc
static DWORD WINAPI GameSupervisorThread(LPVOID lpParam)
{
    LauncherConfig *cfg = (LauncherConfig *)lpParam;

    wchar_t gameDir[MAX_PATH] = {};
    GetEffectiveGameDir(cfg, gameDir, MAX_PATH);

    wchar_t iniPath[MAX_PATH];
    swprintf_s(iniPath, MAX_PATH, L"%ls\\onrush.ini", g_exeDir);
    SaveConfigFile(iniPath, cfg);

    if (_wcsicmp(gameDir, g_exeDir) != 0)
    {
        wchar_t gameIniPath[MAX_PATH];
        swprintf_s(gameIniPath, MAX_PATH, L"%ls\\onrush.ini", gameDir);
        SaveConfigFile(gameIniPath, cfg);
    }

    wchar_t envBuf[64];
    swprintf_s(envBuf, 64, L"%d", cfg->width);
    SetEnvironmentVariableW(L"ONRUSH_RES_WIDTH", envBuf);

    swprintf_s(envBuf, 64, L"%d", cfg->height);
    SetEnvironmentVariableW(L"ONRUSH_RES_HEIGHT", envBuf);

    SetEnvironmentVariableW(L"ONRUSH_WINDOW_MODE", cfg->windowMode);

    swprintf_s(envBuf, 64, L"%d", cfg->vsync);
    SetEnvironmentVariableW(L"ONRUSH_VSYNC", envBuf);
    // In-game GRAPHICS rows (bridge gfx_options.inc): start values, and the ini the game writes changes to.
    swprintf_s(envBuf, 64, L"%d", cfg->gfxPreset);
    SetEnvironmentVariableW(L"ONRUSH_GFX_PRESET", envBuf);
    swprintf_s(envBuf, 64, L"%d", cfg->fpsLimit);
    SetEnvironmentVariableW(L"ONRUSH_FPS_LIMIT", envBuf);
    SetEnvironmentVariableW(L"ONRUSH_INI", iniPath);

    swprintf_s(envBuf, 64, L"%d", cfg->d3d12Debug);
    SetEnvironmentVariableW(L"ONRUSH_D3D12_DEBUG", envBuf);
    // debug mode: full bridge log + periodic frame_*.bmp captures in the game folder
    g_debugMode = cfg->verboseLog != 0;
    g_firstChanceLogged = 0;
    SetEnvironmentVariableW(L"ONRUSH_FRAME_DUMPS", g_debugMode ? L"1" : nullptr);
    SetEnvironmentVariableW(L"ONRUSH_VERBOSE_LOG", g_debugMode ? L"1" : nullptr);

    // Quick test mode ("OnRushLauncher.exe -gameplay", developer tests only): the bridge skips the boot screens.
    {
        const wchar_t *cl = GetCommandLineW();
        bool qg = cl && wcsstr(cl, L"-gameplay");
        SetEnvironmentVariableW(L"ONRUSH_GAMEPLAY", qg ? L"1" : nullptr);
        SetEnvironmentVariableW(L"ONRUSH_SKIP_INTRO", cfg->skipIntro ? L"1" : nullptr);
    }
    // Game languages: text via kernelx GetUserDefaultLocaleName, voice via the bridge's audio-table patch.
    SetEnvironmentVariableW(L"ONRUSH_TEXT_LANG", cfg->textLang[0] ? cfg->textLang : nullptr);
    SetEnvironmentVariableW(L"ONRUSH_VOICE_LANG", cfg->voiceLang[0] ? cfg->voiceLang : nullptr);
    // Player name (gamertag in game) and where Connected Storage saves go: <save folder>\<name>\...
    SetEnvironmentVariableW(L"ONRUSH_PLAYER_NAME", cfg->playerName[0] ? cfg->playerName : nullptr);
    // Photo export (F12): saved to <game folder>\\Fotos at the output resolution, without the HUD.
    // Keyboard as a controller ("Controles" tab): onrush.ini [Keyboard] -> ONRUSH_KEYMAP for the bridge.
    KbSetGameEnv();
    SetEnvironmentVariableW(L"ONRUSH_PHOTO_FORMAT", _wcsicmp(cfg->photoFormat, L"jpg") == 0 ? L"jpg" : L"png");
    // Own playlist (launcher "Playlist" tab): the bridge plays it and holds the game music at 0.
    {
        wchar_t pl[MAX_PATH] = {}, vol[16];
        if (!PLAYLIST_HIDDEN && cfg->musicEnabled && cfg->musicPlaylist[0])  // playlist parked: the game keeps its own music
        {
            swprintf_s(pl, MAX_PATH, L"%ls\\playlists\\%ls.m3u8", g_exeDir, cfg->musicPlaylist);
            if (GetFileAttributesW(pl) == INVALID_FILE_ATTRIBUTES) pl[0] = 0;
        }
        swprintf_s(vol, 16, L"%d", cfg->musicVolume);
        SetEnvironmentVariableW(L"ONRUSH_MUSIC_PLAYLIST", pl[0] ? pl : nullptr);
        SetEnvironmentVariableW(L"ONRUSH_MUSIC_SHUFFLE", pl[0] && cfg->musicShuffle ? L"1" : nullptr);
        SetEnvironmentVariableW(L"ONRUSH_MUSIC_VOLUME", pl[0] ? vol : nullptr);
    }
    {
        wchar_t gameDir[MAX_PATH], saveDir[MAX_PATH];
        GetEffectiveGameDir(cfg, gameDir, MAX_PATH);
        if (!cfg->savePath[0]) wcscpy_s(saveDir, MAX_PATH, L"");
        else if (PathIsRelativeW(cfg->savePath)) PathCombineW(saveDir, gameDir, cfg->savePath);
        else wcscpy_s(saveDir, MAX_PATH, cfg->savePath);
        SetEnvironmentVariableW(L"ONRUSH_SAVE_DIR", saveDir[0] ? saveDir : nullptr);
    }

    // Prefer the branded copy (scripts/brand_exe.py): shows "ONRUSH(TM)" / onrush-pc.exe in Task Manager.
    wchar_t targetExe[MAX_PATH];
    swprintf_s(targetExe, MAX_PATH, L"%ls\\onrush-pc.exe", gameDir);
    if (GetFileAttributesW(targetExe) == INVALID_FILE_ATTRIBUTES)
        swprintf_s(targetExe, MAX_PATH, L"%ls\\game_release_xbox.exe", gameDir);

    DWORD exeAttr = GetFileAttributesW(targetExe);
    if (exeAttr == INVALID_FILE_ATTRIBUTES || (exeAttr & FILE_ATTRIBUTE_DIRECTORY))
    {
        LogPrintf(L"\r\n======================================================================\r\n");
        LogPrintf(L"[LAUNCHER ERROR] Executável do jogo não encontrado:\r\n  -> %ls\r\n", targetExe);
        LogPrintf(L"[LAUNCHER ERRO] Utilize a opção 'Pasta da Data do Jogo' (Browse...) para selecionar a pasta onde está game_release_xbox.exe.\r\n");
        LogPrintf(L"======================================================================\r\n\r\n");
        PostMessageW(g_hMainWnd, WM_GAME_EXITED, 0, (LPARAM)ERROR_FILE_NOT_FOUND);
        return 1;
    }

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};

    LogPrintf(L"\r\n======================================================================\r\n");
    LogPrintf(L"[LAUNCHER] Disparando ONRUSH (Xbox One ERA) em ambiente supervisionado...\r\n");
    LogPrintf(L"[LAUNCHER] Pasta do Jogo: %ls\r\n", gameDir);
    LogPrintf(L"[LAUNCHER] Executável: %ls\r\n", targetExe);
    LogPrintf(L"[LAUNCHER] Resolução: %dx%d (%ls) | VSync: %ls\r\n",
              cfg->width, cfg->height, cfg->windowMode, cfg->vsync ? L"Ativo" : L"Desativado");
    LogPrintf(L"======================================================================\r\n\r\n");

    // Perfis tab, per profile (runtime switches, the bridge forwards them as launch args; save untouched):
    //  "Desbloquear tudo": the game's own -giveentitlements (beta / deluxe / pre-order) + -unlockall
    //      (item, crashtag, class-video and career-event checks answer "unlocked").
    //  "Desbloquear cosméticos": -giveentitlements + -unlockcosmetics (bridge forces only the item and
    //      crashtag checks exe+0x3D7180 / 0x3D7360; events stay as saved).
    wchar_t cmdLine[MAX_PATH + 96];
    bool unlockAll = ProfAllUnlocked(cfg->playerName);
    bool unlockCosmetics = !unlockAll && ProfUnlocked(cfg->playerName);
    swprintf_s(cmdLine, L"\"%ls\"%ls", targetExe,
               unlockAll ? L" -giveentitlements -unlockall" : unlockCosmetics ? L" -giveentitlements -unlockcosmetics" : L"");
    LogPrintf(L"[LAUNCHER] Perfil: %ls | desbloqueio: %ls\r\n", cfg->playerName,
              unlockAll ? L"tudo" : unlockCosmetics ? L"cosméticos" : L"nenhum");

    BOOL created = CreateProcessW(
        targetExe,
        cmdLine,
        nullptr,
        nullptr,
        FALSE,
        DEBUG_ONLY_THIS_PROCESS | CREATE_SUSPENDED | CREATE_NO_WINDOW,  // game exe is console-subsystem: no CMD window
        nullptr,
        gameDir,
        &si,
        &pi
    );

    if (!created)
    {
        DWORD err = GetLastError();
        LogPrintf(L"[LAUNCHER ERROR] Falha ao criar processo do jogo (Erro Win32: %lu)\r\n", err);
        if (err == ERROR_FILE_NOT_FOUND)
        {
            LogPrintf(L"[LAUNCHER AJUDA] Erro Win32: 2 (ERROR_FILE_NOT_FOUND).\r\nVerifique se o caminho da pasta do jogo contém game_release_xbox.exe e todas as suas dependências.\r\n");
        }
        PostMessageW(g_hMainWnd, WM_GAME_EXITED, 0, (LPARAM)err);
        return 1;
    }

    g_hGameProcess = pi.hProcess;
    g_gamePid = pi.dwProcessId;
    PostMessageW(g_hMainWnd, WM_GAME_STARTED, 0, (LPARAM)pi.dwProcessId);

    LogPrintf(L"[LAUNCHER] Processo criado com sucesso! PID: %lu\r\n", pi.dwProcessId);

    ResumeThread(pi.hThread);

    std::vector<ModuleRecord> modules;
    DEBUG_EVENT evt = {};
    DWORD continueStatus = DBG_CONTINUE;

    while (WaitForDebugEvent(&evt, INFINITE))
    {
        continueStatus = DBG_CONTINUE;

        switch (evt.dwDebugEventCode)
        {
        case CREATE_PROCESS_DEBUG_EVENT:
        {
            ModuleRecord mod = {};
            mod.base = (ULONG_PTR)evt.u.CreateProcessInfo.lpBaseOfImage;
            wcscpy_s(mod.name, MAX_PATH, L"game_release_xbox.exe");
            modules.push_back(mod);
            if (evt.u.CreateProcessInfo.hFile)
                CloseHandle(evt.u.CreateProcessInfo.hFile);
            break;
        }

        case LOAD_DLL_DEBUG_EVENT:
        {
            ModuleRecord mod = {};
            mod.base = (ULONG_PTR)evt.u.LoadDll.lpBaseOfDll;
            if (evt.u.LoadDll.hFile)
            {
                GetFinalPathNameByHandleW(evt.u.LoadDll.hFile, mod.path, MAX_PATH, FILE_NAME_NORMALIZED);
                const wchar_t *slash = wcsrchr(mod.path, L'\\');
                if (slash) wcscpy_s(mod.name, MAX_PATH, slash + 1);
                CloseHandle(evt.u.LoadDll.hFile);
            }
            modules.push_back(mod);
            break;
        }

        case OUTPUT_DEBUG_STRING_EVENT:
        {
            OUTPUT_DEBUG_STRING_INFO *pStr = &evt.u.DebugString;
            if (pStr->fUnicode)
            {
                size_t wchars = pStr->nDebugStringLength;
                wchar_t *wbuf = (wchar_t *)malloc((wchars + 2) * sizeof(wchar_t));
                if (wbuf)
                {
                    SIZE_T read = 0;
                    ReadProcessMemory(pi.hProcess, pStr->lpDebugStringData, wbuf, wchars * sizeof(wchar_t), &read);
                    wbuf[read / sizeof(wchar_t)] = L'\0';
                    LogPrintf(L"%ls", wbuf);
                    free(wbuf);
                }
            }
            else
            {
                char *buf = (char *)malloc(pStr->nDebugStringLength + 2);
                if (buf)
                {
                    SIZE_T read = 0;
                    ReadProcessMemory(pi.hProcess, pStr->lpDebugStringData, buf, pStr->nDebugStringLength, &read);
                    buf[read] = '\0';
                    wchar_t wmsg[2048] = {};
                    MultiByteToWideChar(CP_UTF8, 0, buf, -1, wmsg, 2048);
                    LogPrintf(L"%ls", wmsg);
                    free(buf);
                }
            }
            break;
        }

        case EXCEPTION_DEBUG_EVENT:
        {
            EXCEPTION_RECORD *pRec = &evt.u.Exception.ExceptionRecord;
            DWORD firstChance = evt.u.Exception.dwFirstChance;

            if (pRec->ExceptionCode == 0x80000003 || pRec->ExceptionCode == 0x40080201)
            {
                continueStatus = DBG_CONTINUE;
                break;
            }

            if (pRec->ExceptionCode == 0xE06D7363) // MSVC C++ Exception
            {
                if (firstChance)
                {
                    continueStatus = DBG_EXCEPTION_NOT_HANDLED;
                    break;
                }
            }

            wchar_t modName[MAX_PATH] = L"unknown";
            ULONG_PTR modOffset = 0;
            ULONG_PTR faultAddr = (ULONG_PTR)pRec->ExceptionAddress;
            ULONG_PTR bestBase = 0;
            for (const auto &m : modules)
            {
                if (faultAddr >= m.base && m.base > bestBase)
                {
                    bestBase = m.base;
                    wcscpy_s(modName, MAX_PATH, m.name);
                    modOffset = faultAddr - m.base;
                }
            }

            // first-chance exceptions are often handled by the game or the bridge and repeat a lot:
            // the first 30 per game session go to the log (all of them in debug mode)
            if (firstChance)
            {
                if (g_debugMode || ++g_firstChanceLogged <= 30)
                    LogPrintf(L"[LAUNCHER] Exceção First-Chance: 0x%08X no Endereço: %p (%ls + 0x%llX)\r\n",
                              pRec->ExceptionCode, pRec->ExceptionAddress, modName, (unsigned long long)modOffset);
                if (!g_debugMode && g_firstChanceLogged == 30)
                    LogPrintf(L"[LAUNCHER] (próximas exceções First-Chance omitidas; log completo: modo de depuração)\r\n");
            }
            else
            {
                LogPrintf(L"\r\n======================================================================\r\n");
                LogPrintf(L"[LAUNCHER CRITICAL EXCEPTION] Processo não tratou a exceção!\r\n");
                LogPrintf(L"  Código: 0x%08X\r\n", pRec->ExceptionCode);
                LogPrintf(L"  Endereço de Falha: %p (%ls + 0x%llX)\r\n",
                          pRec->ExceptionAddress, modName, (unsigned long long)modOffset);
                if (pRec->ExceptionCode == 0xC0000005 && pRec->NumberParameters >= 2)
                {
                    LogPrintf(L"  Violação de Acesso (%ls) no endereço %p\r\n",
                              pRec->ExceptionInformation[0] == 0 ? L"Leitura (Read)" :
                              pRec->ExceptionInformation[0] == 1 ? L"Escrita (Write)" : L"Execução (DEP)",
                              (void*)pRec->ExceptionInformation[1]);
                }
                LogPrintf(L"======================================================================\r\n\r\n");
            }
            continueStatus = DBG_EXCEPTION_NOT_HANDLED;
            break;
        }

        case EXIT_PROCESS_DEBUG_EVENT:
        {
            DWORD exitCode = evt.u.ExitProcess.dwExitCode;
            LogPrintf(L"\r\n[LAUNCHER] Evento EXIT_PROCESS_DEBUG_EVENT recebido. dwExitCode: 0x%08X (%lu)\r\n", exitCode, exitCode);
            ContinueDebugEvent(evt.dwProcessId, evt.dwThreadId, continueStatus);

            // Wait for child process to terminate with WaitForSingleObject and query GetExitCodeProcess
            DWORD waitRes = WaitForSingleObject(pi.hProcess, 10000);
            if (waitRes == WAIT_OBJECT_0)
            {
                LogPrintf(L"[LAUNCHER] WaitForSingleObject: processo filho finalizado com sucesso (PID: %lu)\r\n",
                          pi.dwProcessId);
            }
            else if (waitRes == WAIT_TIMEOUT)
            {
                LogPrintf(L"[LAUNCHER] WaitForSingleObject: timeout atingido (10000 ms, PID: %lu)\r\n",
                          pi.dwProcessId);
            }
            else if (waitRes == WAIT_FAILED)
            {
                DWORD err = GetLastError();
                LogPrintf(L"[LAUNCHER] WaitForSingleObject falhou (PID: %lu, Erro Win32: %lu)\r\n",
                          pi.dwProcessId, err);
            }
            else
            {
                LogPrintf(L"[LAUNCHER] WaitForSingleObject retornou status 0x%08X (PID: %lu)\r\n",
                          waitRes, pi.dwProcessId);
            }

            DWORD procExitCode = 0;
            if (GetExitCodeProcess(pi.hProcess, &procExitCode))
            {
                LogPrintf(L"[LAUNCHER] GetExitCodeProcess confirmado (PID: %lu): Código de saída 0x%08X (%lu)\r\n",
                          pi.dwProcessId, procExitCode, procExitCode);
                exitCode = procExitCode;
            }
            else
            {
                DWORD err = GetLastError();
                LogPrintf(L"[LAUNCHER] GetExitCodeProcess falhou (PID: %lu, Erro Win32: %lu)\r\n",
                          pi.dwProcessId, err);
            }

            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            g_hGameProcess = nullptr;
            g_gamePid = 0;
            PostMessageW(g_hMainWnd, WM_GAME_EXITED, 0, (LPARAM)exitCode);
            return (DWORD)exitCode;
        }

        default:
            break;
        }

        ContinueDebugEvent(evt.dwProcessId, evt.dwThreadId, continueStatus);
    }

    DWORD waitRes = WaitForSingleObject(pi.hProcess, 10000);
    if (waitRes == WAIT_OBJECT_0)
    {
        LogPrintf(L"[LAUNCHER] WaitForSingleObject: processo filho finalizado com sucesso (PID: %lu)\r\n",
                  pi.dwProcessId);
    }
    else if (waitRes == WAIT_TIMEOUT)
    {
        LogPrintf(L"[LAUNCHER] WaitForSingleObject: timeout atingido (10000 ms, PID: %lu)\r\n",
                  pi.dwProcessId);
    }
    else if (waitRes == WAIT_FAILED)
    {
        DWORD err = GetLastError();
        LogPrintf(L"[LAUNCHER] WaitForSingleObject falhou (PID: %lu, Erro Win32: %lu)\r\n",
                  pi.dwProcessId, err);
    }
    else
    {
        LogPrintf(L"[LAUNCHER] WaitForSingleObject retornou status 0x%08X (PID: %lu)\r\n",
                  waitRes, pi.dwProcessId);
    }

    DWORD finalExitCode = 0;
    if (GetExitCodeProcess(pi.hProcess, &finalExitCode))
    {
        LogPrintf(L"[LAUNCHER] GetExitCodeProcess confirmado (PID: %lu): Código de saída 0x%08X (%lu)\r\n",
                  pi.dwProcessId, finalExitCode, finalExitCode);
    }
    else
    {
        DWORD err = GetLastError();
        LogPrintf(L"[LAUNCHER] GetExitCodeProcess falhou (PID: %lu, Erro Win32: %lu)\r\n",
                  pi.dwProcessId, err);
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    g_hGameProcess = nullptr;
    g_gamePid = 0;
    PostMessageW(g_hMainWnd, WM_GAME_EXITED, 0, (LPARAM)finalExitCode);
    return (DWORD)finalExitCode;
}

// ============================================================================
// Prerequisite Check
// ============================================================================

struct PrereqItem
{
    const wchar_t *name;
    const wchar_t *file;
    bool critical;
    bool found;
};

static std::vector<PrereqItem> CheckPrerequisites(const wchar_t *gameDir = nullptr)
{
    std::vector<PrereqItem> items = {
        { L"Executável do Jogo", L"game_release_xbox.exe", true, false },
        { L"Bridge Direct3D 12", L"d3d12_x.dll", true, false },
        { L"Kernel Durango (SlimEra)", L"kernelx.dll", true, false },
        { L"Audio Endpoint (MMDevAPI)", L"MMDevAPI.dll", true, false },
        { L"Audio Coprocessor (AcpHal)", L"AcpHal.dll", true, false },
        { L"Console GFX Driver (xg_x)", L"xg_x.dll", true, false },
        { L"Video Decoder (Bink2)", L"Bink2XboxOne.dll", true, false },
        { L"Junction de Assets (dat/)", L"dat\\index\\dat.ndx", true, false },
    };

    const wchar_t *targetDir = (gameDir && gameDir[0] != L'\0') ? gameDir : g_exeDir;

    for (auto &item : items)
    {
        wchar_t path[MAX_PATH];
        swprintf_s(path, MAX_PATH, L"%ls\\%ls", targetDir, item.file);
        DWORD attr = GetFileAttributesW(path);
        item.found = (attr != INVALID_FILE_ATTRIBUTES);
    }
    return items;
}

#include "launcher_ui.inc"
