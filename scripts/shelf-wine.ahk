; Win+E opens shelf instead of Explorer (AutoHotkey v1). Put a copy in the
; Startup folder. Win+Shift+E still opens Explorer.
#NoEnv
#NoTrayIcon
#SingleInstance Force
EnvGet, LocalAppData, LOCALAPPDATA

#e::Run, %LocalAppData%\Programs\shelf\shelf.exe
#+e::Run, explorer.exe
