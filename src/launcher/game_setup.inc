// ============================================================================
// Game folder setup: turns the launcher's own folder (e.g. the extracted release zip,
// which has the port DLLs) into a playable game folder from the player's copy of ONRUSH.
//  - copies the game executable/metadata from <copy>\Mount and the console system
//    libraries from <copy>\EmbeddedXvd\Windows\System32
//  - links the big folders (Mount\dat, EmbeddedXvd) as NTFS junctions (nothing duplicated)
//  - writes onrush-pc.exe: a copy of game_release_xbox.exe whose version resource says
//    "ONRUSH(TM)" (what Task Manager shows). The player's copy is never modified.
// ============================================================================
#include <winioctl.h>
#include <string>

static const wchar_t *kPortFiles[] = { L"d3d12_x.dll", L"kernelx.dll", L"combase.dll", L"EtwPlus.dll", L"MMDevAPI.dll" };
static const wchar_t *kGameFiles[] = { L"game_release_xbox.exe", L"appxmanifest.xml", L"XboxIntegratedMultiplayer.dll", L"Bink2XboxOne.dll",
                                       L"appdata.bin", L"Catalog000.bin", L"resources.pri", L"saveversions.txt", L"SplashScreen.png" };
// (not the console D3DCompiler_47.dll: the port uses the Windows one, and a copy here would
// shadow it for anything loading it by name, e.g. RenderDoc)
static const wchar_t *kSystemFiles[] = { L"AcpHal.dll", L"ACP_kernel.bin", L"xg_x.dll", L"xaudio2_9.dll" };

static bool FileExistsIn(const wchar_t *dir, const wchar_t *name)
{
    wchar_t p[MAX_PATH];
    swprintf_s(p, MAX_PATH, L"%ls\\%ls", dir, name);
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES;
}

// A folder the launcher can start the game from: game executable + the port's D3D12 bridge.
static bool GameDirIsReady(const wchar_t *dir)
{
    return dir && dir[0] && (FileExistsIn(dir, L"onrush-pc.exe") || FileExistsIn(dir, L"game_release_xbox.exe")) && FileExistsIn(dir, L"d3d12_x.dll");
}

// Finds a copy of the game from the folder the user picked: the extracted package root
// (with Mount\ and EmbeddedXvd\) or its Mount folder. Fills the Mount and package root paths.
static bool FindGameCopy(const wchar_t *dir, wchar_t *mount, wchar_t *root)
{
    wchar_t full[MAX_PATH], p[MAX_PATH];
    if (!dir || !dir[0] || !GetFullPathNameW(dir, MAX_PATH, full, nullptr)) return false;
    size_t n = wcslen(full);
    while (n > 3 && full[n - 1] == L'\\') full[--n] = 0;
    swprintf_s(p, MAX_PATH, L"%ls\\Mount", full);
    if (FileExistsIn(p, L"game_release_xbox.exe")) { wcscpy_s(mount, MAX_PATH, p); wcscpy_s(root, MAX_PATH, full); return true; }
    if (FileExistsIn(full, L"game_release_xbox.exe"))
    {
        wcscpy_s(mount, MAX_PATH, full);
        wcscpy_s(root, MAX_PATH, full);
        wchar_t parent[MAX_PATH]; wcscpy_s(parent, MAX_PATH, full);
        PathRemoveFileSpecW(parent);
        swprintf_s(p, MAX_PATH, L"%ls\\EmbeddedXvd", parent);
        if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) wcscpy_s(root, MAX_PATH, parent);
        return true;
    }
    return false;
}

