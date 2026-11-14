// ============================================================================
// "Perfis" tab: the saved game profiles. Each profile is one folder in the save folder
// (<save>\<name>, name encoded like the bridge's connected storage: CsEncode in
// src/d3d12_x/connected_storage.inc) and its name is also the in-game player name.
// Select / create / rename / delete (to the Recycle Bin), plus two per-profile switches,
// applied only while that profile plays (nothing is written into the save):
//   "Desbloquear cosméticos" -> -giveentitlements -unlockcosmetics (bridge forces the item checks)
//   "Desbloquear tudo"       -> -giveentitlements -unlockall (game's own switch: items, events, videos)
// ============================================================================
#define ID_PROF_LIST     1070
#define ID_PROF_NAME     1071
#define ID_PROF_SELECT   1072
#define ID_PROF_NEW      1073
#define ID_PROF_RENAME   1074
#define ID_PROF_DELETE   1075
#define ID_PROF_UNLOCK   1076
#define ID_PROF_UNLOCKALL 1077

static HWND g_hProfList = nullptr, g_hProfName = nullptr, g_hProfSelect = nullptr, g_hProfNew = nullptr,
            g_hProfRename = nullptr, g_hProfDelete = nullptr, g_hProfUnlock = nullptr,
            g_hProfUnlockAll = nullptr;
static std::vector<std::wstring> g_profNames;

enum ProfStr {
    R_TITLE, R_HINT, R_NAME, R_SELECT, R_NEW, R_RENAME, R_DELETE, R_UNLOCK, R_UNLOCKALL, R_UNLOCK_HINT, R_CURRENT,
    R_EXISTS, R_EMPTY, R_CONFIRM_DEL, R_RUNNING, R_FAIL, R_PICK, R_COUNT
};
static const wchar_t *kProfStr[R_COUNT][3] = {
    { L"PERFIS", L"PROFILES", L"PERFILES" },
    { L"Cada perfil tem o seu próprio save. O nome do perfil é o nome do jogador no jogo.",
      L"Each profile has its own save. The profile name is the player name in game.",
      L"Cada perfil tiene su propia partida. El nombre del perfil es el nombre del jugador en el juego." },
    { L"Nome do perfil", L"Profile name", L"Nombre del perfil" },
    { L"Usar", L"Use", L"Usar" },
    { L"Criar", L"Create", L"Crear" },
    { L"Renomear", L"Rename", L"Renombrar" },
    { L"Excluir", L"Delete", L"Eliminar" },
    { L"Desbloquear cosméticos", L"Unlock cosmetics", L"Desbloquear cosméticos" },
    { L"Desbloquear tudo (cosméticos e eventos)", L"Unlock everything (cosmetics and events)", L"Desbloquear todo (cosméticos y eventos)" },
    { L"Vale só para o perfil selecionado na lista. Desligue para voltar ao progresso salvo; o save não é alterado.",
      L"Applies only to the profile selected in the list. Turn it off to go back to the saved progress; the save is not changed.",
      L"Solo para el perfil seleccionado en la lista. Desactívalo para volver al progreso guardado; la partida no se modifica." },
    { L"(em uso)", L"(in use)", L"(en uso)" },
    { L"Já existe um perfil com esse nome.", L"A profile with that name already exists.", L"Ya existe un perfil con ese nombre." },
    { L"Digite um nome para o perfil.", L"Type a name for the profile.", L"Escribe un nombre para el perfil." },
    { L"Excluir o perfil \"%ls\"?\n\nA pasta do save vai para a Lixeira (dá para restaurar de lá).",
      L"Delete the profile \"%ls\"?\n\nIts save folder goes to the Recycle Bin (it can be restored from there).",
      L"¿Eliminar el perfil \"%ls\"?\n\nLa carpeta de la partida va a la Papelera (se puede restaurar desde allí)." },
    { L"Feche o jogo antes de mudar os perfis.", L"Close the game before changing profiles.", L"Cierra el juego antes de cambiar los perfiles." },
    { L"Não foi possível concluir (erro %lu).", L"Could not finish (error %lu).", L"No se pudo completar (error %lu)." },
    { L"Selecione um perfil na lista.", L"Select a profile in the list.", L"Selecciona un perfil en la lista." },
};
static const wchar_t *RT(ProfStr s) { return kProfStr[s][g_uiLang]; }

