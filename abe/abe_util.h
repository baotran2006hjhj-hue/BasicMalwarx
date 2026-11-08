#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <cstring>

// ---------------------------------------------------------------------------
// String conversion
// ---------------------------------------------------------------------------
inline std::string W2U8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

inline std::wstring U82W(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

// ---------------------------------------------------------------------------
// Base64 (standard alphabet, padding optional)
// ---------------------------------------------------------------------------
inline std::vector<BYTE> Base64Decode(const std::string& in) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<BYTE> out;
    int buf = 0, bits = 0;
    for (char c : in) {
        int v = val(c);
        if (v < 0) continue; // '=' and whitespace
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((BYTE)((buf >> bits) & 0xFF));
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// File IO (shared read: works while browser holds the DB open)
// ---------------------------------------------------------------------------
inline bool ReadFileAll(const std::wstring& path, std::vector<BYTE>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (64LL << 20)) {
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

inline bool ReadFileText(const std::wstring& path, std::string& out) {
    std::vector<BYTE> b;
    if (!ReadFileAll(path, b)) return false;
    out.assign((const char*)b.data(), b.size());
    return true;
}

inline bool WriteFileAll(const std::wstring& path, const void* data, size_t len) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, data, (DWORD)len, &written, NULL);
    CloseHandle(h);
    return ok && written == len;
}

// ---------------------------------------------------------------------------
// Minimal JSON string extraction: "key" : "value"
// ---------------------------------------------------------------------------
inline bool ExtractJsonString(const std::string& json, const std::string& key, std::string& out) {
    std::string pat = "\"" + key + "\"";
    size_t p = json.find(pat);
    if (p == std::string::npos) return false;
    p = json.find(':', p + pat.size());
    if (p == std::string::npos) return false;
    p = json.find('"', p + 1);
    if (p == std::string::npos) return false;
    size_t q = json.find('"', p + 1);
    if (q == std::string::npos) return false;
    out = json.substr(p + 1, q - p - 1);
    return true;
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------
inline std::wstring TempPath(const std::wstring& name) {
    wchar_t tmp[MAX_PATH] = {0};
    GetTempPathW(MAX_PATH, tmp);
    return std::wstring(tmp) + name;
}

inline std::wstring SelfDir() {
    wchar_t self[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, self, MAX_PATH);
    std::wstring p(self);
    size_t slash = p.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? std::wstring() : p.substr(0, slash + 1);
}

// ---------------------------------------------------------------------------
// JSON escaping for values
// ---------------------------------------------------------------------------
inline std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    wsprintfA(buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    return out;
}