// The player's copy a game folder runs from. A folder prepared by the launcher has a "dat" junction
// to <copy>\Mount\dat; otherwise the folder is the copy's Mount itself.
static bool SourceCopyOf(const wchar_t *gameDir, wchar_t *mount, wchar_t *root)
{
    wchar_t dat[MAX_PATH], fin[MAX_PATH];
    swprintf_s(dat, MAX_PATH, L"%ls\\dat", gameDir);
    DWORD a = GetFileAttributesW(dat);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_REPARSE_POINT)) return FindGameCopy(gameDir, mount, root);
    HANDLE h = CreateFileW(dat, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD n = GetFinalPathNameByHandleW(h, fin, MAX_PATH, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    if (!n || n >= MAX_PATH) return false;
    std::wstring p = fin;
    if (!p.compare(0, 8, L"\\\\?\\UNC\\")) p = L"\\\\" + p.substr(8);
    else if (!p.compare(0, 4, L"\\\\?\\")) p = p.substr(4);
    wcscpy_s(fin, MAX_PATH, p.c_str());
    PathRemoveFileSpecW(fin);  // <copy>\Mount\dat -> <copy>\Mount
    return FindGameCopy(fin, mount, root);
}

static bool PortFilesNextToLauncher()
{
    for (const wchar_t *f : kPortFiles) if (!FileExistsIn(g_exeDir, f)) return false;
    return true;
}

// NTFS junction (mount point) link -> target; no admin rights needed.
static bool CreateJunction(const wchar_t *link, const wchar_t *target)
{
    if (!CreateDirectoryW(link, nullptr)) return false;
    HANDLE h = CreateFileW(link, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) { RemoveDirectoryW(link); return false; }
    std::wstring sub = std::wstring(L"\\??\\") + target, print = target;
    WORD subB = (WORD)(sub.size() * 2), prB = (WORD)(print.size() * 2);
    WORD pathLen = (WORD)(subB + 2 + prB + 2);
    std::vector<BYTE> buf(16 + pathLen, 0);
    *(DWORD *)&buf[0] = IO_REPARSE_TAG_MOUNT_POINT;
    *(WORD *)&buf[4] = (WORD)(8 + pathLen);            // ReparseDataLength
    WORD *m = (WORD *)&buf[8];
    m[0] = 0; m[1] = subB; m[2] = (WORD)(subB + 2); m[3] = prB;  // substitute name, print name
    memcpy(&buf[16], sub.c_str(), subB);
    memcpy(&buf[16 + subB + 2], print.c_str(), prB);
    DWORD ret = 0;
    BOOL ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf.data(), (DWORD)buf.size(), nullptr, 0, &ret, nullptr);
    CloseHandle(h);
    if (!ok) RemoveDirectoryW(link);
    return ok != FALSE;
}