// same rules as CsEncode (bridge): file-name safe and reversible
static std::wstring ProfEncode(const std::wstring &n)
{
    std::wstring o; wchar_t b[8];
    for (size_t i = 0; i < n.size(); ++i)
    {
        wchar_t c = n[i]; bool last = i + 1 == n.size();
        if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'_' || c == L'-' ||
            ((c == L'.' || c == L' ') && !last) || (c >= 0xC0 && c < 0xD800))
            o += c;
        else { swprintf_s(b, L"%%%04X", (unsigned)c); o += b; }
    }
    return o.empty() ? L"%0000" : o;
}
static std::wstring ProfDecode(const std::wstring &n)
{
    std::wstring o;
    for (size_t i = 0; i < n.size(); ++i)
    {
        if (n[i] == L'%' && i + 4 < n.size()) { o += (wchar_t)wcstoul(n.substr(i + 1, 4).c_str(), nullptr, 16); i += 4; }
        else o += n[i];
    }
    return o;
}

// The save folder as the game will see it (ONRUSH_SAVE_DIR rules in OnRushLauncher.cpp).
static std::wstring ProfSaveRoot()
{
    wchar_t savePath[MAX_PATH] = {}, full[MAX_PATH] = {};
    if (g_hSaveEdit) GetWindowTextW(g_hSaveEdit, savePath, MAX_PATH);
    if (!savePath[0]) wcscpy_s(savePath, L".\\save");
    if (PathIsRelativeW(savePath)) PathCombineW(full, g_gameDirShown[0] ? g_gameDirShown : g_exeDir, savePath);
    else wcscpy_s(full, MAX_PATH, savePath);
    std::wstring r = full;
    while (!r.empty() && r.back() == L'\\') r.pop_back();
    return r;
}

// Same clean-up as the old "Jogador" field: no characters Windows can't use, trimmed, max 20.
static std::wstring ProfCleanName(const wchar_t *in)
{
    std::wstring s;
    for (const wchar_t *p = in; *p && s.size() < 20; ++p)
        if (*p >= 32 && !wcschr(L"<>:\"/\\|?*", *p)) s += *p;
    while (!s.empty() && (s.back() == L' ' || s.back() == L'.')) s.pop_back();
    size_t a = 0; while (a < s.size() && s[a] == L' ') ++a;
    return s.substr(a);
}

static std::wstring ProfIniPath() { wchar_t p[MAX_PATH]; swprintf_s(p, MAX_PATH, L"%ls\\onrush.ini", g_exeDir); return p; }
static bool ProfFlag(const wchar_t *section, const std::wstring &name)
{
    return GetPrivateProfileIntW(section, ProfEncode(name).c_str(), 0, ProfIniPath().c_str()) != 0;
}
static void ProfSetFlag(const wchar_t *section, const std::wstring &name, bool on)
{
    WritePrivateProfileStringW(section, ProfEncode(name).c_str(), on ? L"1" : nullptr, ProfIniPath().c_str());
}
static bool ProfUnlocked(const std::wstring &name) { return ProfFlag(L"CosmeticsUnlocked", name); }
static bool ProfAllUnlocked(const std::wstring &name) { return ProfFlag(L"AllUnlocked", name); }
// the switches of a renamed / deleted profile
static void ProfMoveFlags(const std::wstring &from, const std::wstring &to)
{
    for (const wchar_t *sec : { L"CosmeticsUnlocked", L"AllUnlocked" })
    {
        bool on = ProfFlag(sec, from);
        ProfSetFlag(sec, from, false);
        if (!to.empty()) ProfSetFlag(sec, to, on);
    }
}

