// The file system and the shell: listing folders, drives, WSL distros,
// watching a folder, and the things Explorer does to files (open, copy,
// paste, recycle, rename, drag out, properties, its context menu).
#pragma once

#include <windows.h>

#include <atomic>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "paths.h"

namespace shelf {

std::wstring Widen(std::string_view s);
std::string Narrow(std::wstring_view s);
std::string ErrorText(DWORD code);

// DataDir is %APPDATA%\shelf (or SHELF_DATA, for test runs).
std::wstring DataDir();
// CacheDir is %LOCALAPPDATA%\shelf (or SHELF_DATA\cache).
std::wstring CacheDir();
bool ReadFileBytes(const std::wstring& path, std::string& out);
bool WriteFileAtomic(const std::wstring& path, const std::string& data);
bool IsDir(const std::wstring& path);

// ListDir reads a folder. On failure it returns false with the error.
bool ListDir(const std::wstring& dir, std::vector<Entry>& out, DWORD& error);

struct Drive {
    std::wstring root;  // "C:\"
    std::string label;  // "Windows (C:)"
    UINT type = 0;      // DRIVE_FIXED etc.
    uint64_t total = 0, free = 0;
};
// ListDrives asks every drive for its name and space; a slow one (an
// empty DVD drive, a dropped network drive) can take a while, so call it
// off the window's thread.
std::vector<Drive> ListDrives();

struct Distro {
    std::wstring name;
    DWORD uid = 0;
    bool isDefault = false;
};
// ListDistros reads the WSL distros from the registry (it doesn't start
// any).
std::vector<Distro> ListDistros();
// DistroHome finds a user's home by reading the distro's /etc/passwd
// (this starts the distro if it isn't running).
std::wstring DistroHome(const Distro& d);

struct Place {
    std::string label;
    std::wstring path;
    const char* icon;
};
// KnownPlaces is the usual set (home, desktop, downloads…), for the first
// run's pins.
std::vector<Place> KnownPlaces();
std::wstring HomeDir();

// Watcher calls changed (from its own thread) when the folder's contents
// change, at most every ~250 ms.
class Watcher {
public:
    explicit Watcher(std::function<void()> changed) : changed_(std::move(changed)) {}
    ~Watcher() { Stop(); }
    void Watch(const std::wstring& dir);
    void Stop();

private:
    std::function<void()> changed_;
    std::thread thread_;
    HANDLE stop_ = nullptr;
};

// The shell. Paths are full paths; the window's thread calls these
// (it's initialised for OLE).
void OpenItem(HWND owner, const std::wstring& path);
void OpenWith(HWND owner, const std::wstring& path);
// LinkTarget gives a shortcut's (.lnk) target path, or "".
std::wstring LinkTarget(const std::wstring& lnk);
void Properties(HWND owner, const std::vector<std::wstring>& paths);
void CopyText(HWND owner, const std::wstring& text);
// SetFileClipboard puts files on the clipboard to paste here or in
// Explorer; cut marks them to move.
bool SetFileClipboard(const std::vector<std::wstring>& paths, bool cut);
bool ClipboardHasFiles();
// The file jobs run on a thread of their own with the shell's progress
// and conflict dialogs, then call done on that thread.
void PasteInto(HWND owner, const std::wstring& dir, std::function<void()> done);
void Recycle(HWND owner, const std::vector<std::wstring>& paths, bool permanently, std::function<void()> done);
void CopyOrMove(HWND owner, const std::vector<std::wstring>& paths, const std::wstring& dir, bool move,
                std::function<void()> done);
// Rename returns "" or why it failed.
std::string Rename(const std::wstring& from, const std::wstring& to);
std::string NewFolder(const std::wstring& path);
// DragOut starts a drag of the files to another window (Explorer, a
// browser, a chat). Returns once it's dropped or called off.
void DragOut(HWND owner, const std::vector<std::wstring>& paths);
// ShellMenu shows Explorer's own right-click menu for the files (or for
// the folder's background if paths is empty) at a screen point. It
// returns "rename" if that was picked (the caller does it), else "".
std::string ShellMenu(HWND owner, const std::wstring& dir, const std::vector<std::wstring>& paths, POINT at);
// HandleMenuMessage passes the menu's owner-drawn messages on to the
// shell while a ShellMenu is open.
bool HandleMenuMessage(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT& result);

}  // namespace shelf
