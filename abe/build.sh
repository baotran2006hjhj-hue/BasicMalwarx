#!/usr/bin/env bash
set -e
cd "$(dirname "$0")"

CXX=x86_64-w64-mingw32-g++

echo "[*] payload..."
$CXX -shared -o abe_payload.dll abe_payload.cpp \
    -static -lole32 -loleaut32 -lshlwapi -lshell32

echo "[*] injector..."
$CXX -o abe_inject.exe abe_inject.cpp \
    -mwindows -static \
    -lwinhttp -lbcrypt -lcrypt32 -lole32 -loleaut32 -lshlwapi -lshell32 -ladvapi32

echo "[+] done"
ls -l abe_payload.dll abe_inject.exe
sha256sum abe_payload.dll abe_inject.exe
