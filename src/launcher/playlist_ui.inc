// ============================================================================
// Launcher "Playlist" tab: the player's own music for the game.
//  - playlists are UTF-8 .m3u8 files in <launcher dir>\playlists\<name>.m3u8
//  - create / rename / delete, choose one in a dropdown
//  - add files or folders (button or drag-and-drop from Explorer), reorder by dragging,
//    Del removes, double-click previews; search by title, artist or file name
//  - the bridge plays the chosen playlist in game and holds the game music at 0
// Included from launcher_ui.inc (uses its fonts, colours, T()/g_uiLang and SaveIni()).
// ============================================================================
#include <propkey.h>
#include <propvarutil.h>
#include <mfapi.h>
#include <mfmediaengine.h>
#include <string>
#include <algorithm>
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "oleaut32.lib")

#define ID_TAB_SETTINGS   1040
#define ID_TAB_PLAYLIST   1041
#define ID_TAB_HOME       1058
#define ID_TAB_SETUP      1059
#define ID_PL_COMBO       1042
#define ID_PL_NEW         1043
#define ID_PL_RENAME      1044
#define ID_PL_DELETE      1045
#define ID_PL_ENABLE      1046
#define ID_PL_SHUFFLE     1047
#define ID_PL_VOLUME      1048
#define ID_PL_ADDFILES    1049
#define ID_PL_ADDFOLDER   1050
#define ID_PL_SEARCH      1051
#define ID_PL_LIST        1052
#define ID_PL_NAMEEDIT    1053
#define ID_PL_PLAY        1054
#define ID_PL_STOP        1055
#define ID_PL_NEXT        1056
#define ID_PL_REMOVE      1057
#define WM_PL_PREVIEW_EVENT (WM_APP + 40)
#define PL_TIMER_ID       77

enum PlStr {
    P_TAB_PLAYLIST, P_PLAYLIST, P_NEW, P_RENAME, P_DELETE, P_ENABLE, P_SHUFFLE, P_VOLUME, P_ADDFILES, P_ADDFOLDER,
    P_SEARCH, P_EMPTY, P_EMPTY2, P_HINT, P_NONE, P_DEFAULTNAME, P_CONFIRM_DEL, P_NAME_EXISTS, P_SONGS, P_PREVIEW,
    P_NOTHING, P_REMOVE, P_PICK_FOLDER, P_NO_MATCH, P_NEED_PLAYLIST, P_GAME_NOTE, P_TAB_HOME, P_TAB_SETUP, P_COUNT
};
static const wchar_t *kPlStr[P_COUNT][3] = {
    { L"PLAYLIST", L"PLAYLIST", L"PLAYLIST" },
    { L"Playlist", L"Playlist", L"Playlist" },
    { L"Nova", L"New", L"Nueva" },
    { L"Renomear", L"Rename", L"Renombrar" },
    { L"Excluir", L"Delete", L"Eliminar" },
    { L"Tocar minha playlist no jogo", L"Play my playlist in game", L"Reproducir mi playlist en el juego" },
    { L"Ordem aleatória", L"Shuffle", L"Orden aleatorio" },
    { L"Volume da música", L"Music volume", L"Volumen de la música" },
    { L"Adicionar músicas", L"Add songs", L"Añadir canciones" },
    { L"Adicionar pasta", L"Add folder", L"Añadir carpeta" },
    { L"Pesquisar por música, artista ou arquivo", L"Search by song, artist or file", L"Buscar por canción, artista o archivo" },
    { L"Arraste músicas ou pastas para cá", L"Drag songs or folders here", L"Arrastra canciones o carpetas aquí" },
    { L"ou use \"Adicionar músicas\" no painel ao lado", L"or use \"Add songs\" in the side panel", L"o usa \"Añadir canciones\" en el panel lateral" },
    { L"Arraste para reordenar  ·  Del remove  ·  Duplo clique ouve a prévia",
      L"Drag to reorder  ·  Del removes  ·  Double-click previews",
      L"Arrastra para reordenar  ·  Supr elimina  ·  Doble clic para escuchar" },
    { L"(nenhuma playlist)", L"(no playlist)", L"(ninguna playlist)" },
    { L"Minha playlist", L"My playlist", L"Mi playlist" },
    { L"Excluir a playlist \"%ls\"?\nO arquivo vai para a Lixeira.", L"Delete the playlist \"%ls\"?\nThe file goes to the Recycle Bin.",
      L"¿Eliminar la playlist \"%ls\"?\nEl archivo va a la Papelera." },
    { L"Já existe uma playlist com esse nome.", L"A playlist with that name already exists.", L"Ya existe una playlist con ese nombre." },
    { L"%d músicas  ·  %ls", L"%d songs  ·  %ls", L"%d canciones  ·  %ls" },
    { L"PRÉVIA", L"PREVIEW", L"VISTA PREVIA" },
    { L"Nada tocando", L"Nothing playing", L"Nada en reproducción" },
    { L"Remover", L"Remove", L"Quitar" },
    { L"Selecione uma pasta com músicas", L"Select a folder with music", L"Selecciona una carpeta con música" },
    { L"Nenhuma música encontrada para \"%ls\"", L"No songs match \"%ls\"", L"Ninguna canción coincide con \"%ls\"" },
    { L"Crie uma playlist primeiro (botão Nova).", L"Create a playlist first (New button).", L"Crea una playlist primero (botón Nueva)." },
    { L"No jogo, a música original fica em silêncio enquanto a sua playlist toca.",
      L"In game, the original music is muted while your playlist plays.",
      L"En el juego, la música original se silencia mientras suena tu playlist." },
    { L"Jogar", L"Play", L"Jugar" },
    { L"Perfis", L"Profiles", L"Perfiles" },
};
static const wchar_t *PT(PlStr s) { return kPlStr[s][g_uiLang]; }

struct PlTrack { std::wstring path, title, artist; int secs = 0; bool off = false; };  // off: skipped in game (toggled there)
static std::vector<std::wstring> g_plNames;   // playlists on disk, sorted
static std::vector<PlTrack> g_plTracks;       // tracks of the current playlist
static std::vector<int> g_plView;             // list rows -> g_plTracks index (search filter)
static int g_plCur = -1;                      // index into g_plNames
static int g_tab = 0;                         // 0 = home, 1 = playlist, 2 = settings, 3 = installation
static bool g_plEditing = false, g_plEditIsNew = false;
static UINT g_dragListMsg = 0;
static int g_plDragFrom = -1;
static void ProfUpdateVisibility(HWND hwnd);
static void PhUpdateVisibility(HWND hwnd);
static void KbUpdateVisibility(HWND hwnd);
static HWND g_hTabHome = nullptr, g_hTabSettings = nullptr, g_hTabPlaylist = nullptr, g_hTabSetup = nullptr;
static HWND g_hPlCombo = nullptr, g_hPlNew = nullptr, g_hPlRename = nullptr, g_hPlDelete = nullptr;
static HWND g_hPlEnable = nullptr, g_hPlShuffle = nullptr, g_hPlVolume = nullptr, g_hPlAddFiles = nullptr, g_hPlAddFolder = nullptr;
static HWND g_hPlSearch = nullptr, g_hPlList = nullptr, g_hPlNameEdit = nullptr;
static HWND g_hPlPlay = nullptr, g_hPlStop = nullptr, g_hPlNext = nullptr, g_hPlRemove = nullptr;

