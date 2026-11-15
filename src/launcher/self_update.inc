// ============================================================================
// Launcher self-update (after update_check.inc found a newer release):
//  1. downloads the release zip (only from this repository's release downloads) to
//     %TEMP%\rushlauncher-update and checks its SHA-256 against the "digest" GitHub publishes for the asset;
//  2. extracts it with Windows' own tar.exe;
//  3. copies this exe to the temp folder and runs that copy with
//       -applyupdate "<extracted folder>" "<launcher folder>" <pid>
//     then the launcher closes. The copy waits for it to exit, copies the new files over the old
//     ones (never the player's settings, saves, shaders or game files) and opens the new launcher.
// ============================================================================
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

#define WM_UPDATE_PROGRESS    (WM_USER + 131)  // wParam = percent
#define WM_UPDATE_READY       (WM_USER + 132)  // lParam = UpdateJob* (ok or error)

struct UpdateJob
{
    HWND notify;
    wchar_t tag[64];
    wchar_t asset[512];
    char sha256[65];           // expected, lowercase hex
    bool ok = false;
    wchar_t error[160] = {};
    wchar_t srcDir[MAX_PATH] = {};  // extracted release folder (has RushLauncher.exe)
};

// Before v0.2.0 the launcher was called OnRushLauncher.exe. Launchers of those versions install an update only if
// the zip has OnRushLauncher.exe and then start that file, so release zips also carry a copy under the old name:
// started under it, the launcher hands over to RushLauncher.exe next to it, which then deletes the old file.
static const wchar_t kLauncherExe[] = L"RushLauncher.exe";
static const wchar_t kLegacyLauncherExe[] = L"OnRushLauncher.exe";

static bool HasLauncherExe(const wchar_t *dir)
{
    wchar_t probe[MAX_PATH];
    for (const wchar_t *n : { kLauncherExe, kLegacyLauncherExe })
    {
        swprintf_s(probe, MAX_PATH, L"%ls\\%ls", dir, n);
        if (GetFileAttributesW(probe) != INVALID_FILE_ATTRIBUTES) return true;
    }
    return false;
}

static bool g_updateDownloading = false;
static int g_updatePercent = 0;

// Entries of the launcher folder an update never writes, even if a release zip had them.
static const wchar_t *const kUpdateKeep[] = {
    L"onrush.ini", L"onrush_launcher.log", L"save", L"shaders", L"playlists", L"Fotos", L"dat", L"EmbeddedXvd",
    L"onrush-pc.exe", L"game_release_xbox.exe", L"appxmanifest.xml", L"appdata.bin", L"Catalog000.bin", L"resources.pri",
};

static bool UpdateTempDir(wchar_t *out)
{
    wchar_t tmp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tmp)) return false;
    swprintf_s(out, MAX_PATH, L"%lsrushlauncher-update", tmp);
    return true;
}