// Where a junction points (its print name), even when the target no longer exists. "" if not a junction.
static std::wstring JunctionTarget(const wchar_t *link)
{
    HANDLE h = CreateFileW(link, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    std::vector<BYTE> buf(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
    DWORD got = 0;
    BOOL ok = DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, nullptr, 0, buf.data(), (DWORD)buf.size(), &got, nullptr);
    CloseHandle(h);
    if (!ok || got < 16 || *(DWORD *)&buf[0] != IO_REPARSE_TAG_MOUNT_POINT) return L"";
    const WORD *m = (const WORD *)&buf[8];  // substitute offset/length, print offset/length (bytes)
    if (16u + m[2] + m[3] <= got && m[3])
        return std::wstring((const wchar_t *)&buf[16 + m[2]], m[3] / 2);
    std::wstring sub((const wchar_t *)&buf[16 + m[0]], m[1] / 2);
    return sub.compare(0, 4, L"\\??\\") ? sub : sub.substr(4);
}

// The game's packs can be read from the folder (the dat junction leads to a copy that still exists).
static bool GameAssetsReachable(const wchar_t *dir)
{
    return FileExistsIn(dir, L"dat\\index\\dat.ndx");
}

// ---- VS_VERSIONINFO builder (same layout as scripts/brand_exe.py) ---------------------------
static void VerPad(std::vector<BYTE> &b) { while (b.size() % 4) b.push_back(0); }
static std::vector<BYTE> VerNode(const wchar_t *key, const std::vector<BYTE> &value, WORD type, const std::vector<BYTE> &children, int valueLen = -1)
{
    std::vector<BYTE> out(6, 0);
    const BYTE *k = (const BYTE *)key;
    out.insert(out.end(), k, k + (wcslen(key) + 1) * 2);
    VerPad(out);
    out.insert(out.end(), value.begin(), value.end());
    VerPad(out);
    out.insert(out.end(), children.begin(), children.end());
    WORD len = (WORD)out.size();
    WORD vlen = (WORD)(valueLen >= 0 ? valueLen : (type == 1 ? value.size() / 2 : value.size()));
    memcpy(&out[0], &len, 2); memcpy(&out[2], &vlen, 2); memcpy(&out[4], &type, 2);
    VerPad(out);
    return out;
}
static std::vector<BYTE> VerString(const wchar_t *s)
{
    const BYTE *b = (const BYTE *)s;
    return std::vector<BYTE>(b, b + (wcslen(s) + 1) * 2);
}

// The launcher's own icon group (resource 1 in launcher.rc), as stored in the module.
static const BYTE *OwnIconGroup(DWORD *size)
{
    HMODULE self = GetModuleHandleW(nullptr);
    HRSRC r = FindResourceW(self, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(14) /* RT_GROUP_ICON */);
    HGLOBAL g = r ? LoadResource(self, r) : nullptr;
    *size = r ? SizeofResource(self, r) : 0;
    return g && *size >= 6 ? (const BYTE *)LockResource(g) : nullptr;
}

// Writes the launcher's icon (RT_GROUP_ICON 1 + its RT_ICON images) into the exe being updated.
// The game exe ships no icon, so these are new entries.
static bool CopyOwnIcon(HANDLE h)
{
    DWORD gsz = 0; const BYTE *grp = OwnIconGroup(&gsz);
    if (!grp) return false;
    HMODULE self = GetModuleHandleW(nullptr);
    const WORD count = *(const WORD *)(grp + 4);
    if (gsz < 6u + 14u * count) return false;
    for (WORD i = 0; i < count; ++i)
    {
        const WORD id = *(const WORD *)(grp + 6 + 14 * i + 12);  // GRPICONDIRENTRY.nID
        HRSRC r = FindResourceW(self, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(3) /* RT_ICON */);
        HGLOBAL g = r ? LoadResource(self, r) : nullptr;
        void *p = g ? LockResource(g) : nullptr;
        if (!p || !UpdateResourceW(h, MAKEINTRESOURCEW(3), MAKEINTRESOURCEW(id), 0x0409, p, SizeofResource(self, r))) return false;
    }
    return UpdateResourceW(h, MAKEINTRESOURCEW(14), MAKEINTRESOURCEW(1), 0x0409, (void *)grp, gsz) != FALSE;
}

// True when game_release_xbox.exe changed after onrush-pc.exe was made from it (the copy was patched and
// the setup did not get to rebrand it, e.g. it stopped on an error): onrush-pc.exe is the old game.
static bool GameExeOutdated(const wchar_t *dir)
{
    wchar_t src[MAX_PATH], dst[MAX_PATH];
    swprintf_s(src, MAX_PATH, L"%ls\\game_release_xbox.exe", dir);
    swprintf_s(dst, MAX_PATH, L"%ls\\onrush-pc.exe", dir);
    WIN32_FILE_ATTRIBUTE_DATA a, b;
    if (!GetFileAttributesExW(src, GetFileExInfoStandard, &a) || !GetFileAttributesExW(dst, GetFileExInfoStandard, &b)) return false;
    return CompareFileTime(&a.ftLastWriteTime, &b.ftLastWriteTime) > 0;
}

// True when onrush-pc.exe lacks the launcher's current icon (made by an older launcher, or the icon changed).
static bool GameExeIconStale(const wchar_t *dir)
{
    wchar_t exe[MAX_PATH];
    swprintf_s(exe, MAX_PATH, L"%ls\\onrush-pc.exe", dir);
    DWORD osz = 0; const BYTE *own = OwnIconGroup(&osz);
    if (!own) return false;
    HMODULE m = LoadLibraryExW(exe, nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!m) return false;
    bool stale = true;
    HRSRC r = FindResourceW(m, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(14));
    HGLOBAL g = r ? LoadResource(m, r) : nullptr;
    const void *p = g ? LockResource(g) : nullptr;
    if (p && SizeofResource(m, r) == osz && memcmp(p, own, osz) == 0)
    {   // same group (sizes, byte counts); also compare the largest image's bytes
        const WORD count = *(const WORD *)(own + 4);
        const WORD id = *(const WORD *)(own + 6 + 14 * (count - 1) + 12);
        HRSRC a = FindResourceW(m, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(3));
        HRSRC b = FindResourceW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(3));
        const void *pa = a ? LockResource(LoadResource(m, a)) : nullptr;
        const void *pb = b ? LockResource(LoadResource(GetModuleHandleW(nullptr), b)) : nullptr;
        stale = !(pa && pb && SizeofResource(m, a) == SizeofResource(GetModuleHandleW(nullptr), b) &&
                  memcmp(pa, pb, SizeofResource(m, a)) == 0);
    }
    FreeLibrary(m);
    return stale;
}