// ---- files -----------------------------------------------------------------------------------
static void PlDir(wchar_t *out) { swprintf_s(out, MAX_PATH, L"%ls\\playlists", g_exeDir); }
static void PlFile(const std::wstring &name, wchar_t *out) { swprintf_s(out, MAX_PATH, L"%ls\\playlists\\%ls.m3u8", g_exeDir, name.c_str()); }

static std::string ToUtf8(const std::wstring &w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
static std::wstring FromUtf8(const std::string &s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}
static std::wstring FileNameOf(const std::wstring &p)
{
    size_t k = p.find_last_of(L"\\/");
    return k == std::wstring::npos ? p : p.substr(k + 1);
}

static void PlScanNames()
{
    g_plNames.clear();
    wchar_t pat[MAX_PATH]; PlDir(pat); wcscat_s(pat, L"\\*.m3u8");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring n = fd.cFileName;
            g_plNames.push_back(n.substr(0, n.size() - 5));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(g_plNames.begin(), g_plNames.end(), [](const std::wstring &a, const std::wstring &b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
}

// Title / artist / duration from the Windows property system (ID3, MP4, WMA, FLAC... tags).
static void PlReadTags(PlTrack &t)
{
    IPropertyStore *ps = nullptr;
    if (SUCCEEDED(SHGetPropertyStoreFromParsingName(t.path.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&ps))))
    {
        PROPVARIANT v;
        wchar_t buf[512];
        PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(PKEY_Title, &v)) && SUCCEEDED(PropVariantToString(v, buf, 512)) && buf[0]) t.title = buf;
        PropVariantClear(&v);
        if (SUCCEEDED(ps->GetValue(PKEY_Music_Artist, &v)) && SUCCEEDED(PropVariantToString(v, buf, 512)) && buf[0]) t.artist = buf;
        PropVariantClear(&v);
        if (t.artist.empty() && SUCCEEDED(ps->GetValue(PKEY_Music_AlbumArtist, &v)) && SUCCEEDED(PropVariantToString(v, buf, 512)) && buf[0]) t.artist = buf;
        PropVariantClear(&v);
        ULONGLONG dur = 0;
        if (SUCCEEDED(ps->GetValue(PKEY_Media_Duration, &v)) && SUCCEEDED(PropVariantToUInt64(v, &dur))) t.secs = (int)(dur / 10000000ULL);
        PropVariantClear(&v);
        ps->Release();
    }
    if (t.title.empty())
    {   // no tag: file name without extension
        std::wstring f = FileNameOf(t.path);
        size_t dot = f.find_last_of(L'.');
        t.title = dot == std::wstring::npos ? f : f.substr(0, dot);
    }
}

static void PlLoad(const std::wstring &name)
{
    g_plTracks.clear();
    wchar_t path[MAX_PATH]; PlFile(name, path);
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD size = GetFileSize(f, nullptr), got = 0;
    std::string data(size, '\0');
    ReadFile(f, data.data(), size, &got, nullptr);
    CloseHandle(f);
    size_t pos = data.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    PlTrack pending; bool haveMeta = false;
    while (pos < data.size())
    {
        size_t e = data.find('\n', pos);
        std::string line = data.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        pos = e == std::string::npos ? data.size() : e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        if (line.rfind("#EXTINF:", 0) == 0) { pending.secs = atoi(line.c_str() + 8); continue; }
        if (line.rfind("#ONRUSH:", 0) == 0)
        {   // #ONRUSH:<title>\t<artist>
            std::wstring m = FromUtf8(line.substr(8));
            size_t tab = m.find(L'\t');
            pending.title = m.substr(0, tab);
            pending.artist = tab == std::wstring::npos ? L"" : m.substr(tab + 1);
            haveMeta = true;
            continue;
        }
        if (line == "#ONRUSH-OFF") { pending.off = true; continue; }
        if (line[0] == '#') continue;
        pending.path = FromUtf8(line);
        if (!haveMeta) PlReadTags(pending);
        g_plTracks.push_back(pending);
        pending = PlTrack(); haveMeta = false;
    }
}

static void PlSave()
{
    if (g_plCur < 0 || g_plCur >= (int)g_plNames.size()) return;
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    PlDir(dir); CreateDirectoryW(dir, nullptr);
    PlFile(g_plNames[g_plCur], path);
    std::string out = "#EXTM3U\r\n";
    for (const PlTrack &t : g_plTracks)
    {
        char head[32]; sprintf_s(head, "#EXTINF:%d,", t.secs);
        out += head + ToUtf8(t.artist.empty() ? t.title : t.artist + L" - " + t.title) + "\r\n";
        out += "#ONRUSH:" + ToUtf8(t.title) + "\t" + ToUtf8(t.artist) + "\r\n";
        if (t.off) out += "#ONRUSH-OFF\r\n";
        out += ToUtf8(t.path) + "\r\n";
    }
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0; WriteFile(f, out.data(), (DWORD)out.size(), &wr, nullptr);
    CloseHandle(f);
}

static bool IsAudioFile(const wchar_t *p)
{
    const wchar_t *ext = PathFindExtensionW(p);
    static const wchar_t *kExt[] = { L".mp3", L".m4a", L".aac", L".wma", L".wav", L".flac", L".ogg", L".opus", L".aif", L".aiff", L".alac", L".mp4" };
    for (const wchar_t *e : kExt) if (_wcsicmp(ext, e) == 0) return true;
    return false;
}