// Deletes a folder tree we created ourselves; never follows links.
static void UpdateDeleteTree(const wchar_t *dir)
{
    wchar_t pat[MAX_PATH];
    swprintf_s(pat, MAX_PATH, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            wchar_t p[MAX_PATH];
            swprintf_s(p, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { RemoveDirectoryW(p); DeleteFileW(p); continue; }
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) UpdateDeleteTree(p);
            else { SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL); DeleteFileW(p); }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

static bool UpdateRun(const wchar_t *exe, wchar_t *cmd, DWORD timeoutMs, DWORD *exitCode)
{
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    bool done = WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0;
    if (!done) TerminateProcess(pi.hProcess, 1);
    GetExitCodeProcess(pi.hProcess, exitCode);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return done;
}

static DWORD WINAPI UpdateDownloadThread(LPVOID param)
{
    UpdateJob *job = (UpdateJob *)param;
    wchar_t base[MAX_PATH], zip[MAX_PATH], stage[MAX_PATH];
    URL_COMPONENTSW uc = { sizeof(uc) };
    wchar_t host[256], path[1024];
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 1024;
    HINTERNET ses = nullptr, con = nullptr, req = nullptr;
    HANDLE out = INVALID_HANDLE_VALUE;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD code = 0, sz = sizeof(code);
    unsigned long long total = 0, got = 0;
    UCHAR digest[32];
    char hex[65];

    if (!UpdateTempDir(base)) { wcscpy_s(job->error, L"pasta temporária"); goto done; }
    UpdateDeleteTree(base);  // leftovers of an earlier attempt
    CreateDirectoryW(base, nullptr);
    swprintf_s(zip, MAX_PATH, L"%ls\\rushlauncher-%ls.zip", base, job->tag);
    swprintf_s(stage, MAX_PATH, L"%ls\\files", base);

    if (!WinHttpCrackUrl(job->asset, 0, 0, &uc)) { wcscpy_s(job->error, L"endereço inválido"); goto done; }
    ses = WinHttpOpen(L"rushlauncher-update/" OLAUNCHER_VERSION, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (ses) WinHttpSetTimeouts(ses, 10000, 10000, 15000, 30000);
    con = ses ? WinHttpConnect(ses, host, uc.nPort, 0) : nullptr;
    req = con ? WinHttpOpenRequest(con, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    // GitHub answers with a redirect to its download host; WinHTTP follows it (HTTPS only)
    if (!req || !WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(req, nullptr) ||
        !WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX))
    { swprintf_s(job->error, L"WinHTTP %lu", GetLastError()); goto done; }
    if (code != 200) { swprintf_s(job->error, L"HTTP %lu", code); goto done; }
    {
        wchar_t len[32]; DWORD ls = sizeof(len);
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, len, &ls, WINHTTP_NO_HEADER_INDEX)) total = _wcstoui64(len, nullptr, 10);
    }
    out = CreateFileW(zip, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (out == INVALID_HANDLE_VALUE || BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0 ||
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) != 0)
    { wcscpy_s(job->error, L"não foi possível gravar o arquivo"); goto done; }
    {
        std::vector<char> buf(1 << 16);
        int lastPct = -1;
        for (;;)
        {
            DWORD n = 0;
            if (!WinHttpReadData(req, buf.data(), (DWORD)buf.size(), &n)) { swprintf_s(job->error, L"download interrompido (%lu)", GetLastError()); goto done; }
            if (!n) break;
            DWORD w = 0;
            if (!WriteFile(out, buf.data(), n, &w, nullptr) || w != n) { wcscpy_s(job->error, L"disco cheio?"); goto done; }
            BCryptHashData(hash, (PUCHAR)buf.data(), n, 0);
            got += n;
            if (got > (200ull << 20)) { wcscpy_s(job->error, L"arquivo grande demais"); goto done; }
            int pct = total ? (int)(got * 100 / total) : 0;
            if (pct != lastPct) { lastPct = pct; PostMessageW(job->notify, WM_UPDATE_PROGRESS, pct, 0); }
        }
    }
    CloseHandle(out); out = INVALID_HANDLE_VALUE;
    if (total && got != total) { wcscpy_s(job->error, L"download incompleto"); goto done; }
    BCryptFinishHash(hash, digest, 32, 0);
    for (int i = 0; i < 32; ++i) sprintf_s(hex + i * 2, 3, "%02x", digest[i]);
    if (_stricmp(hex, job->sha256) != 0) { wcscpy_s(job->error, L"o arquivo baixado não confere (SHA-256)"); goto done; }

    {   // extract with Windows' tar.exe (bsdtar, reads zip)
        wchar_t tar[MAX_PATH], cmd[3 * MAX_PATH];
        GetSystemDirectoryW(tar, MAX_PATH);
        wcscat_s(tar, MAX_PATH, L"\\tar.exe");
        CreateDirectoryW(stage, nullptr);
        swprintf_s(cmd, 3 * MAX_PATH, L"\"%ls\" -xf \"%ls\" -C \"%ls\"", tar, zip, stage);
        DWORD ec = 1;
        if (!UpdateRun(tar, cmd, 120000, &ec) || ec != 0) { swprintf_s(job->error, L"falha ao descompactar (tar %lu)", ec); goto done; }
    }
    {   // the zip has one top folder (any name); accept files at the root too
        wchar_t probe[MAX_PATH], pat[MAX_PATH];
        if (HasLauncherExe(stage)) wcscpy_s(job->srcDir, MAX_PATH, stage);
        swprintf_s(pat, MAX_PATH, L"%ls\\*", stage);
        WIN32_FIND_DATAW fd;
        HANDLE h = job->srcDir[0] ? INVALID_HANDLE_VALUE : FindFirstFileW(pat, &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
                swprintf_s(probe, MAX_PATH, L"%ls\\%ls", stage, fd.cFileName);
                if (HasLauncherExe(probe)) { wcscpy_s(job->srcDir, MAX_PATH, probe); break; }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        if (!job->srcDir[0]) { wcscpy_s(job->error, L"o zip não tem o RushLauncher.exe"); goto done; }
    }
    job->ok = true;

done:
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    if (!PostMessageW(job->notify, WM_UPDATE_READY, 0, (LPARAM)job)) delete job;
    return 0;
}

static bool StartUpdateDownload(HWND notify, const UpdateResult &r)
{
    if (g_updateDownloading || !r.asset[0] || !r.sha256[0]) return false;
    UpdateJob *job = new UpdateJob();
    job->notify = notify;
    wcscpy_s(job->tag, r.tag);
    wcscpy_s(job->asset, r.asset);
    strcpy_s(job->sha256, r.sha256);
    g_updateDownloading = true; g_updatePercent = 0;
    HANDLE t = CreateThread(nullptr, 0, UpdateDownloadThread, job, 0, nullptr);
    if (t) { CloseHandle(t); return true; }
    delete job; g_updateDownloading = false;
    return false;
}

// Runs the copy of the launcher in the temp folder that installs the update; true if it started.
static bool LaunchUpdateInstaller(const UpdateJob *job, wchar_t *err, int errLen)
{
    wchar_t base[MAX_PATH], self[MAX_PATH], copy[MAX_PATH], cmd[4 * MAX_PATH];
    if (!UpdateTempDir(base)) { wcscpy_s(err, errLen, L"pasta temporária"); return false; }
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    swprintf_s(copy, MAX_PATH, L"%ls\\rushlauncher-updater.exe", base);
    if (!CopyFileW(self, copy, FALSE)) { swprintf_s(err, errLen, L"cópia do instalador (%lu)", GetLastError()); return false; }
    swprintf_s(cmd, 4 * MAX_PATH, L"\"%ls\" -applyupdate \"%ls\" \"%ls\" %lu", copy, job->srcDir, g_exeDir, GetCurrentProcessId());
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(copy, cmd, nullptr, nullptr, FALSE, 0, nullptr, base, &si, &pi)) { swprintf_s(err, errLen, L"instalador (%lu)", GetLastError()); return false; }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return true;
}