// onrush-pc.exe = copy of game_release_xbox.exe with the ONRUSH(TM) version resource and the launcher's icon.
static bool BrandGameExe(const wchar_t *dir)
{
    wchar_t src[MAX_PATH], dst[MAX_PATH];
    swprintf_s(src, MAX_PATH, L"%ls\\game_release_xbox.exe", dir);
    swprintf_s(dst, MAX_PATH, L"%ls\\onrush-pc.exe", dir);
    if (!CopyFileW(src, dst, FALSE)) return false;
    const DWORD ffi[13] = { 0xFEEF04BD, 0x00010000, 0x00010000, 0x00000001, 0x00010000, 0x00000001, 0x3F, 0, 0x40004, 1, 0, 0, 0 };
    std::vector<BYTE> ffiBytes((const BYTE *)ffi, (const BYTE *)ffi + sizeof(ffi));
    const wchar_t *kv[][2] = { { L"CompanyName", L"Codemasters" }, { L"FileDescription", L"ONRUSH™" }, { L"FileVersion", L"1.0.0.1" },
                               { L"InternalName", L"onrush-pc" }, { L"OriginalFilename", L"onrush-pc.exe" },
                               { L"ProductName", L"ONRUSH™" }, { L"ProductVersion", L"1.0.0.1" } };
    std::vector<BYTE> table, none;
    for (auto &e : kv) { auto n = VerNode(e[0], VerString(e[1]), 1, none); table.insert(table.end(), n.begin(), n.end()); }
    auto sfi = VerNode(L"StringFileInfo", none, 1, VerNode(L"041604B0", none, 1, table), 0);
    const WORD tr[2] = { 0x0416, 0x04B0 };
    auto vfi = VerNode(L"VarFileInfo", none, 1, VerNode(L"Translation", std::vector<BYTE>((const BYTE *)tr, (const BYTE *)tr + 4), 0, none), 0);
    std::vector<BYTE> kids = sfi; kids.insert(kids.end(), vfi.begin(), vfi.end());
    auto ver = VerNode(L"VS_VERSION_INFO", ffiBytes, 0, kids);
    // the game ships one version block (language 0x809); replace exactly that entry
    HANDLE h = BeginUpdateResourceW(dst, FALSE);
    if (!h) return true;  // plain copy still works, only the name in Task Manager differs
    BOOL ok = UpdateResourceW(h, MAKEINTRESOURCEW(16) /* RT_VERSION */, MAKEINTRESOURCEW(1), 0x0809, ver.data(), (DWORD)ver.size());
    if (ok && !CopyOwnIcon(h)) LogPrintf(L"[Setup] onrush-pc.exe sem o ícone do launcher\r\n");
    if (!EndUpdateResourceW(h, ok ? FALSE : TRUE) || !ok) LogPrintf(L"[Setup] onrush-pc.exe sem o nome ONRUSH (recurso de versão não gravado)\r\n");
    return true;
}