// Adds a file, or every audio file under a folder (recursively), skipping duplicates.
static int PlAddPath(const std::wstring &p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return 0;
    int added = 0;
    if (a & FILE_ATTRIBUTE_DIRECTORY)
    {
        std::vector<std::wstring> files, dirs;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((p + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return 0;
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? dirs : files).push_back(p + L"\\" + fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        auto byName = [](const std::wstring &x, const std::wstring &y) { return StrCmpLogicalW(x.c_str(), y.c_str()) < 0; };
        std::sort(files.begin(), files.end(), byName);
        std::sort(dirs.begin(), dirs.end(), byName);
        for (auto &f : files) added += PlAddPath(f);
        for (auto &d : dirs) added += PlAddPath(d);
        return added;
    }
    if (!IsAudioFile(p.c_str())) return 0;
    for (const PlTrack &t : g_plTracks) if (_wcsicmp(t.path.c_str(), p.c_str()) == 0) return 0;
    PlTrack t; t.path = p; PlReadTags(t);
    g_plTracks.push_back(t);
    return 1;
}

static std::wstring PlCleanName(const wchar_t *in)
{
    std::wstring s;
    for (const wchar_t *p = in; *p && s.size() < 48; ++p)
        if (*p >= 32 && !wcschr(L"<>:\"/\\|?*", *p)) s += *p;
    while (!s.empty() && (s.back() == L' ' || s.back() == L'.')) s.pop_back();
    size_t b = s.find_first_not_of(L' ');
    return b == std::wstring::npos ? L"" : s.substr(b);
}
static int PlFindName(const std::wstring &n)
{
    for (size_t i = 0; i < g_plNames.size(); ++i) if (_wcsicmp(g_plNames[i].c_str(), n.c_str()) == 0) return (int)i;
    return -1;
}

// ---- preview player (Media Foundation media engine, audio only) -------------------------------
struct PlPreviewNotify : IMFMediaEngineNotify {
    volatile LONG refs = 1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **pp) override {
        if (id == __uuidof(IUnknown) || id == __uuidof(IMFMediaEngineNotify)) { *pp = this; AddRef(); return S_OK; }
        *pp = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override { LONG r = InterlockedDecrement(&refs); if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE EventNotify(DWORD ev, DWORD_PTR, DWORD) override {
        if (ev == MF_MEDIA_ENGINE_EVENT_ENDED || ev == MF_MEDIA_ENGINE_EVENT_ERROR || ev == MF_MEDIA_ENGINE_EVENT_PLAYING ||
            ev == MF_MEDIA_ENGINE_EVENT_PAUSE)
            PostMessageW(g_hMainWnd, WM_PL_PREVIEW_EVENT, ev, 0);
        return S_OK;
    }
};
static IMFMediaEngine *g_plEngine = nullptr;
static std::wstring g_plPreviewPath;   // file in the preview player ("" = nothing)
static int g_plPreviewIdx = -1;        // its row in g_plTracks (re-found by path after edits)
static bool g_plPreviewPaused = false;

static bool PlEnsureEngine()
{
    if (g_plEngine) return true;
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return false;
    IMFMediaEngineClassFactory *factory = nullptr;
    IMFAttributes *attr = nullptr;
    PlPreviewNotify *notify = new PlPreviewNotify();
    HRESULT hr = CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = MFCreateAttributes(&attr, 2);
    if (SUCCEEDED(hr)) hr = attr->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify);
    if (SUCCEEDED(hr)) hr = factory->CreateInstance(MF_MEDIA_ENGINE_AUDIOONLY, attr, &g_plEngine);
    if (attr) attr->Release();
    if (factory) factory->Release();
    notify->Release();
    return SUCCEEDED(hr) && g_plEngine;
}
static void PlPreviewVolume() { if (g_plEngine) g_plEngine->SetVolume(g_config.musicVolume / 100.0); }

static void PlPreviewStop()
{
    if (g_plEngine) { g_plEngine->Pause(); g_plEngine->SetCurrentTime(0); }
    g_plPreviewPath.clear();
    g_plPreviewPaused = false;
    KillTimer(g_hMainWnd, PL_TIMER_ID);
}
static void PlPreviewPlay(int track)
{
    if (track < 0 || track >= (int)g_plTracks.size() || !PlEnsureEngine()) return;
    g_plPreviewPath = g_plTracks[track].path;
    g_plPreviewIdx = track;
    g_plPreviewPaused = false;
    BSTR url = SysAllocString(g_plPreviewPath.c_str());
    g_plEngine->SetSource(url);
    SysFreeString(url);
    PlPreviewVolume();
    g_plEngine->Play();
    SetTimer(g_hMainWnd, PL_TIMER_ID, 250, nullptr);
}
static int PlPreviewIndex()
{
    if (g_plPreviewPath.empty()) return -1;
    if (g_plPreviewIdx >= 0 && g_plPreviewIdx < (int)g_plTracks.size() && g_plTracks[g_plPreviewIdx].path == g_plPreviewPath) return g_plPreviewIdx;
    for (size_t i = 0; i < g_plTracks.size(); ++i) if (g_plTracks[i].path == g_plPreviewPath) return g_plPreviewIdx = (int)i;
    return -1;
}
static void PlPreviewNext()
{
    if (g_plTracks.empty()) return;
    int i = PlPreviewIndex();
    PlPreviewPlay(i < 0 ? 0 : (i + 1) % (int)g_plTracks.size());
}

// ---- UI --------------------------------------------------------------------------------------
static std::wstring FmtTime(int s)
{
    wchar_t b[32];
    if (s >= 3600) swprintf_s(b, L"%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    else swprintf_s(b, L"%d:%02d", s / 60, s % 60);
    return b;
}

static bool PlMatches(const PlTrack &t, const wchar_t *q)
{
    if (!q[0]) return true;
    return StrStrIW(t.title.c_str(), q) || StrStrIW(t.artist.c_str(), q) || StrStrIW(FileNameOf(t.path).c_str(), q);
}

static void PlRefreshList()
{
    wchar_t q[128] = {};
    if (g_hPlSearch) GetWindowTextW(g_hPlSearch, q, 128);
    int sel = (int)SendMessageW(g_hPlList, LB_GETCURSEL, 0, 0);
    int top = (int)SendMessageW(g_hPlList, LB_GETTOPINDEX, 0, 0);
    SendMessageW(g_hPlList, WM_SETREDRAW, FALSE, 0);
    SendMessageW(g_hPlList, LB_RESETCONTENT, 0, 0);
    g_plView.clear();
    for (size_t i = 0; i < g_plTracks.size(); ++i)
        if (PlMatches(g_plTracks[i], q)) { g_plView.push_back((int)i); SendMessageW(g_hPlList, LB_ADDSTRING, 0, (LPARAM)i); }
    if (sel >= (int)g_plView.size()) sel = (int)g_plView.size() - 1;
    SendMessageW(g_hPlList, LB_SETTOPINDEX, top, 0);
    if (sel >= 0) SendMessageW(g_hPlList, LB_SETCURSEL, sel, 0);
    SendMessageW(g_hPlList, WM_SETREDRAW, TRUE, 0);
    ShowWindow(g_hPlList, g_tab == 1 && !g_plView.empty() ? SW_SHOW : SW_HIDE);
    InvalidateRect(g_hPlList, nullptr, TRUE);
    RECT rc = { 0, 0, UI_HERO_W, UI_H };
    InvalidateRect(g_hMainWnd, &rc, FALSE);
}

static void PlFillCombo()
{
    SendMessageW(g_hPlCombo, CB_RESETCONTENT, 0, 0);
    if (g_plNames.empty()) { SendMessageW(g_hPlCombo, CB_ADDSTRING, 0, (LPARAM)PT(P_NONE)); SendMessageW(g_hPlCombo, CB_SETCURSEL, 0, 0); }
    for (auto &n : g_plNames) SendMessageW(g_hPlCombo, CB_ADDSTRING, 0, (LPARAM)n.c_str());
    if (g_plCur >= 0) SendMessageW(g_hPlCombo, CB_SETCURSEL, g_plCur, 0);
    EnableWindow(g_hPlCombo, !g_plNames.empty());
    EnableWindow(g_hPlRename, g_plCur >= 0);
    EnableWindow(g_hPlDelete, g_plCur >= 0);
}

static void PlSelect(int idx)
{
    g_plCur = idx >= 0 && idx < (int)g_plNames.size() ? idx : -1;
    g_plTracks.clear();
    if (g_plCur >= 0) PlLoad(g_plNames[g_plCur]);
    wcscpy_s(g_config.musicPlaylist, 64, g_plCur >= 0 ? g_plNames[g_plCur].c_str() : L"");
    PlFillCombo();
    if (g_hPlSearch) SetWindowTextW(g_hPlSearch, L"");
    PlRefreshList();
}

static void PlCreateNamed(const std::wstring &name)
{
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    PlDir(dir); CreateDirectoryW(dir, nullptr);
    PlFile(name, path);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) { DWORD wr; WriteFile(f, "#EXTM3U\r\n", 9, &wr, nullptr); CloseHandle(f); }
    PlScanNames();
    PlSelect(PlFindName(name));
}

// A playlist must exist before songs can be added (drag-and-drop creates "My playlist").
static bool PlEnsurePlaylist()
{
    if (g_plCur >= 0) return true;
    std::wstring base = PT(P_DEFAULTNAME), n = base;
    for (int k = 2; PlFindName(n) >= 0; ++k) n = base + L" " + std::to_wstring(k);
    PlCreateNamed(n);
    return g_plCur >= 0;
}

static void PlUpdateVisibility(HWND hwnd);

static void PlBeginEdit(bool isNew)
{
    g_plEditing = true; g_plEditIsNew = isNew;
    SetWindowTextW(g_hPlNameEdit, isNew ? PT(P_DEFAULTNAME) : (g_plCur >= 0 ? g_plNames[g_plCur].c_str() : L""));
    ShowWindow(g_hPlCombo, SW_HIDE);
    ShowWindow(g_hPlNameEdit, SW_SHOW);
    SetFocus(g_hPlNameEdit);
    SendMessageW(g_hPlNameEdit, EM_SETSEL, 0, -1);
    InvalidateRect(g_hMainWnd, nullptr, FALSE);
}
static void PlEndEdit(bool commit)
{
    if (!g_plEditing) return;
    g_plEditing = false;
    wchar_t raw[128] = {};
    GetWindowTextW(g_hPlNameEdit, raw, 128);
    ShowWindow(g_hPlNameEdit, SW_HIDE);
    ShowWindow(g_hPlCombo, g_tab == 1 ? SW_SHOW : SW_HIDE);
    InvalidateRect(g_hMainWnd, nullptr, FALSE);
    std::wstring name = PlCleanName(raw);
    if (!commit || name.empty()) return;
    int existing = PlFindName(name);
    if (g_plEditIsNew)
    {
        if (existing >= 0) { MessageBoxW(g_hMainWnd, PT(P_NAME_EXISTS), L"ONRUSH", MB_ICONINFORMATION); return; }
        PlCreateNamed(name);
    }
    else if (g_plCur >= 0 && name != g_plNames[g_plCur])
    {
        if (existing >= 0 && existing != g_plCur) { MessageBoxW(g_hMainWnd, PT(P_NAME_EXISTS), L"ONRUSH", MB_ICONINFORMATION); return; }
        wchar_t from[MAX_PATH], to[MAX_PATH];
        PlFile(g_plNames[g_plCur], from); PlFile(name, to);
        if (MoveFileW(from, to)) { PlScanNames(); PlSelect(PlFindName(name)); }
    }
    SaveIni();
}

static void PlDeleteCurrent(HWND hwnd)
{
    if (g_plCur < 0) return;
    wchar_t msg[512];
    swprintf_s(msg, kPlStr[P_CONFIRM_DEL][g_uiLang], g_plNames[g_plCur].c_str());
    if (MessageBoxW(hwnd, msg, L"ONRUSH", MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) != IDYES) return;
    PlPreviewStop();
    wchar_t path[MAX_PATH + 2] = {};
    PlFile(g_plNames[g_plCur], path);   // double-NUL terminated for SHFileOperation
    SHFILEOPSTRUCTW op = {};
    op.hwnd = hwnd; op.wFunc = FO_DELETE; op.pFrom = path;
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
    SHFileOperationW(&op);
    int keep = g_plCur;
    PlScanNames();
    PlSelect(g_plNames.empty() ? -1 : min(keep, (int)g_plNames.size() - 1));
    SaveIni();
}

static void PlAddPaths(const std::vector<std::wstring> &paths)
{
    if (paths.empty() || !PlEnsurePlaylist()) return;
    HCURSOR old = SetCursor(LoadCursorW(nullptr, (LPCWSTR)IDC_WAIT));
    int added = 0;
    for (auto &p : paths) added += PlAddPath(p);
    SetCursor(old);
    if (added) { PlSave(); PlRefreshList(); }
}

static void PlPickFiles(HWND hwnd, bool folder)
{
    IFileOpenDialog *dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    DWORD opts = 0; dlg->GetOptions(&opts);
    if (folder) { dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM); dlg->SetTitle(PT(P_PICK_FOLDER)); }
    else
    {
        dlg->SetOptions(opts | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
        COMDLG_FILTERSPEC spec[] = { { L"Audio", L"*.mp3;*.m4a;*.aac;*.wma;*.wav;*.flac;*.ogg;*.opus;*.aif;*.aiff;*.alac;*.mp4" }, { L"*.*", L"*.*" } };
        dlg->SetFileTypes(2, spec);
    }
    std::vector<std::wstring> picked;
    if (SUCCEEDED(dlg->Show(hwnd)))
    {
        IShellItemArray *items = nullptr;
        if (SUCCEEDED(dlg->GetResults(&items)))
        {
            DWORD n = 0; items->GetCount(&n);
            for (DWORD i = 0; i < n; ++i)
            {
                IShellItem *it = nullptr; PWSTR p = nullptr;
                if (SUCCEEDED(items->GetItemAt(i, &it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { picked.push_back(p); CoTaskMemFree(p); }
                if (it) it->Release();
            }
            items->Release();
        }
    }
    dlg->Release();
    PlAddPaths(picked);
}

static void PlRemoveRow(int row)
{
    if (row < 0 || row >= (int)g_plView.size()) return;
    int t = g_plView[row];
    int playing = PlPreviewIndex();
    if (t == playing) PlPreviewStop();
    else if (playing > t) g_plPreviewIdx = playing - 1;
    g_plTracks.erase(g_plTracks.begin() + t);
    PlSave();
    PlRefreshList();
    int n = (int)SendMessageW(g_hPlList, LB_GETCOUNT, 0, 0);
    if (n > 0) SendMessageW(g_hPlList, LB_SETCURSEL, min(row, n - 1), 0);
}

static void PlCreateControls(HWND hwnd)
{
    const int X = UI_PANEL_X, W = UI_PANEL_W, HALF = (UI_PANEL_W - 12) / 2;
    DWORD od = WS_CHILD | WS_TABSTOP | BS_OWNERDRAW;
    g_hTabHome = CreateWindowW(L"BUTTON", L"", od | WS_VISIBLE, 16, 132, 184, 42, hwnd, (HMENU)ID_TAB_HOME, nullptr, nullptr);
    // with the playlist tab hidden the other tabs move up one slot
    const int yPl = 180, ySet = PLAYLIST_HIDDEN ? 180 : 228, ySetup = PLAYLIST_HIDDEN ? 228 : 276;
    g_hTabPlaylist = CreateWindowW(L"BUTTON", L"", od | (PLAYLIST_HIDDEN ? 0 : WS_VISIBLE), 16, yPl, 184, 42, hwnd, (HMENU)ID_TAB_PLAYLIST, nullptr, nullptr);
    g_hTabSettings = CreateWindowW(L"BUTTON", L"", od | WS_VISIBLE, 16, ySet, 184, 42, hwnd, (HMENU)ID_TAB_SETTINGS, nullptr, nullptr);
    g_hTabSetup = CreateWindowW(L"BUTTON", L"", od | WS_VISIBLE, 16, ySetup, 184, 42, hwnd, (HMENU)ID_TAB_SETUP, nullptr, nullptr);

    DWORD comboStyle = WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL;
    g_hPlCombo = CreateWindowW(WC_COMBOBOXW, L"", comboStyle, X, 78, W, 300, hwnd, (HMENU)ID_PL_COMBO, nullptr, nullptr);
    SetWindowTheme(g_hPlCombo, g_pal.dark ? L"DarkMode_CFD" : L"CFD", nullptr);
    SendMessageW(g_hPlCombo, CB_SETITEMHEIGHT, (WPARAM)-1, 24);
    g_hPlNameEdit = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, X + 10, 85, W - 20, 20, hwnd, (HMENU)ID_PL_NAMEEDIT, nullptr, nullptr);
    SendMessageW(g_hPlNameEdit, EM_LIMITTEXT, 48, 0);
    int bw = (W - 16) / 3;
    g_hPlNew = CreateWindowW(L"BUTTON", L"", od, X, 118, bw, 30, hwnd, (HMENU)ID_PL_NEW, nullptr, nullptr);
    g_hPlRename = CreateWindowW(L"BUTTON", L"", od, X + bw + 8, 118, bw, 30, hwnd, (HMENU)ID_PL_RENAME, nullptr, nullptr);
    g_hPlDelete = CreateWindowW(L"BUTTON", L"", od, X + 2 * (bw + 8), 118, bw, 30, hwnd, (HMENU)ID_PL_DELETE, nullptr, nullptr);
    g_hPlEnable = CreateWindowW(L"BUTTON", L"", od, X, 162, W, 24, hwnd, (HMENU)ID_PL_ENABLE, nullptr, nullptr);
    g_hPlShuffle = CreateWindowW(L"BUTTON", L"", od, X, 188, W, 24, hwnd, (HMENU)ID_PL_SHUFFLE, nullptr, nullptr);
    g_hPlVolume = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS | TBS_BOTH, X - 4, 242, W + 8, 26,
                                hwnd, (HMENU)ID_PL_VOLUME, nullptr, nullptr);
    SendMessageW(g_hPlVolume, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(g_hPlVolume, TBM_SETPAGESIZE, 0, 10);
    SendMessageW(g_hPlVolume, TBM_SETPOS, TRUE, g_config.musicVolume);
    g_hPlAddFiles = CreateWindowW(L"BUTTON", L"", od, X, 284, HALF, 30, hwnd, (HMENU)ID_PL_ADDFILES, nullptr, nullptr);
    g_hPlAddFolder = CreateWindowW(L"BUTTON", L"", od, X + HALF + 12, 284, HALF, 30, hwnd, (HMENU)ID_PL_ADDFOLDER, nullptr, nullptr);
    // preview card: 330..458; transport buttons at the bottom of the card
    g_hPlPlay = CreateWindowW(L"BUTTON", L"", od, X + 12, 416, 44, 30, hwnd, (HMENU)ID_PL_PLAY, nullptr, nullptr);
    g_hPlStop = CreateWindowW(L"BUTTON", L"", od, X + 62, 416, 44, 30, hwnd, (HMENU)ID_PL_STOP, nullptr, nullptr);
    g_hPlNext = CreateWindowW(L"BUTTON", L"", od, X + 112, 416, 44, 30, hwnd, (HMENU)ID_PL_NEXT, nullptr, nullptr);

    // song list over the art
    g_hPlSearch = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, 244, 75, UI_HERO_W - 268 - 100, 20, hwnd, (HMENU)ID_PL_SEARCH, nullptr, nullptr);
    g_hPlRemove = CreateWindowW(L"BUTTON", L"", od, UI_HERO_W - 24 - 92, 66, 92, 36, hwnd, (HMENU)ID_PL_REMOVE, nullptr, nullptr);
    g_hPlList = CreateWindowW(L"LISTBOX", L"", WS_CHILD | WS_TABSTOP | WS_VSCROLL | LBS_OWNERDRAWFIXED | LBS_NOTIFY |
                              LBS_NOINTEGRALHEIGHT | LBS_WANTKEYBOARDINPUT,
                              220, 114, UI_HERO_W - 244, UI_H - 114 - 44, hwnd, (HMENU)ID_PL_LIST, nullptr, nullptr);
    SetWindowTheme(g_hPlList, g_pal.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    MakeDragList(g_hPlList);
    g_dragListMsg = RegisterWindowMessageW(L"commctrl_DragListMsg");  // DRAGLISTMSGSTRING (ANSI in this build)
    DragAcceptFiles(hwnd, TRUE);
    // drag-and-drop from Explorer also works when the launcher is elevated
    ChangeWindowMessageFilterEx(hwnd, WM_DROPFILES, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hwnd, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hwnd, 0x0049 /* WM_COPYGLOBALDATA */, MSGFLT_ALLOW, nullptr);

    PlScanNames();
    PlSelect(PlFindName(g_config.musicPlaylist) >= 0 ? PlFindName(g_config.musicPlaylist) : (g_plNames.empty() ? -1 : 0));
}

static void PlApplyLanguage()
{
    SetWindowTextW(g_hTabHome, PT(P_TAB_HOME));
    SetWindowTextW(g_hTabSettings, T(S_TAB_SETTINGS));
    SetWindowTextW(g_hTabPlaylist, PT(P_TAB_PLAYLIST));
    SetWindowTextW(g_hTabSetup, PT(P_TAB_SETUP));
    SetWindowTextW(g_hPlNew, PT(P_NEW));
    SetWindowTextW(g_hPlRename, PT(P_RENAME));
    SetWindowTextW(g_hPlDelete, PT(P_DELETE));
    SetWindowTextW(g_hPlEnable, PT(P_ENABLE));
    SetWindowTextW(g_hPlShuffle, PT(P_SHUFFLE));
    SetWindowTextW(g_hPlAddFiles, PT(P_ADDFILES));
    SetWindowTextW(g_hPlAddFolder, PT(P_ADDFOLDER));
    SetWindowTextW(g_hPlRemove, PT(P_REMOVE));
    SetWindowTextW(g_hPlPlay, g_plPreviewPath.empty() || g_plPreviewPaused ? L"▶" : L"❚❚");
    SetWindowTextW(g_hPlStop, L"■");
    SetWindowTextW(g_hPlNext, L"▶▶");
    SendMessageW(g_hPlSearch, EM_SETCUEBANNER, TRUE, (LPARAM)PT(P_SEARCH));
    PlFillCombo();
}

// Shows the controls of the active tab.
static void PlUpdateVisibility(HWND hwnd)
{
    bool pl = g_tab == 1, settingsPage = g_tab == 2;
    int s = settingsPage ? SW_SHOW : SW_HIDE, p = pl ? SW_SHOW : SW_HIDE;
    HWND settings[] = { g_hResCombo, g_hModeCombo, g_hAspectCombo, g_hTextLangCombo, g_hVoiceLangCombo, g_hPhotoFmtCombo, g_hVsyncChk, g_hDiscordChk,
                        g_hSkipIntroChk, g_hSaveEdit, g_hAdvBtn, GetDlgItem(hwnd, ID_SAVE_BROWSE), g_hMinimizeChk, g_hThemeCombo };
    for (HWND h : settings) if (h) ShowWindow(h, s);
    ShowWindow(g_hGamePathEdit, g_tab == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(hwnd, ID_GAME_PATH_BROWSE), g_tab == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hSaveEdit, settingsPage ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(hwnd, ID_SAVE_BROWSE), settingsPage ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hBtnLaunch, g_tab == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hBtnOpenGame, settingsPage ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hBtnOpenSave, settingsPage ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hLogBtn, SW_SHOW);
    ShowWindow(g_hUiLangCombo, SW_SHOW);
    ShowWindow(g_hBtnUpdate, SW_SHOW);
    HWND adv[] = { g_hD3DDebugChk, g_hVerboseChk };
    for (HWND h : adv) if (h) ShowWindow(h, settingsPage && g_showAdvanced ? SW_SHOW : SW_HIDE);
    HWND plc[] = { g_hPlNew, g_hPlRename, g_hPlDelete, g_hPlEnable, g_hPlShuffle, g_hPlVolume, g_hPlAddFiles, g_hPlAddFolder,
                   g_hPlPlay, g_hPlStop, g_hPlNext, g_hPlSearch, g_hPlRemove };
    for (HWND h : plc) ShowWindow(h, p);
    ShowWindow(g_hPlCombo, pl && !g_plEditing ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hPlNameEdit, pl && g_plEditing ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hPlList, pl && !g_plView.empty() ? SW_SHOW : SW_HIDE);
    if (g_tab != 0 && g_showLog) { g_showLog = false; LayoutLog(hwnd); }   // the log is a view of the Jogar tab
    GfxUpdateVisibility(settingsPage);
    InvalidateRect(g_hTabHome, nullptr, TRUE);
    InvalidateRect(g_hTabSettings, nullptr, TRUE);
    InvalidateRect(g_hTabPlaylist, nullptr, TRUE);
    InvalidateRect(g_hTabSetup, nullptr, TRUE);
    ProfUpdateVisibility(hwnd);
    PhUpdateVisibility(hwnd);
    KbUpdateVisibility(hwnd);
    InvalidateRect(hwnd, nullptr, FALSE);
}

static void PlSetTab(HWND hwnd, int tab)
{
    if (g_plEditing) PlEndEdit(true);
    g_tab = tab;
    PlUpdateVisibility(hwnd);
}

// Left area while the playlist tab is open: title, search box, list frame, empty state.
static void PlDrawHero(Gdiplus::Graphics &g)
{
    Gdiplus::SolidBrush dim(GPCA(240, g_pal.heroDim));
    g.FillRectangle(&dim, 216, 0, UI_HERO_W - 216, UI_H);
    Gdiplus::FontFamily ff(L"Segoe UI");
    Gdiplus::Font fTitle(&ff, 22, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::Font fSmall(&ff, 12, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::Font fBig(&ff, 17, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush white(Gdiplus::Color(255, 240, 243, 246)), muted(Gdiplus::Color(255, 150, 158, 170)),
        orange(Gdiplus::Color(255, 255, 92, 40));
    const wchar_t *name = g_plCur >= 0 ? g_plNames[g_plCur].c_str() : PT(P_NONE);
    g.DrawString(name, -1, &fTitle, Gdiplus::PointF(232, 18), &white);
    int total = 0; for (auto &t : g_plTracks) total += t.secs;
    wchar_t info[128]; swprintf_s(info, kPlStr[P_SONGS][g_uiLang], (int)g_plTracks.size(), FmtTime(total).c_str());
    Gdiplus::StringFormat right; right.SetAlignment(Gdiplus::StringAlignmentFar);
    g.DrawString(info, -1, &fSmall, Gdiplus::RectF(300, 26, UI_HERO_W - 320.f, 20), &right, &muted);

    // search box
    Gdiplus::GraphicsPath box;
    Gdiplus::RectF sr(220, 66, UI_HERO_W - 244.f - 100, 36);
    float r = 8;
    box.AddArc(sr.X, sr.Y, r * 2, r * 2, 180, 90); box.AddArc(sr.GetRight() - r * 2, sr.Y, r * 2, r * 2, 270, 90);
    box.AddArc(sr.GetRight() - r * 2, sr.GetBottom() - r * 2, r * 2, r * 2, 0, 90); box.AddArc(sr.X, sr.GetBottom() - r * 2, r * 2, r * 2, 90, 90);
    box.CloseFigure();
    Gdiplus::SolidBrush inBg(Gdiplus::Color(255, 30, 22, 17));
    Gdiplus::Pen inBorder(Gdiplus::Color(255, 76, 55, 41));
    g.FillPath(&inBg, &box); g.DrawPath(&inBorder, &box);

    Gdiplus::StringFormat center; center.SetAlignment(Gdiplus::StringAlignmentCenter); center.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    if (g_plTracks.empty())
    {   // drop zone
        Gdiplus::Pen dash(Gdiplus::Color(255, 70, 78, 92), 2);
        dash.SetDashStyle(Gdiplus::DashStyleDash);
        g.DrawRectangle(&dash, 220, 114, UI_HERO_W - 244, UI_H - 114 - 44);
        g.DrawString(L"♫", -1, &fTitle, Gdiplus::RectF(220, 250, UI_HERO_W - 244.f, 40), &center, &orange);
        g.DrawString(PT(P_EMPTY), -1, &fBig, Gdiplus::RectF(220, 292, UI_HERO_W - 244.f, 28), &center, &white);
        g.DrawString(PT(P_EMPTY2), -1, &fSmall, Gdiplus::RectF(220, 322, UI_HERO_W - 244.f, 22), &center, &muted);
    }
    else if (g_plView.empty())
    {
        wchar_t q[128] = {}, msg[256]; GetWindowTextW(g_hPlSearch, q, 128);
        swprintf_s(msg, kPlStr[P_NO_MATCH][g_uiLang], q);
        g.DrawString(msg, -1, &fSmall, Gdiplus::RectF(220, 200, UI_HERO_W - 244.f, 22), &center, &muted);
    }
    g.DrawString(PT(P_HINT), -1, &fSmall, Gdiplus::PointF(220, UI_H - 32), &muted);
    Gdiplus::SolidBrush rail(GPC(COLOR_BG_MAIN));
    g.FillRectangle(&rail, 0, 0, 216, UI_H);
    Gdiplus::SolidBrush ready(Gdiplus::Color(255, 145, 195, 133));
    Gdiplus::SolidBrush missing(Gdiplus::Color(255, 220, 86, 62));
    DrawRailBrand(g);
    g.DrawString(T(S_GAMEDIR), -1, &fSmall, Gdiplus::PointF(24, 470), &muted);
    const wchar_t *gameStatus = g_gameRunning ? T(S_RUNNING) : (g_gameDirOk ? T(S_READY) : T(S_NOTFOUND));
    g.FillEllipse(g_gameDirOk ? &ready : &missing, 24, 498, 8, 8);
    g.DrawString(gameStatus, -1, &fSmall, Gdiplus::PointF(40, 492), &white);
}

static void PlDrawPanel(HDC hdc)
{
    const int X = UI_PANEL_X, W = UI_PANEL_W;
    SelectObject(hdc, g_hFontUI);
    SetTextColor(hdc, COLOR_TEXT_PRIMARY);
    TextOutW(hdc, X, 58, PT(P_PLAYLIST), (int)wcslen(PT(P_PLAYLIST)));
    if (g_plEditing) { RECT r = { X, 78, X + W, 110 }; FillRound(hdc, r, 8, COLOR_BG_INPUT, COLOR_ACCENT_ORANGE); }
    wchar_t vol[64]; swprintf_s(vol, L"%ls  ·  %d%%", PT(P_VOLUME), g_config.musicVolume);
    TextOutW(hdc, X, 222, vol, (int)wcslen(vol));

    // preview card
    RECT card = { X, 330, X + W, 458 };
    FillRound(hdc, card, 10, COLOR_BG_INPUT, COLOR_BORDER);
    SelectObject(hdc, g_hFontSmallCaps);
    SetTextColor(hdc, COLOR_TEXT_MUTED);
    TextOutW(hdc, X + 12, 342, PT(P_PREVIEW), (int)wcslen(PT(P_PREVIEW)));
    int idx = PlPreviewIndex();
    RECT rt = { X + 12, 358, X + W - 12, 378 };
    SelectObject(hdc, g_hFontUIBold);
    SetTextColor(hdc, COLOR_TEXT_PRIMARY);
    DrawTextW(hdc, idx >= 0 ? g_plTracks[idx].title.c_str() : PT(P_NOTHING), -1, &rt, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (idx >= 0)
    {
        RECT ra = { X + 12, 377, X + W - 12, 395 };
        SelectObject(hdc, g_hFontUI);
        SetTextColor(hdc, COLOR_TEXT_MUTED);
        DrawTextW(hdc, g_plTracks[idx].artist.c_str(), -1, &ra, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    // progress
    double cur = 0, dur = 0;
    if (g_plEngine && !g_plPreviewPath.empty()) { cur = g_plEngine->GetCurrentTime(); dur = g_plEngine->GetDuration(); }
    if (!(dur > 0) || dur != dur || dur > 1e7) dur = 0;
    RECT bar = { X + 12, 401, X + W - 12, 405 };
    FillRound(hdc, bar, 4, RGB(50, 56, 66), RGB(50, 56, 66));
    if (dur > 0)
    {
        RECT fill = bar; fill.right = bar.left + (LONG)((bar.right - bar.left) * min(1.0, cur / dur));
        if (fill.right > fill.left + 2) FillRound(hdc, fill, 4, COLOR_ACCENT_ORANGE, COLOR_ACCENT_ORANGE);
    }
    std::wstring tm = FmtTime((int)cur) + L" / " + FmtTime((int)dur);
    RECT rtm = { X + 160, 416, X + W - 12, 446 };
    SelectObject(hdc, g_hFontUI);
    SetTextColor(hdc, COLOR_TEXT_MUTED);
    DrawTextW(hdc, tm.c_str(), -1, &rtm, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    RECT note = { X, 470, X + W, 520 };
    DrawTextW(hdc, PT(P_GAME_NOTE), -1, &note, DT_LEFT | DT_WORDBREAK);
}

// One row: title (+ duration on the right), artist · file name below.
static void PlDrawListItem(const DRAWITEMSTRUCT *di)
{
    if (di->itemID == (UINT)-1) return;
    int t = (int)di->itemData;
    if (t < 0 || t >= (int)g_plTracks.size()) return;
    const PlTrack &tr = g_plTracks[t];
    HDC hdc = di->hDC;
    RECT r = di->rcItem;
    bool sel = (di->itemState & ODS_SELECTED) != 0;
    bool playing = PlPreviewIndex() == t;
    HBRUSH b = CreateSolidBrush(sel ? RGB(48, 34, 28) : COLOR_BG_INPUT);
    FillRect(hdc, &r, b); DeleteObject(b);
    if (sel || playing) { RECT acc = { r.left, r.top + 6, r.left + 3, r.bottom - 6 }; HBRUSH ab = CreateSolidBrush(COLOR_ACCENT_ORANGE); FillRect(hdc, &acc, ab); DeleteObject(ab); }
    RECT sep = { r.left + 12, r.bottom - 1, r.right - 12, r.bottom };
    HBRUSH sb = CreateSolidBrush(RGB(30, 34, 40)); FillRect(hdc, &sep, sb); DeleteObject(sb);
    SetBkMode(hdc, TRANSPARENT);
    wchar_t num[16]; swprintf_s(num, L"%d", t + 1);
    RECT rn = { r.left + 8, r.top, r.left + 40, r.bottom };
    SelectObject(hdc, g_hFontUI);
    SetTextColor(hdc, playing ? COLOR_ACCENT_ORANGE : COLOR_TEXT_MUTED);
    DrawTextW(hdc, playing ? L"♪" : num, -1, &rn, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    RECT rd = { r.right - 70, r.top, r.right - 12, r.top + 24 };
    SetTextColor(hdc, COLOR_TEXT_MUTED);
    if (tr.secs > 0) DrawTextW(hdc, FmtTime(tr.secs).c_str(), -1, &rd, DT_RIGHT | DT_BOTTOM | DT_SINGLELINE);
    RECT rt = { r.left + 52, r.top + 4, r.right - 76, r.top + 24 };
    SelectObject(hdc, g_hFontUIBold);
    SetTextColor(hdc, playing ? RGB(255, 140, 90) : COLOR_TEXT_PRIMARY);
    DrawTextW(hdc, tr.title.c_str(), -1, &rt, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS);
    std::wstring sub = tr.artist.empty() ? FileNameOf(tr.path) : tr.artist + L"  ·  " + FileNameOf(tr.path);
    RECT rs = { r.left + 52, r.top + 24, r.right - 12, r.bottom - 4 };
    SelectObject(hdc, g_hFontUI);
    SetTextColor(hdc, COLOR_TEXT_MUTED);
    DrawTextW(hdc, sub.c_str(), -1, &rs, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
}

// Volume slider custom draw: orange filled track + round knob.
static LRESULT PlVolumeCustomDraw(NMCUSTOMDRAW *cd)
{
    if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (cd->dwDrawStage != CDDS_ITEMPREPAINT) return CDRF_DODEFAULT;
    if (cd->dwItemSpec == TBCD_CHANNEL)
    {
        RECT full; GetClientRect(cd->hdr.hwndFrom, &full);
        HBRUSH bg = CreateSolidBrush(COLOR_BG_CARD); FillRect(cd->hdc, &full, bg); DeleteObject(bg);
        RECT ch = cd->rc; int mid = (ch.top + ch.bottom) / 2; ch.top = mid - 2; ch.bottom = mid + 2;
        FillRound(cd->hdc, ch, 4, RGB(58, 64, 74), RGB(58, 64, 74));
        RECT thumb; SendMessageW(cd->hdr.hwndFrom, TBM_GETTHUMBRECT, 0, (LPARAM)&thumb);
        RECT on = ch; on.right = (thumb.left + thumb.right) / 2;
        if (on.right > on.left + 2) FillRound(cd->hdc, on, 4, COLOR_ACCENT_ORANGE, COLOR_ACCENT_ORANGE);
        return CDRF_SKIPDEFAULT;
    }
    if (cd->dwItemSpec == TBCD_THUMB)
    {
        RECT t = cd->rc; int cx = (t.left + t.right) / 2, cy = (t.top + t.bottom) / 2;
        RECT k = { cx - 8, cy - 8, cx + 8, cy + 8 };
        FillRound(cd->hdc, k, 16, RGB(245, 245, 245), COLOR_ACCENT_ORANGE);
        return CDRF_SKIPDEFAULT;
    }
    return CDRF_SKIPDEFAULT;
}

// Returns true when the message was handled (result in *res).
static bool PlHandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT *res)
{
    if (g_dragListMsg && msg == g_dragListMsg)
    {
        DRAGLISTINFO *dl = (DRAGLISTINFO *)lParam;
        wchar_t q[8] = {}; GetWindowTextW(g_hPlSearch, q, 8);
        switch (dl->uNotification)
        {
        case DL_BEGINDRAG:
            g_plDragFrom = q[0] ? -1 : LBItemFromPt(dl->hWnd, dl->ptCursor, FALSE);   // reorder only without a filter
            *res = g_plDragFrom >= 0; return true;
        case DL_DRAGGING:
        {
            int at = LBItemFromPt(dl->hWnd, dl->ptCursor, TRUE);
            DrawInsert(hwnd, dl->hWnd, at);
            *res = at >= 0 ? DL_MOVECURSOR : DL_STOPCURSOR; return true;
        }
        case DL_DROPPED:
        {
            int at = LBItemFromPt(dl->hWnd, dl->ptCursor, TRUE);
            DrawInsert(hwnd, dl->hWnd, -1);
            if (at >= 0 && g_plDragFrom >= 0 && at != g_plDragFrom && at != g_plDragFrom + 1)
            {
                int playingRow = PlPreviewIndex();
                PlTrack t = g_plTracks[g_plDragFrom];
                g_plTracks.erase(g_plTracks.begin() + g_plDragFrom);
                int to = at > g_plDragFrom ? at - 1 : at;
                if (to > (int)g_plTracks.size()) to = (int)g_plTracks.size();
                g_plTracks.insert(g_plTracks.begin() + to, t);
                if (playingRow == g_plDragFrom) g_plPreviewIdx = to;
                else if (playingRow > g_plDragFrom && playingRow <= to) g_plPreviewIdx = playingRow - 1;
                else if (playingRow < g_plDragFrom && playingRow >= to) g_plPreviewIdx = playingRow + 1;
                PlSave(); PlRefreshList();
                SendMessageW(g_hPlList, LB_SETCURSEL, to, 0);
            }
            g_plDragFrom = -1;
            *res = 0; return true;
        }
        case DL_CANCELDRAG:
            DrawInsert(hwnd, dl->hWnd, -1); g_plDragFrom = -1; *res = 0; return true;
        }
        return false;
    }
    switch (msg)
    {
    case WM_DROPFILES:
    {
        if (PLAYLIST_HIDDEN) { DragFinish((HDROP)wParam); *res = 0; return true; }
        HDROP drop = (HDROP)wParam;
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        std::vector<std::wstring> paths;
        for (UINT i = 0; i < n; ++i) { wchar_t p[MAX_PATH]; if (DragQueryFileW(drop, i, p, MAX_PATH)) paths.push_back(p); }
        DragFinish(drop);
        if (g_tab != 1) PlSetTab(hwnd, 1);
        PlAddPaths(paths);
        *res = 0; return true;
    }
    case WM_VKEYTOITEM:
        if ((HWND)lParam == g_hPlList)
        {
            int row = (int)SendMessageW(g_hPlList, LB_GETCURSEL, 0, 0);
            if (LOWORD(wParam) == VK_DELETE) { PlRemoveRow(row); *res = -2; return true; }
            if (LOWORD(wParam) == VK_RETURN && row >= 0) { PlPreviewPlay(g_plView[row]); PlApplyLanguage(); InvalidateRect(hwnd, nullptr, FALSE); *res = -2; return true; }
            *res = -1; return true;
        }
        return false;
    case WM_HSCROLL:
        if ((HWND)lParam == g_hPlVolume)
        {
            g_config.musicVolume = (int)SendMessageW(g_hPlVolume, TBM_GETPOS, 0, 0);
            PlPreviewVolume();
            RECT r = { UI_PANEL_X, 218, UI_PANEL_X + UI_PANEL_W, 240 }; InvalidateRect(hwnd, &r, FALSE);
            if (LOWORD(wParam) == TB_ENDTRACK || LOWORD(wParam) == TB_THUMBPOSITION) SaveIni();
            *res = 0; return true;
        }
        return false;
    case WM_NOTIFY:
    {
        NMHDR *nh = (NMHDR *)lParam;
        if (nh->hwndFrom == g_hPlVolume && nh->code == NM_CUSTOMDRAW) { *res = PlVolumeCustomDraw((NMCUSTOMDRAW *)lParam); return true; }
        return false;
    }
    case WM_TIMER:
        if (wParam == PL_TIMER_ID) { RECT r = { UI_PANEL_X, 395, UI_PANEL_X + UI_PANEL_W, 450 }; InvalidateRect(hwnd, &r, FALSE); *res = 0; return true; }
        return false;
    case WM_PL_PREVIEW_EVENT:
        if (wParam == MF_MEDIA_ENGINE_EVENT_ENDED) PlPreviewNext();
        else if (wParam == MF_MEDIA_ENGINE_EVENT_ERROR) PlPreviewStop();
        PlApplyLanguage();
        InvalidateRect(g_hPlList, nullptr, FALSE);
        InvalidateRect(hwnd, nullptr, FALSE);
        *res = 0; return true;
    case WM_COMMAND:
    {
        int id = LOWORD(wParam), code = HIWORD(wParam);
        switch (id)
        {
        case ID_TAB_HOME: PlSetTab(hwnd, 0); *res = 0; return true;
        case ID_TAB_SETTINGS: PlSetTab(hwnd, 2); *res = 0; return true;
        case ID_TAB_PLAYLIST: PlSetTab(hwnd, 1); *res = 0; return true;
        case ID_TAB_SETUP: PlSetTab(hwnd, 3); *res = 0; return true;
        case IDOK: if (g_plEditing) { PlEndEdit(true); *res = 0; return true; } return false;
        case IDCANCEL: if (g_plEditing) { PlEndEdit(false); *res = 0; return true; } return false;
        case ID_PL_NAMEEDIT: if (code == EN_KILLFOCUS && g_plEditing) PlEndEdit(true); *res = 0; return true;
        case ID_PL_COMBO:
            if (code == CBN_SELCHANGE && !g_plNames.empty())
            {
                PlPreviewStop();
                PlSelect((int)SendMessageW(g_hPlCombo, CB_GETCURSEL, 0, 0));
                SaveIni();
            }
            *res = 0; return true;
        case ID_PL_NEW: PlBeginEdit(true); *res = 0; return true;
        case ID_PL_RENAME: if (g_plCur >= 0) PlBeginEdit(false); *res = 0; return true;
        case ID_PL_DELETE: PlDeleteCurrent(hwnd); *res = 0; return true;
        case ID_PL_ENABLE:
            g_config.musicEnabled = !g_config.musicEnabled; InvalidateRect((HWND)lParam, nullptr, TRUE); SaveIni(); *res = 0; return true;
        case ID_PL_SHUFFLE:
            g_config.musicShuffle = !g_config.musicShuffle; InvalidateRect((HWND)lParam, nullptr, TRUE); SaveIni(); *res = 0; return true;
        case ID_PL_ADDFILES: PlPickFiles(hwnd, false); *res = 0; return true;
        case ID_PL_ADDFOLDER: PlPickFiles(hwnd, true); *res = 0; return true;
        case ID_PL_REMOVE: PlRemoveRow((int)SendMessageW(g_hPlList, LB_GETCURSEL, 0, 0)); *res = 0; return true;
        case ID_PL_SEARCH: if (code == EN_CHANGE) PlRefreshList(); *res = 0; return true;
        case ID_PL_LIST:
            if (code == LBN_DBLCLK)
            {
                int row = (int)SendMessageW(g_hPlList, LB_GETCURSEL, 0, 0);
                if (row >= 0) PlPreviewPlay(g_plView[row]);
                PlApplyLanguage(); InvalidateRect(g_hPlList, nullptr, FALSE); InvalidateRect(hwnd, nullptr, FALSE);
            }
            *res = 0; return true;
        case ID_PL_PLAY:
            if (g_plPreviewPath.empty())
            {
                int row = (int)SendMessageW(g_hPlList, LB_GETCURSEL, 0, 0);
                PlPreviewPlay(row >= 0 ? g_plView[row] : (g_plView.empty() ? -1 : g_plView[0]));
            }
            else if (g_plPreviewPaused) { g_plEngine->Play(); g_plPreviewPaused = false; }
            else { g_plEngine->Pause(); g_plPreviewPaused = true; }
            PlApplyLanguage(); InvalidateRect(g_hPlList, nullptr, FALSE); InvalidateRect(hwnd, nullptr, FALSE);
            *res = 0; return true;
        case ID_PL_STOP: PlPreviewStop(); PlApplyLanguage(); InvalidateRect(g_hPlList, nullptr, FALSE); InvalidateRect(hwnd, nullptr, FALSE); *res = 0; return true;
        case ID_PL_NEXT: PlPreviewNext(); PlApplyLanguage(); InvalidateRect(g_hPlList, nullptr, FALSE); InvalidateRect(hwnd, nullptr, FALSE); *res = 0; return true;
        }
        return false;
    }
    }
    return false;
}

static void PlShutdown()
{
    if (g_plEngine) { g_plEngine->Shutdown(); g_plEngine->Release(); g_plEngine = nullptr; MFShutdown(); }
}
