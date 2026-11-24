# Makes bones the default way folders and drives open for this user: double-
# clicking a folder (desktop or Explorer), "open folder" from other apps,
# `start C:\x`. Only HKCU is touched. Explorer's own "Open" stays in the
# right-click menu. Undo with -Undo.
#
# Not covered (Windows always uses Explorer): "Show in folder" (apps call
# SHOpenFolderAndSelectItems), This PC / Recycle Bin / Control Panel, and the
# Open/Save dialogs. Win+E is remapped separately (scripts\bones-wine.ahk).
param([switch]$Undo, [string]$Exe = "$env:LOCALAPPDATA\Programs\bones\bones.exe")

$classes = 'HKCU:\Software\Classes'
foreach ($class in 'Directory', 'Drive') {
    $shell = "$classes\$class\shell"
    if ($Undo) {
        Remove-Item "$shell\bones" -Recurse -Force -ErrorAction SilentlyContinue
        if ((Get-Item $shell -ErrorAction SilentlyContinue) -and
            (Get-ItemProperty $shell).'(default)' -eq 'bones') {
            reg.exe delete "HKCU\Software\Classes\$class\shell" /ve /f | Out-Null
        }
        continue
    }
    if (-not (Test-Path $Exe)) { throw "bones isn't installed at $Exe" }
    # Never New-Item -Force an existing key: it recreates it, dropping other
    # apps' verbs (it once wiped TreeSize's from Folder\shell).
    if (-not (Test-Path "$shell\bones\command")) { New-Item "$shell\bones\command" -Force | Out-Null }
    Set-ItemProperty "$shell\bones" -Name '(default)' -Value 'Open in bones'
    Set-ItemProperty "$shell\bones" -Name 'Icon' -Value "`"$Exe`",0"
    Set-ItemProperty "$shell\bones\command" -Name '(default)' -Value "`"$Exe`" `"%1`""
    Set-ItemProperty $shell -Name '(default)' -Value 'bones'
}
# Folder lists "open" ahead of Directory's default, so name bones as Folder's
# default too. The verb itself lives only on Directory and Drive, so virtual
# folders (This PC, Recycle Bin, Control Panel) have no "bones" verb and fall
# back to Explorer's Open.
$folder = "$classes\Folder\shell"
if ($Undo) {
    if ((Get-ItemProperty $folder -ErrorAction SilentlyContinue).'(default)' -eq 'bones') {
        reg.exe delete 'HKCU\Software\Classes\Folder\shell' /ve /f | Out-Null
    }
} else {
    if (-not (Test-Path $folder)) { New-Item $folder -Force | Out-Null }
    Set-ItemProperty $folder -Name '(default)' -Value 'bones'
}
foreach ($class in 'Directory', 'Drive', 'Folder') {
    $d = (Get-ItemProperty "$classes\$class\shell" -ErrorAction SilentlyContinue).'(default)'
    "$class default verb: $(if ($d) { $d } else { '(Windows default)' })"
}