// Prepares destDir from the copy at mount/root. Returns false with a reason in err.
static bool SetupGameFolder(const wchar_t *mount, const wchar_t *root, const wchar_t *destDir, std::wstring &err)
{
    wchar_t s[MAX_PATH], d[MAX_PATH];
    bool same = _wcsicmp(mount, destDir) == 0;
    int copied = 0;
    for (const wchar_t *f : kGameFiles)
    {
        swprintf_s(s, MAX_PATH, L"%ls\\%ls", mount, f);
        swprintf_s(d, MAX_PATH, L"%ls\\%ls", destDir, f);
        if (same || GetFileAttributesW(s) == INVALID_FILE_ATTRIBUTES) continue;
        if (!CopyFileW(s, d, FALSE)) { err = std::wstring(L"copiar ") + f + L" (erro " + std::to_wstring(GetLastError()) + L")"; return false; }
        ++copied;
    }
    for (const wchar_t *f : kSystemFiles)
    {
        swprintf_s(s, MAX_PATH, L"%ls\\EmbeddedXvd\\Windows\\System32\\%ls", root, f);
        swprintf_s(d, MAX_PATH, L"%ls\\%ls", destDir, f);
        if (GetFileAttributesW(s) == INVALID_FILE_ATTRIBUTES) continue;
        if (!CopyFileW(s, d, FALSE)) { err = std::wstring(L"copiar ") + f + L" (erro " + std::to_wstring(GetLastError()) + L")"; return false; }
        ++copied;
    }
    struct { const wchar_t *name; wchar_t target[MAX_PATH]; } links[2];
    links[0].name = L"dat"; swprintf_s(links[0].target, MAX_PATH, L"%ls\\dat", mount);
    links[1].name = L"EmbeddedXvd"; swprintf_s(links[1].target, MAX_PATH, L"%ls\\EmbeddedXvd", root);
    for (auto &l : links)
    {
        swprintf_s(d, MAX_PATH, L"%ls\\%ls", destDir, l.name);
        if (same || GetFileAttributesW(l.target) == INVALID_FILE_ATTRIBUTES) continue;
        DWORD da = GetFileAttributesW(d);
        if (da != INVALID_FILE_ATTRIBUTES)
        {
            // a junction from an earlier setup may point at another copy, or at one that was moved or renamed
            if (!(da & FILE_ATTRIBUTE_REPARSE_POINT) || !_wcsicmp(JunctionTarget(d).c_str(), l.target)) continue;
            if (!RemoveDirectoryW(d)) { err = std::wstring(L"trocar o atalho ") + l.name + L" (erro " + std::to_wstring(GetLastError()) + L")"; return false; }
        }
        if (!CreateJunction(d, l.target)) { err = std::wstring(L"criar o atalho ") + l.name + L" (erro " + std::to_wstring(GetLastError()) + L")"; return false; }
        LogPrintf(L"[Setup] %ls -> %ls\r\n", d, l.target);
    }
    if (!BrandGameExe(destDir)) { err = L"criar onrush-pc.exe (erro " + std::to_wstring(GetLastError()) + L")"; return false; }
    LogPrintf(L"[Setup] Jogo preparado em %ls (%d arquivos copiados de %ls)\r\n", destDir, copied, root);
    return true;
}

// ---- Final version (1.80.100.100) from the base copy + the update patch -------------------
// The port targets the game's final update. The patch (delta package) is a folder that mirrors the
// package: <file>.gdiff = W3C GDIFF v4 binary diff of <file>, everything else = new/replaced files
// (the new Mount\dat\*.dat packs and Mount\dat\index\dat.ndx). It is applied to the player's base
// copy; each patched file keeps its old version as <file>.pre_gdiff (also how a re-run knows that
// file is done).
static const wchar_t *kFinalVersion = L"1.80.100.100";

// Version="..." of the package identity in <mount>\appxmanifest.xml ("" if unreadable).
static std::wstring GameCopyVersion(const wchar_t *mount)
{
    wchar_t p[MAX_PATH];
    swprintf_s(p, MAX_PATH, L"%ls\\appxmanifest.xml", mount);
    HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    char buf[16384] = {}; DWORD got = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(h);
    const char *id = strstr(buf, "<Identity");
    const char *v = id ? strstr(id, "Version=\"") : nullptr;
    if (!v) return L"";
    v += 9;
    std::wstring out;
    while (*v && *v != '"' && out.size() < 32) out += (wchar_t)*v++;
    return out;
}

static bool PatchReadFile(const wchar_t *path, std::vector<BYTE> &out)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz; bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart < (1ll << 31);
    if (ok) { out.resize((size_t)sz.QuadPart); DWORD got = 0; ok = out.empty() || (ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size()); }
    CloseHandle(h);
    return ok;
}

