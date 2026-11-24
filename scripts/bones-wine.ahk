; Win+E opens bones instead of Explorer (AutoHotkey v1). Put a copy in the
; Startup folder. Win+Shift+E still opens Explorer.
#NoEnv
#NoTrayIcon
#SingleInstance Force
EnvGet, LocalAppData, LOCALAPPDATA

#e::Run, %LocalAppData%\Programs\bones\bones.exe
#+e::Run, explorer.exe
