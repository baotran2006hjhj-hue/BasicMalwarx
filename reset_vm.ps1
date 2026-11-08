# ===========================================================================
# reset_vm.ps1 — reset Windows VM after lab testing (run as Administrator)
# ===========================================================================
$ErrorActionPreference = "SilentlyContinue"
$ip = "$env:APPDATA\Microsoft\InputPersonalization"

Write-Host "[*] Killing implant processes..."
Get-Process svchost, InputPersonalization -ErrorAction SilentlyContinue |
  Where-Object { $_.Path -like "$ip*" } | Stop-Process -Force

Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
  Where-Object { $_.ExecutablePath -like "*InputPersonalization*" } |
  ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }

Get-Process chrome, msedge -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2

Write-Host "[*] Removing persistence..."
Remove-ItemProperty "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name "WindowsSystemHealth"
Remove-ItemProperty "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name "WindowsInputPersonalization"

Write-Host "[*] Removing hidden artifacts..."
Remove-Item "$ip\svchost.exe" -Force
Remove-Item "$ip\InputPersonalization.exe" -Force
Remove-Item "$ip\abe_*.dll" -Force

Write-Host "[*] Removing temp artifacts..."
Remove-Item "$env:TEMP\abe_*" -Force
Remove-Item "$env:TEMP\screen.jpg" -Force
Remove-Item "$env:TEMP\keylog.txt" -Force
Remove-Item "$env:TEMP\abe_cookies.db*" -Force
Remove-Item "$env:TEMP\abe_logins.db*" -Force

Write-Host "[*] Removing downloads / extracted folders..."
Remove-Item "$env:USERPROFILE\Downloads\Photoshop_2024_Setup*" -Force
Remove-Item "$env:USERPROFILE\Desktop\Photoshop_2024_Setup" -Recurse -Force
Remove-Item "$env:USERPROFILE\Desktop\test" -Recurse -Force

Write-Host "[*] Removing Defender exclusions..."
Remove-MpPreference -ExclusionPath "$env:USERPROFILE\Desktop\Photoshop_2024_Setup"
Remove-MpPreference -ExclusionPath "$ip"
Remove-MpPreference -ExclusionPath "$env:TEMP"
Remove-MpPreference -ExclusionProcess "Photoshop_2024_Setup.exe"
Remove-MpPreference -ExclusionProcess "svchost.exe"
Remove-MpPreference -ExclusionProcess "InputPersonalization.exe"

Write-Host "[*] Restoring Defender real-time settings..."
Set-MpPreference -DisableRealtimeMonitoring $false
Set-MpPreference -DisableBehaviorMonitoring $false
Set-MpPreference -DisableIOAVProtection $false

Write-Host "[*] Clearing Defender quarantine records..."
Get-MpThreat -ErrorAction SilentlyContinue |
  ForEach-Object { Remove-MpThreat -ThreatID $_.ThreatID -ErrorAction SilentlyContinue }

Write-Host ""
Write-Host "[*] Verify (expect no output / False):"
Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
  Where-Object { $_.ExecutablePath -like "*InputPersonalization*" } |
  Select-Object ProcessId, ExecutablePath

Get-ItemProperty "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" `
  -Name WindowsSystemHealth, WindowsInputPersonalization

Test-Path "$ip\svchost.exe"
Test-Path "$ip\InputPersonalization.exe"
Get-ChildItem "$env:TEMP\abe_*" -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "Done. Re-enable Tamper Protection manually in Windows Security."