// W3C GDIFF v4 (big-endian): 0 = end, 1..246 = that many literal bytes, 247/248 = u16/u32 length
// + bytes, 249..255 = copy (position, length) from the old file with different field sizes.
static bool GdiffApply(const std::vector<BYTE> &src, const std::vector<BYTE> &d, std::vector<BYTE> &out)
{
    size_t n = d.size(), p = 5;
    if (n < 5 || d[0] != 0xD1 || d[1] != 0xFF || d[2] != 0xD1 || d[3] != 0xFF || d[4] != 4) return false;
    auto be = [&](int bytes, unsigned long long &v) {
        if (p + bytes > n) return false;
        v = 0; for (int i = 0; i < bytes; i++) v = (v << 8) | d[p++];
        return true; };
    out.clear(); out.reserve(src.size() + src.size() / 8);
    while (p < n)
    {
        BYTE c = d[p++];
        if (c == 0) return true;
        unsigned long long len = 0, pos = 0;
        if (c <= 246) len = c;
        else if (c == 247) { if (!be(2, len)) return false; }
        else if (c == 248) { if (!be(4, len)) return false; }
        if (c <= 248)
        {
            if (p + len > n) return false;
            out.insert(out.end(), d.begin() + p, d.begin() + p + (size_t)len); p += (size_t)len;
            continue;
        }
        static const int posB[] = { 2, 2, 2, 4, 4, 4, 8 }, lenB[] = { 1, 2, 4, 1, 2, 4, 4 };
        if (!be(posB[c - 249], pos) || !be(lenB[c - 249], len) || pos + len > src.size()) return false;
        out.insert(out.end(), src.begin() + (size_t)pos, src.begin() + (size_t)(pos + len));
    }
    return true;  // no explicit end marker
}

struct PatchFile { std::wstring rel; ULONGLONG size; bool gdiff; };

static void ListPatch(const std::wstring &dir, const std::wstring &rel, std::vector<PatchFile> &out)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\" + rel + (rel.empty() ? L"*" : L"\\*")).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
    {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        std::wstring r = rel.empty() ? fd.cFileName : rel + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) ListPatch(dir, r, out);
            continue;
        }
        size_t L = r.size();
        bool g = L > 6 && !_wcsicmp(r.c_str() + L - 6, L".gdiff");
        // leftovers of an earlier run inside the game copy
        if ((L > 14 && !_wcsicmp(r.c_str() + L - 14, L".gdiff.applied")) || (L > 10 && !_wcsicmp(r.c_str() + L - 10, L".pre_gdiff"))) continue;
        out.push_back({ r, ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow, g });
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

struct PatchJob
{
    std::wstring patch, game, err;
    std::vector<PatchFile> files;
    ULONGLONG total = 0;
    volatile LONG64 done = 0;
    bool ok = false;
};

static DWORD CALLBACK PatchCopyProgress(LARGE_INTEGER, LARGE_INTEGER copied, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD, HANDLE, HANDLE, LPVOID ctx)
{
    auto *pr = (std::pair<PatchJob *, ULONGLONG> *)ctx;
    InterlockedExchange64(&pr->first->done, (LONG64)(pr->second + copied.QuadPart));
    return PROGRESS_CONTINUE;
}