// Play time per profile, counted by the launcher (game start -> exit, warm-up runs excluded): [PlayTime] in onrush.ini.
static ULONGLONG ProfPlaySeconds(const std::wstring &name)
{
    wchar_t v[32] = {};
    GetPrivateProfileStringW(L"PlayTime", ProfEncode(name).c_str(), L"0", v, 32, ProfIniPath().c_str());
    return _wcstoui64(v, nullptr, 10);
}
static void ProfAddPlaySeconds(const std::wstring &name, ULONGLONG secs)
{
    wchar_t v[32]; swprintf_s(v, L"%llu", ProfPlaySeconds(name) + secs);
    WritePrivateProfileStringW(L"PlayTime", ProfEncode(name).c_str(), v, ProfIniPath().c_str());
}
static std::wstring ProfFmtPlay(ULONGLONG s)
{
    wchar_t t[48];
    if (s < 3600) swprintf_s(t, L"%llu min", s / 60);
    else swprintf_s(t, L"%llu h %02llu min", s / 3600, (s / 60) % 60);
    return t;
}
static ULONGLONG g_playStartTick = 0;
static std::wstring g_playProfile;

static int ProfSelectedRow()
{
    int r = g_hProfList ? (int)SendMessageW(g_hProfList, LB_GETCURSEL, 0, 0) : -1;
    return (r >= 0 && r < (int)g_profNames.size()) ? r : -1;
}