// ---- "-applyupdate <src> <dst> <pid>" (runs from %TEMP%\rushlauncher-update) ----
static bool UpdateKeepName(const wchar_t *name)
{
    for (const wchar_t *k : kUpdateKeep) if (!_wcsicmp(name, k)) return true;
    return false;
}

static bool UpdateCopyTree(const wchar_t *src, const wchar_t *dst, bool top, std::wstring &err)
{
    CreateDirectoryW(dst, nullptr);
    wchar_t pat[MAX_PATH];
    swprintf_s(pat, MAX_PATH, L"%ls\\*", src);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) { err = L"pasta da atualização vazia"; return false; }
    bool ok = true;
    do
    {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (top && (UpdateKeepName(fd.cFileName) || !_wcsicmp(fd.cFileName, kLegacyLauncherExe))) continue;  // old name: not needed here
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        wchar_t s[MAX_PATH], d[MAX_PATH];
        swprintf_s(s, MAX_PATH, L"%ls\\%ls", src, fd.cFileName);
        swprintf_s(d, MAX_PATH, L"%ls\\%ls", dst, fd.cFileName);
        DWORD da = GetFileAttributesW(d);
        if (da != INVALID_FILE_ATTRIBUTES && (da & FILE_ATTRIBUTE_REPARSE_POINT)) continue;  // never write through a link
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { ok = UpdateCopyTree(s, d, false, err) && ok; continue; }
        bool copied = false;
        for (int i = 0; i < 40 && !copied; ++i)  // files can stay locked for a moment after the old launcher exits
        {
            if (da != INVALID_FILE_ATTRIBUTES) SetFileAttributesW(d, FILE_ATTRIBUTE_NORMAL);
            copied = CopyFileW(s, d, FALSE) != 0;
            if (!copied) Sleep(250);
        }
        if (!copied) { ok = false; err += std::wstring(L"\n") + d; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

static int RunApplyUpdate()
{
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int i = 0;
    while (i < argc && _wcsicmp(argv[i], L"-applyupdate")) ++i;
    if (i + 3 >= argc) { LocalFree(argv); return 1; }
    const wchar_t *src = argv[i + 1], *dst = argv[i + 2];
    DWORD pid = wcstoul(argv[i + 3], nullptr, 10);
    if (HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid)) { WaitForSingleObject(p, 30000); CloseHandle(p); }
    Sleep(300);
    std::wstring err;
    bool ok = HasLauncherExe(src) && UpdateCopyTree(src, dst, true, err);
    if (!ok)
        MessageBoxW(nullptr, (std::wstring(L"Não foi possível concluir a atualização do launcher.\nArquivos não atualizados:") + err +
                              L"\n\nBaixe o zip da versão nova no GitHub e extraia por cima da pasta do launcher.").c_str(),
                    L"RushLauncher", MB_ICONWARNING);
    wchar_t exe[MAX_PATH], cmd[2 * MAX_PATH];
    swprintf_s(exe, MAX_PATH, L"%ls\\%ls", dst, kLauncherExe);
    if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) swprintf_s(exe, MAX_PATH, L"%ls\\%ls", dst, kLegacyLauncherExe);
    swprintf_s(cmd, 2 * MAX_PATH, L"\"%ls\" %ls", exe, ok ? L"-updated" : L"");
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(exe, cmd, nullptr, nullptr, FALSE, 0, nullptr, dst, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    LocalFree(argv);
    return ok ? 0 : 1;
}

// Started as OnRushLauncher.exe (the old name) with RushLauncher.exe next to it: start that one with the same
// arguments and return true (the caller exits). Started as RushLauncher.exe: delete a leftover OnRushLauncher.exe
// in the background (it may still be running for a moment while it hands over).
static DWORD WINAPI LegacyLauncherCleanupThread(LPVOID)
{
    wchar_t old[MAX_PATH];
    swprintf_s(old, MAX_PATH, L"%ls\\%ls", g_exeDir, kLegacyLauncherExe);
    for (int i = 0; i < 40 && GetFileAttributesW(old) != INVALID_FILE_ATTRIBUTES; ++i)
    {
        SetFileAttributesW(old, FILE_ATTRIBUTE_NORMAL);
        if (DeleteFileW(old)) break;
        Sleep(500);
    }
    return 0;
}

static bool HandOverFromLegacyName()
{
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const wchar_t *name = wcsrchr(self, L'\\');
    name = name ? name + 1 : self;
    wchar_t exe[MAX_PATH];
    swprintf_s(exe, MAX_PATH, L"%ls\\%ls", g_exeDir, kLauncherExe);
    if (!_wcsicmp(name, kLegacyLauncherExe))
    {
        if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) return false;   // no new file: keep running as is
        const wchar_t *args = PathGetArgsW(GetCommandLineW());
        std::wstring cmd = std::wstring(L"\"") + exe + L"\" " + (args ? args : L"");
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        if (!CreateProcessW(exe, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, g_exeDir, &si, &pi)) return false;
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        return true;
    }
    if (!_wcsicmp(name, kLauncherExe))
        if (HANDLE t = CreateThread(nullptr, 0, LegacyLauncherCleanupThread, nullptr, 0, nullptr)) CloseHandle(t);
    return false;
}

// The new launcher removes the temp folder once the installer copy has exited.
static DWORD WINAPI UpdateCleanupThread(LPVOID)
{
    Sleep(5000);
    wchar_t base[MAX_PATH];
    if (UpdateTempDir(base) && GetFileAttributesW(base) != INVALID_FILE_ATTRIBUTES) UpdateDeleteTree(base);
    return 0;
}