static DWORD WINAPI PatchThread(LPVOID arg)
{
    PatchJob *j = (PatchJob *)arg;
    bool inPlace = _wcsicmp(j->patch.c_str(), j->game.c_str()) == 0;
    ULONGLONG base = 0;
    int patched = 0, copied = 0;
    for (const PatchFile &f : j->files)
    {
        std::wstring dst = j->game + L"\\" + f.rel, src = j->patch + L"\\" + f.rel;
        if (f.gdiff)
        {
            std::wstring target = dst.substr(0, dst.size() - 6), old = target + L".pre_gdiff";
            if (GetFileAttributesW(old.c_str()) == INVALID_FILE_ATTRIBUTES)  // not patched yet
            {
                std::vector<BYTE> a, d, out;
                if (!PatchReadFile(target.c_str(), a)) { j->err = L"ler " + f.rel.substr(0, f.rel.size() - 6); return 0; }
                if (!PatchReadFile(src.c_str(), d) || !GdiffApply(a, d, out)) { j->err = L"aplicar " + f.rel; return 0; }
                std::wstring tmp = target + L".tmp";
                HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
                DWORD w = 0;
                bool wr = h != INVALID_HANDLE_VALUE && WriteFile(h, out.data(), (DWORD)out.size(), &w, nullptr) && w == out.size();
                if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
                if (!wr || !MoveFileExW(target.c_str(), old.c_str(), 0) || !MoveFileExW(tmp.c_str(), target.c_str(), 0))
                { DeleteFileW(tmp.c_str()); j->err = L"gravar " + f.rel.substr(0, f.rel.size() - 6) + L" (erro " + std::to_wstring(GetLastError()) + L")"; return 0; }
                if (inPlace) MoveFileExW(src.c_str(), (src + L".applied").c_str(), 0);
                ++patched;
            }
        }
        else if (!inPlace)
        {
            // new/replaced files are MOVED out of the patch folder (instant on the same disk; on another disk
            // Windows copies and then deletes the source), so the patch data is not stored twice
            WIN32_FILE_ATTRIBUTE_DATA fa;
            bool same = GetFileAttributesExW(dst.c_str(), GetFileExInfoStandard, &fa) && (((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow) == f.size;
            if (same) DeleteFileW(src.c_str());  // already in the game copy (earlier, interrupted run)
            else
            {
                std::wstring dir = dst.substr(0, dst.find_last_of(L'\\'));
                SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
                std::pair<PatchJob *, ULONGLONG> ctx(j, base);
                if (!MoveFileWithProgressW(src.c_str(), dst.c_str(), PatchCopyProgress, &ctx, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH))
                { j->err = L"mover " + f.rel + L" (erro " + std::to_wstring(GetLastError()) + L")"; return 0; }
                ++copied;
            }
        }
        base += f.size;
        InterlockedExchange64(&j->done, (LONG64)base);
    }
    LogPrintf(L"[Setup] patch aplicado: %d arquivo(s) atualizado(s), %d movido(s) da pasta do patch\r\n", patched, copied);
    j->ok = true;
    return 0;
}

// "-setup <copy> -patch <patch folder>": patch folder from the command line, no questions asked
static wchar_t g_cmdPatchDir[MAX_PATH] = L"";

// Asks for the patch folder and applies it to the copy at root. True when the copy is now final.
static bool OfferGamePatch(HWND hwnd, const wchar_t *mount, const wchar_t *root)
{
    wchar_t msg[1024], dir[MAX_PATH];
    std::wstring ver = GameCopyVersion(mount);
    bool quiet = g_cmdPatchDir[0] != 0;
    if (quiet) wcscpy_s(dir, MAX_PATH, g_cmdPatchDir);
    swprintf_s(msg, 1024, T(S_PATCH_ASK), ver.empty() ? L"?" : ver.c_str(), kFinalVersion);
    if (!quiet && MessageBoxW(hwnd, msg, L"ONRUSH", MB_ICONINFORMATION | MB_OKCANCEL) != IDOK) return false;
    for (;;)
    {
        if (quiet)
        {
            if (FileExistsIn(dir, L"Mount\\game_release_xbox.exe.gdiff")) break;
            LogPrintf(L"[Setup] %ls não é o patch do ONRUSH\r\n", dir);
            return false;
        }
        if (!PickFolder(hwnd, T(S_PICK_PATCH), dir)) return false;
        size_t n = wcslen(dir);
        while (n > 3 && dir[n - 1] == L'\\') dir[--n] = 0;
        if (FileExistsIn(dir, L"Mount\\game_release_xbox.exe.gdiff")) break;
        // the Mount folder of the patch was picked
        if (FileExistsIn(dir, L"game_release_xbox.exe.gdiff")) { PathRemoveFileSpecW(dir); break; }
        if (MessageBoxW(hwnd, T(S_BAD_PATCH), L"ONRUSH", MB_ICONWARNING | MB_YESNO) != IDYES) return false;
    }
    swprintf_s(msg, 1024, T(S_PATCH_CONFIRM), root);
    if (!quiet && MessageBoxW(hwnd, msg, L"ONRUSH", MB_ICONQUESTION | MB_YESNO) != IDYES) return false;

    PatchJob job;
    job.patch = dir; job.game = root;
    ListPatch(job.patch, L"", job.files);
    ULONGLONG need = 0;
    for (const PatchFile &f : job.files) { job.total += f.size; if (!f.gdiff) need += f.size; }
    // files are moved: on the same disk that takes no extra space
    wchar_t volP[MAX_PATH] = {}, volG[MAX_PATH] = {};
    if (GetVolumePathNameW(dir, volP, MAX_PATH) && GetVolumePathNameW(root, volG, MAX_PATH) && !_wcsicmp(volP, volG)) need = 0;
    ULARGE_INTEGER freeB = {};
    if (need && _wcsicmp(dir, root) != 0 && GetDiskFreeSpaceExW(root, &freeB, nullptr, nullptr) && freeB.QuadPart < need + (512ull << 20))
    {
        // files already copied by an earlier, interrupted run are counted again: only a hint
        swprintf_s(msg, 1024, T(S_PATCH_FAIL), (L"pouco espaço no disco (" + std::to_wstring(need >> 30) + L" GB necessários)").c_str());
        if (MessageBoxW(hwnd, msg, L"ONRUSH", MB_ICONWARNING | MB_RETRYCANCEL) != IDRETRY) return false;
    }
    LogPrintf(L"[Setup] aplicando o patch %ls em %ls (%zu arquivos)\r\n", dir, root, job.files.size());

    // worker thread; the window stays responsive (input disabled) and the JOGAR button shows the progress
    HWND top = GetAncestor(hwnd, GA_ROOT);
    EnableWindow(top, FALSE);
    wchar_t oldText[128] = {};
    if (g_hBtnLaunch) GetWindowTextW(g_hBtnLaunch, oldText, 128);
    HANDLE th = CreateThread(nullptr, 0, PatchThread, &job, 0, nullptr);
    int lastPct = -1;
    while (th && MsgWaitForMultipleObjects(1, &th, FALSE, 200, QS_ALLINPUT) != WAIT_OBJECT_0)
    {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        int pct = job.total ? (int)(job.done * 100 / job.total) : 0;
        if (pct != lastPct && g_hBtnLaunch)
        {
            wchar_t t[128]; swprintf_s(t, 128, T(S_PATCH_PROGRESS), pct);
            SetWindowTextW(g_hBtnLaunch, t); InvalidateRect(g_hBtnLaunch, nullptr, TRUE); lastPct = pct;
        }
    }
    if (th) CloseHandle(th); else job.err = L"iniciar a cópia";
    if (g_hBtnLaunch) { SetWindowTextW(g_hBtnLaunch, oldText); InvalidateRect(g_hBtnLaunch, nullptr, TRUE); }
    EnableWindow(top, TRUE);
    SetForegroundWindow(top);
    std::wstring now = GameCopyVersion(mount);
    if (job.ok && now != kFinalVersion) job.err = L"a cópia ficou na versão " + now + L" (esperado " + kFinalVersion + L")";
    if (job.err.empty())
    {   // the copy is final now: the old versions (.pre_gdiff, only needed to resume) would just take space
        int gone = 0;
        for (const PatchFile &f : job.files)
            if (f.gdiff && DeleteFileW((job.game + L"\\" + f.rel.substr(0, f.rel.size() - 6) + L".pre_gdiff").c_str())) ++gone;
        LogPrintf(L"[Setup] %d versão(ões) antiga(s) (.pre_gdiff) removida(s)\r\n", gone);
    }
    if (!job.err.empty())
    {
        LogPrintf(L"[Setup] patch falhou: %ls\r\n", job.err.c_str());
        swprintf_s(msg, 1024, T(S_PATCH_FAIL), job.err.c_str());
        MessageBoxW(hwnd, msg, L"ONRUSH", MB_ICONERROR);
        return false;
    }
    return true;
}

// Asks to set the game up in the launcher's folder from the copy found in dir.
// Returns true when destDir (g_exeDir) is ready to play.
static bool OfferGameSetup(HWND hwnd, const wchar_t *dir)
{
    wchar_t mount[MAX_PATH], root[MAX_PATH], msg[1024];
    if (!FindGameCopy(dir, mount, root)) return false;
    if (!PortFilesNextToLauncher()) { MessageBoxW(hwnd, T(S_SETUP_NOPORT), L"ONRUSH", MB_ICONWARNING); return false; }
    // only the final version is supported: an older copy is updated with the patch first
    std::wstring ver = GameCopyVersion(mount);
    LogPrintf(L"[Setup] cópia do jogo: versão %ls\r\n", ver.empty() ? L"desconhecida" : ver.c_str());
    if (ver != kFinalVersion && !OfferGamePatch(hwnd, mount, root)) return false;
    swprintf_s(msg, 1024, T(S_SETUP_ASK), root, g_exeDir);
    if (MessageBoxW(hwnd, msg, L"ONRUSH", MB_ICONQUESTION | MB_YESNO) != IDYES) return false;
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    std::wstring err;
    bool ok = SetupGameFolder(mount, root, g_exeDir, err);
    SetCursor(old);
    if (!ok)
    {
        swprintf_s(msg, 1024, T(S_SETUP_FAIL), err.c_str());
        MessageBoxW(hwnd, msg, L"ONRUSH", MB_ICONERROR);
        return false;
    }
    return true;
}