static void ProfRefresh(const std::wstring &keep)
{
    g_profNames.clear();
    std::wstring root = ProfSaveRoot();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.' || fd.cFileName[0] == L'_') continue;
            g_profNames.push_back(ProfDecode(fd.cFileName));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    // the profile in use always shows up, even before the game created its folder
    bool found = false;
    for (auto &n : g_profNames) if (!_wcsicmp(n.c_str(), g_config.playerName)) found = true;
    if (!found) g_profNames.push_back(g_config.playerName);
    std::sort(g_profNames.begin(), g_profNames.end(), [](const std::wstring &a, const std::wstring &b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    if (!g_hProfList) return;
    SendMessageW(g_hProfList, LB_RESETCONTENT, 0, 0);
    int sel = -1;
    for (size_t i = 0; i < g_profNames.size(); ++i)
    {
        std::wstring row = g_profNames[i];
        if (!_wcsicmp(row.c_str(), g_config.playerName)) row += std::wstring(L"   ") + RT(R_CURRENT);
        if (ProfAllUnlocked(g_profNames[i])) row += L"   ★★";
        else if (ProfUnlocked(g_profNames[i])) row += L"   ★";
        row += L"   ·  " + ProfFmtPlay(ProfPlaySeconds(g_profNames[i]));
        SendMessageW(g_hProfList, LB_ADDSTRING, 0, (LPARAM)row.c_str());
        if (!_wcsicmp(g_profNames[i].c_str(), (keep.empty() ? std::wstring(g_config.playerName) : keep).c_str())) sel = (int)i;
    }
    SendMessageW(g_hProfList, LB_SETCURSEL, sel, 0);
    if (sel >= 0) SetWindowTextW(g_hProfName, g_profNames[sel].c_str());
    InvalidateRect(g_hProfUnlock, nullptr, TRUE);
    InvalidateRect(g_hProfUnlockAll, nullptr, TRUE);
}

static void ProfUse(const std::wstring &name)
{
    wcscpy_s(g_config.playerName, 32, name.c_str());
    if (g_hPlayerEdit) SetWindowTextW(g_hPlayerEdit, name.c_str());  // ReadUiIntoConfig reads the name from there
    SaveIni();
    LogPrintf(L"[Perfis] perfil em uso: %ls\r\n", name.c_str());
}

static bool ProfGameRunning(HWND hwnd)
{
    if (!g_gameRunning) return false;
    MessageBoxW(hwnd, RT(R_RUNNING), L"ONRUSH", MB_ICONINFORMATION);
    return true;
}

static void ProfFail(HWND hwnd, DWORD err)
{
    wchar_t m[256]; swprintf_s(m, RT(R_FAIL), err);
    MessageBoxW(hwnd, m, L"ONRUSH", MB_ICONWARNING);
}

static void ProfCommand(HWND hwnd, int id)
{
    wchar_t typed[64] = {};
    GetWindowTextW(g_hProfName, typed, 64);
    std::wstring name = ProfCleanName(typed), root = ProfSaveRoot();
    int row = ProfSelectedRow();
    auto exists = [&](const std::wstring &n) {
        for (auto &p : g_profNames) if (!_wcsicmp(p.c_str(), n.c_str())) return true;
        return false; };
    switch (id)
    {
    case ID_PROF_SELECT:
        if (row < 0) { MessageBoxW(hwnd, RT(R_PICK), L"ONRUSH", MB_ICONINFORMATION); return; }
        if (ProfGameRunning(hwnd)) return;
        ProfUse(g_profNames[row]);
        ProfRefresh(g_profNames[row]);
        return;
    case ID_PROF_NEW:
        if (ProfGameRunning(hwnd)) return;
        if (name.empty()) { MessageBoxW(hwnd, RT(R_EMPTY), L"ONRUSH", MB_ICONINFORMATION); return; }
        if (exists(name)) { MessageBoxW(hwnd, RT(R_EXISTS), L"ONRUSH", MB_ICONINFORMATION); return; }
        SHCreateDirectoryExW(nullptr, root.c_str(), nullptr);
        if (!CreateDirectoryW((root + L"\\" + ProfEncode(name)).c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) { ProfFail(hwnd, GetLastError()); return; }
        LogPrintf(L"[Perfis] perfil criado: %ls\r\n", name.c_str());
        ProfUse(name);
        ProfRefresh(name);
        return;
    case ID_PROF_RENAME:
    {
        if (row < 0) { MessageBoxW(hwnd, RT(R_PICK), L"ONRUSH", MB_ICONINFORMATION); return; }
        if (ProfGameRunning(hwnd)) return;
        std::wstring old = g_profNames[row];
        if (name.empty()) { MessageBoxW(hwnd, RT(R_EMPTY), L"ONRUSH", MB_ICONINFORMATION); return; }
        if (name == old) return;
        if (_wcsicmp(name.c_str(), old.c_str()) && exists(name)) { MessageBoxW(hwnd, RT(R_EXISTS), L"ONRUSH", MB_ICONINFORMATION); return; }
        std::wstring from = root + L"\\" + ProfEncode(old), to = root + L"\\" + ProfEncode(name);
        if (GetFileAttributesW(from.c_str()) != INVALID_FILE_ATTRIBUTES && !MoveFileW(from.c_str(), to.c_str())) { ProfFail(hwnd, GetLastError()); return; }
        ProfMoveFlags(old, name);
        ULONGLONG played = ProfPlaySeconds(old);  // play time follows the profile
        WritePrivateProfileStringW(L"PlayTime", ProfEncode(old).c_str(), nullptr, ProfIniPath().c_str());
        if (played) ProfAddPlaySeconds(name, played);
        LogPrintf(L"[Perfis] perfil renomeado: %ls -> %ls\r\n", old.c_str(), name.c_str());
        if (!_wcsicmp(old.c_str(), g_config.playerName)) ProfUse(name);
        ProfRefresh(name);
        return;
    }
    case ID_PROF_DELETE:
    {
        if (row < 0) { MessageBoxW(hwnd, RT(R_PICK), L"ONRUSH", MB_ICONINFORMATION); return; }
        if (ProfGameRunning(hwnd)) return;
        std::wstring victim = g_profNames[row];
        wchar_t m[512]; swprintf_s(m, RT(R_CONFIRM_DEL), victim.c_str());
        if (MessageBoxW(hwnd, m, L"ONRUSH", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES) return;
        std::wstring dir = root + L"\\" + ProfEncode(victim);
        if (GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            std::wstring from = dir; from.push_back(L'\0');  // SHFileOperation wants a double-NUL list
            SHFILEOPSTRUCTW op = {};
            op.hwnd = hwnd; op.wFunc = FO_DELETE; op.pFrom = from.c_str();
            op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;  // Recycle Bin, the launcher already asked
            int r = SHFileOperationW(&op);
            if (r || op.fAnyOperationsAborted) { ProfFail(hwnd, (DWORD)r); return; }
        }
        ProfMoveFlags(victim, L"");
        WritePrivateProfileStringW(L"PlayTime", ProfEncode(victim).c_str(), nullptr, ProfIniPath().c_str());
        LogPrintf(L"[Perfis] perfil excluído (Lixeira): %ls\r\n", victim.c_str());
        if (!_wcsicmp(victim.c_str(), g_config.playerName))
        {   // the profile in use is gone: use another one (or a new default)
            std::wstring next = L"Player 1";
            for (auto &n : g_profNames) if (_wcsicmp(n.c_str(), victim.c_str())) { next = n; break; }
            ProfUse(next);
        }
        ProfRefresh(L"");
        return;
    }
    case ID_PROF_UNLOCK:
    case ID_PROF_UNLOCKALL:
    {
        if (row < 0) { MessageBoxW(hwnd, RT(R_PICK), L"ONRUSH", MB_ICONINFORMATION); return; }
        const wchar_t *sec = id == ID_PROF_UNLOCK ? L"CosmeticsUnlocked" : L"AllUnlocked";
        ProfSetFlag(sec, g_profNames[row], !ProfFlag(sec, g_profNames[row]));
        LogPrintf(L"[Perfis] %ls: %ls para %ls\r\n", id == ID_PROF_UNLOCK ? L"desbloquear cosméticos" : L"desbloquear tudo",
                  ProfFlag(sec, g_profNames[row]) ? L"ligado" : L"desligado", g_profNames[row].c_str());
        ProfRefresh(g_profNames[row]);
        return;
    }
    }
}

// state of the unlock switch (DrawOwnerButton)
static bool ProfUnlockSwitchOn(int id)
{
    int row = ProfSelectedRow();
    return row >= 0 && (id == ID_PROF_UNLOCK ? ProfUnlocked(g_profNames[row]) : ProfAllUnlocked(g_profNames[row]));
}

// Profile rows drawn in the theme colours (the system list highlight is a bright blue).
static void ProfDrawListItem(const DRAWITEMSTRUCT *di)
{
    HDC hdc = di->hDC;
    RECT r = di->rcItem;
    HBRUSH bg = CreateSolidBrush(COLOR_BG_INPUT); FillRect(hdc, &r, bg); DeleteObject(bg);
    if ((int)di->itemID < 0) return;
    bool sel = (di->itemState & ODS_SELECTED) != 0;
    if (sel)
    {
        RECT rr = { r.left + 4, r.top + 2, r.right - 4, r.bottom - 2 };
        FillRound(hdc, rr, 8, g_pal.comboSel, g_pal.comboSel);
        RECT bar = { r.left + 4, r.top + 8, r.left + 7, r.bottom - 8 };
        HBRUSH ab = CreateSolidBrush(COLOR_ACCENT_ORANGE); FillRect(hdc, &bar, ab); DeleteObject(ab);
    }
    wchar_t text[160] = {};
    SendMessageW(di->hwndItem, LB_GETTEXT, di->itemID, (LPARAM)text);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, COLOR_TEXT_PRIMARY);
    HGDIOBJ of = SelectObject(hdc, sel ? g_hFontUIBold : g_hFontUI);
    RECT tr = { r.left + 16, r.top, r.right - 10, r.bottom };
    DrawTextW(hdc, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(hdc, of);
}

static void ProfCreateControls(HWND hwnd)
{
    const int X = UI_PANEL_X, W = UI_PANEL_W;
    DWORD od = WS_CHILD | WS_TABSTOP | BS_OWNERDRAW;
    g_hProfList = CreateWindowW(L"LISTBOX", L"", WS_CHILD | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
                                X, 104, W, 190, hwnd, (HMENU)ID_PROF_LIST, nullptr, nullptr);
    SetWindowTheme(g_hProfList, g_pal.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    g_hProfName = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, X + 10, 333, W - 20, 20, hwnd, (HMENU)ID_PROF_NAME, nullptr, nullptr);
    SendMessageW(g_hProfName, EM_LIMITTEXT, 20, 0);
    int bw = (W - 24) / 4;
    g_hProfSelect = CreateWindowW(L"BUTTON", L"", od, X, 372, bw, 30, hwnd, (HMENU)ID_PROF_SELECT, nullptr, nullptr);
    g_hProfNew = CreateWindowW(L"BUTTON", L"", od, X + (bw + 8), 372, bw, 30, hwnd, (HMENU)ID_PROF_NEW, nullptr, nullptr);
    g_hProfRename = CreateWindowW(L"BUTTON", L"", od, X + 2 * (bw + 8), 372, bw, 30, hwnd, (HMENU)ID_PROF_RENAME, nullptr, nullptr);
    g_hProfDelete = CreateWindowW(L"BUTTON", L"", od, X + 3 * (bw + 8), 372, bw, 30, hwnd, (HMENU)ID_PROF_DELETE, nullptr, nullptr);
    g_hProfUnlock = CreateWindowW(L"BUTTON", L"", od, X, 420, W, 24, hwnd, (HMENU)ID_PROF_UNLOCK, nullptr, nullptr);
    g_hProfUnlockAll = CreateWindowW(L"BUTTON", L"", od, X, 452, W, 24, hwnd, (HMENU)ID_PROF_UNLOCKALL, nullptr, nullptr);
    ProfRefresh(L"");
}

static void ProfApplyLanguage()
{
    SetWindowTextW(g_hProfSelect, RT(R_SELECT));
    SetWindowTextW(g_hProfNew, RT(R_NEW));
    SetWindowTextW(g_hProfRename, RT(R_RENAME));
    SetWindowTextW(g_hProfDelete, RT(R_DELETE));
    SetWindowTextW(g_hProfUnlock, RT(R_UNLOCK));
    SetWindowTextW(g_hProfUnlockAll, RT(R_UNLOCKALL));
    ProfRefresh(ProfSelectedRow() >= 0 ? g_profNames[ProfSelectedRow()] : L"");
}

static void ProfUpdateVisibility(HWND hwnd)
{
    int s = g_tab == 3 ? SW_SHOW : SW_HIDE;
    HWND c[] = { g_hProfList, g_hProfName, g_hProfSelect, g_hProfNew, g_hProfRename, g_hProfDelete, g_hProfUnlock, g_hProfUnlockAll };
    for (HWND h : c) if (h) ShowWindow(h, s);
    if (g_tab == 3) ProfRefresh(ProfSelectedRow() >= 0 ? g_profNames[ProfSelectedRow()] : L"");
}

static void ProfDrawPanel(HDC hdc)
{
    const int X = UI_PANEL_X, W = UI_PANEL_W;
    SelectObject(hdc, g_hFontPlay); SetTextColor(hdc, COLOR_TEXT_PRIMARY);
    TextOutW(hdc, X, 20, RT(R_TITLE), (int)wcslen(RT(R_TITLE)));
    SelectObject(hdc, g_hFontUI); SetTextColor(hdc, COLOR_TEXT_MUTED);
    RECT hint = { X, 54, X + W, 100 };
    DrawTextW(hdc, RT(R_HINT), -1, &hint, DT_LEFT | DT_WORDBREAK);
    SetTextColor(hdc, COLOR_TEXT_PRIMARY);
    TextOutW(hdc, X, 306, RT(R_NAME), (int)wcslen(RT(R_NAME)));
    RECT box = { X, 326, X + W, 358 };
    FillRound(hdc, box, 8, COLOR_BG_INPUT, COLOR_BORDER);
    SetTextColor(hdc, COLOR_TEXT_MUTED);
    RECT uh = { X, 484, X + W, 540 };
    DrawTextW(hdc, RT(R_UNLOCK_HINT), -1, &uh, DT_LEFT | DT_WORDBREAK);
}

// WM_COMMAND for the tab's controls; true when handled
static bool ProfHandleCommand(HWND hwnd, int id, int code)
{
    switch (id)
    {
    case ID_PROF_LIST:
        if (code == LBN_SELCHANGE)
        {
            int row = ProfSelectedRow();
            if (row >= 0) SetWindowTextW(g_hProfName, g_profNames[row].c_str());
            InvalidateRect(g_hProfUnlock, nullptr, TRUE);
            InvalidateRect(g_hProfUnlockAll, nullptr, TRUE);
        }
        else if (code == LBN_DBLCLK) ProfCommand(hwnd, ID_PROF_SELECT);
        return true;
    case ID_PROF_NAME: return true;
    case ID_PROF_SELECT: case ID_PROF_NEW: case ID_PROF_RENAME: case ID_PROF_DELETE: case ID_PROF_UNLOCK: case ID_PROF_UNLOCKALL:
        ProfCommand(hwnd, id);
        return true;
    }
    return false;
}
