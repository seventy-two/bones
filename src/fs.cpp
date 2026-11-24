#include "fs.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <memory>

using Microsoft::WRL::ComPtr;

namespace bones {

std::wstring Widen(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string Narrow(std::wstring_view w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string ErrorText(DWORD code) {
    wchar_t* msg = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, code, 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    std::string s = n ? Narrow(std::wstring_view(msg, n)) : "error " + std::to_string(code);
    if (msg) LocalFree(msg);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == '.' || s.back() == ' ')) s.pop_back();
    return s;
}

namespace {

std::wstring knownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    return out;
}

std::wstring makeDir(const std::wstring& dir) {
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return dir;
}

// longPath adds \\?\ so paths past MAX_PATH work.
std::wstring longPath(const std::wstring& p) {
    if (p.rfind(L"\\\\?\\", 0) == 0) return p;
    if (p.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + p.substr(2);
    return L"\\\\?\\" + p;
}

ComPtr<IShellItemArray> itemArray(const std::vector<std::wstring>& paths) {
    std::vector<PIDLIST_ABSOLUTE> pidls;
    for (const auto& p : paths) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(p.c_str(), nullptr, &pidl, 0, nullptr))) pidls.push_back(pidl);
    }
    ComPtr<IShellItemArray> arr;
    if (!pidls.empty())
        SHCreateShellItemArrayFromIDLists(UINT(pidls.size()), const_cast<PCIDLIST_ABSOLUTE_ARRAY>(pidls.data()), &arr);
    for (auto p : pidls) CoTaskMemFree(p);
    return arr;
}

ComPtr<IDataObject> dataObject(const std::vector<std::wstring>& paths) {
    ComPtr<IDataObject> obj;
    if (auto arr = itemArray(paths)) arr->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&obj));
    return obj;
}

UINT dropEffectFormat() {
    static UINT f = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    return f;
}

// runShellJob runs a file job on a thread of its own, set up for OLE, so
// the window keeps drawing while the shell's dialogs show.
void runShellJob(std::function<void()> job, std::function<void()> done) {
    std::thread([job = std::move(job), done = std::move(done)] {
        OleInitialize(nullptr);
        job();
        OleUninitialize();
        if (done) done();
    }).detach();
}

ComPtr<IFileOperation> fileOp(HWND owner, DWORD flags) {
    ComPtr<IFileOperation> op;
    if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op)))) return nullptr;
    op->SetOwnerWindow(owner);
    op->SetOperationFlags(flags);
    return op;
}

}  // namespace

std::wstring DataDir() {
    if (const wchar_t* d = _wgetenv(L"BONES_DATA"); d && *d) return makeDir(d);
    return makeDir(knownFolder(FOLDERID_RoamingAppData) + L"\\bones");
}

std::wstring CacheDir() {
    if (const wchar_t* d = _wgetenv(L"BONES_DATA"); d && *d) return makeDir(std::wstring(d) + L"\\cache");
    return makeDir(knownFolder(FOLDERID_LocalAppData) + L"\\bones");
}

bool ReadFileBytes(const std::wstring& path, std::string& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart < (LONGLONG(256) << 20);
    if (ok) {
        out.resize(size_t(size.QuadPart));
        DWORD got = 0;
        ok = out.empty() || (ReadFile(f, out.data(), DWORD(out.size()), &got, nullptr) && got == out.size());
    }
    CloseHandle(f);
    return ok;
}

bool WriteFileAtomic(const std::wstring& path, const std::string& data) {
    std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    bool ok = WriteFile(f, data.data(), DWORD(data.size()), &wrote, nullptr) && wrote == data.size();
    ok = FlushFileBuffers(f) && ok;
    CloseHandle(f);
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

bool IsDir(const std::wstring& path) {
    DWORD a = GetFileAttributesW(longPath(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool ListDir(const std::wstring& dir, std::vector<Entry>& out, DWORD& error) {
    out.clear();
    error = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(longPath(Join(dir, L"*")).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        // An empty drive root has no "." to find.
        return error == ERROR_FILE_NOT_FOUND;
    }
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        Entry e;
        e.name = fd.cFileName;
        e.utf8 = Narrow(e.name);
        e.dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!e.dir) e.ext = LowerExt(e.name);
        e.size = e.dir ? 0 : (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        e.created = (uint64_t(fd.ftCreationTime.dwHighDateTime) << 32) | fd.ftCreationTime.dwLowDateTime;
        e.modified = (uint64_t(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
        e.attrs = fd.dwFileAttributes;
        out.push_back(std::move(e));
    } while (FindNextFileW(h, &fd));
    DWORD last = GetLastError();
    FindClose(h);
    if (last != ERROR_NO_MORE_FILES) error = last;
    return true;
}

namespace {
uint64_t folderBytes(const std::wstring& dir) {
    uint64_t total = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) total += folderBytes(dir + L"\\" + n);
        } else {
            total += (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total;
}
}  // namespace

std::vector<Drive> DemoDrives(const std::wstring& media) {
    const uint64_t GB = 1000ull * 1000 * 1000;
    std::wstring root = media;
    if (root.size() == 2) root += L"\\";
    uint64_t used = folderBytes(root.substr(0, root.size() - 1));
    std::string letter = Narrow(root.substr(0, 2));
    return {
        {L"C:\\", "Windows (C:)", DRIVE_FIXED, 1000 * GB, 412 * GB},
        {L"G:\\", "Games (G:)", DRIVE_FIXED, 2000 * GB, 644 * GB},
        {root, "Media (" + letter + ")", DRIVE_FIXED, 4000 * GB, 4000 * GB - std::min(used, 4000 * GB)},
        {L"U:\\", "USB stick (U:)", DRIVE_REMOVABLE, 64 * GB, 41 * GB},
    };
}

std::vector<Drive> ListDrives() {
    std::vector<Drive> out;
    DWORD oldMode = 0;
    SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &oldMode);  // no "insert a disk"
    wchar_t buf[512];
    DWORD n = GetLogicalDriveStringsW(ARRAYSIZE(buf), buf);
    for (const wchar_t* p = buf; p < buf + n && *p; p += wcslen(p) + 1) {
        Drive d;
        d.root = p;
        d.type = GetDriveTypeW(p);
        wchar_t label[MAX_PATH + 1] = {};
        bool mounted = GetVolumeInformationW(p, label, ARRAYSIZE(label), nullptr, nullptr, nullptr, nullptr, 0);
        std::wstring name = label;
        if (name.empty()) {
            switch (d.type) {
            case DRIVE_REMOVABLE: name = L"USB Drive"; break;
            case DRIVE_CDROM: name = L"DVD Drive"; break;
            case DRIVE_REMOTE: name = L"Network Drive"; break;
            default: name = L"Local Disk"; break;
            }
        }
        ULARGE_INTEGER freeToUs{}, total{};
        if (mounted && GetDiskFreeSpaceExW(p, &freeToUs, &total, nullptr)) {
            d.total = total.QuadPart;
            d.free = freeToUs.QuadPart;
        }
        d.label = Narrow(name) + " (" + Narrow(std::wstring(p, 2)) + ")";
        out.push_back(std::move(d));
    }
    SetThreadErrorMode(oldMode, nullptr);
    return out;
}

std::vector<Distro> ListDistros() {
    std::vector<Distro> out;
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss", 0, KEY_READ, &key) !=
        ERROR_SUCCESS)
        return out;
    wchar_t def[64] = {};
    DWORD size = sizeof(def);
    RegGetValueW(key, nullptr, L"DefaultDistribution", RRF_RT_REG_SZ, nullptr, def, &size);
    wchar_t sub[64];
    for (DWORD i = 0;; i++) {
        DWORD len = ARRAYSIZE(sub);
        if (RegEnumKeyExW(key, i, sub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        wchar_t name[256] = {};
        size = sizeof(name);
        if (RegGetValueW(key, sub, L"DistributionName", RRF_RT_REG_SZ, nullptr, name, &size) != ERROR_SUCCESS) continue;
        Distro d;
        d.name = name;
        size = sizeof(d.uid);
        RegGetValueW(key, sub, L"DefaultUid", RRF_RT_REG_DWORD, nullptr, &d.uid, &size);
        d.isDefault = _wcsicmp(sub, def) == 0;
        out.push_back(std::move(d));
    }
    RegCloseKey(key);
    std::stable_sort(out.begin(), out.end(), [](const Distro& a, const Distro& b) { return a.isDefault > b.isDefault; });
    return out;
}

std::wstring DistroHome(const Distro& d) {
    if (d.uid == 0) return FromLinux(L"/root", d.name);
    std::string passwd;
    if (ReadFileBytes(FromLinux(L"/etc/passwd", d.name), passwd)) {
        size_t at = 0;
        while (at < passwd.size()) {
            size_t end = passwd.find('\n', at);
            if (end == std::string::npos) end = passwd.size();
            std::string line = passwd.substr(at, end - at);
            at = end + 1;
            // name:x:uid:gid:gecos:home:shell
            std::vector<std::string> f;
            size_t s = 0;
            for (size_t c; (c = line.find(':', s)) != std::string::npos; s = c + 1) f.push_back(line.substr(s, c - s));
            f.push_back(line.substr(s));
            if (f.size() >= 6 && f[2] == std::to_string(d.uid)) return FromLinux(Widen(f[5]), d.name);
        }
    }
    return FromLinux(L"/home", d.name);
}

std::wstring HomeDir() { return knownFolder(FOLDERID_Profile); }

std::vector<Place> KnownPlaces() {
    struct K {
        const char* label;
        const KNOWNFOLDERID* id;
        const char* icon;
    };
    // Segoe Fluent Icons: Home, Desktop (TVMonitor), Download, Document,
    // Pictures, Video, MusicNote.
    const K ks[] = {
        {"Home", &FOLDERID_Profile, "\xEE\xA0\x8F"},      {"Desktop", &FOLDERID_Desktop, "\xEE\x9F\xB4"},
        {"Downloads", &FOLDERID_Downloads, "\xEE\xA2\x96"}, {"Documents", &FOLDERID_Documents, "\xEE\xA2\xA5"},
        {"Pictures", &FOLDERID_Pictures, "\xEE\xA2\xB9"},  {"Videos", &FOLDERID_Videos, "\xEE\x9C\x94"},
        {"Music", &FOLDERID_Music, "\xEE\xB1\x8F"},
    };
    std::vector<Place> out;
    for (const auto& k : ks) {
        std::wstring p = knownFolder(*k.id);
        if (!p.empty()) out.push_back({k.label, p, k.icon});
    }
    return out;
}

void Watcher::Watch(const std::wstring& dir) {
    Stop();
    HANDLE note = FindFirstChangeNotificationW(
        longPath(dir).c_str(), FALSE,
        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE);
    if (note == INVALID_HANDLE_VALUE) return;  // some network shares can't be watched; F5 still works
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE stop = stop_;
    thread_ = std::thread([this, note, stop] {
        HANDLE both[2] = {stop, note};
        for (;;) {
            if (WaitForMultipleObjects(2, both, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) break;
            FindNextChangeNotification(note);
            // Let a burst (a copy, an unzip) settle, but report at least
            // every second so a long copy still shows progress.
            ULONGLONG start = GetTickCount64();
            bool stopping = false;
            while (GetTickCount64() - start < 1000) {
                DWORD r = WaitForMultipleObjects(2, both, FALSE, 250);
                if (r == WAIT_OBJECT_0) {
                    stopping = true;
                    break;
                }
                if (r == WAIT_TIMEOUT) break;
                FindNextChangeNotification(note);
            }
            if (stopping) break;
            changed_();
        }
        FindCloseChangeNotification(note);
    });
}

void Watcher::Stop() {
    if (stop_) SetEvent(stop_);
    if (thread_.joinable()) thread_.join();
    if (stop_) CloseHandle(stop_);
    stop_ = nullptr;
}

void OpenItem(HWND owner, const std::wstring& path) {
    std::wstring dir = Parent(path);
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_INVOKEIDLIST | SEE_MASK_ASYNCOK;
    sei.hwnd = owner;
    sei.lpFile = path.c_str();
    sei.lpDirectory = dir.c_str();
    sei.nShow = SW_SHOWNORMAL;
    ShellExecuteExW(&sei);
}

void OpenWith(HWND owner, const std::wstring& path) {
    OPENASINFO info{};
    info.pcszFile = path.c_str();
    info.oaifInFlags = OAIF_EXEC | OAIF_ALLOW_REGISTRATION;
    SHOpenWithDialog(owner, &info);
}

std::wstring LinkTarget(const std::wstring& lnk) {
    ComPtr<IShellLinkW> link;
    ComPtr<IPersistFile> file;
    wchar_t target[MAX_PATH * 2] = {};
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) ||
        FAILED(link.As(&file)) || FAILED(file->Load(lnk.c_str(), STGM_READ)) ||
        FAILED(link->GetPath(target, ARRAYSIZE(target), nullptr, 0)))
        return L"";
    return target;
}

void Properties(HWND owner, const std::vector<std::wstring>& paths) {
    if (paths.size() == 1) {
        SHObjectProperties(owner, SHOP_FILEPATH, paths[0].c_str(), nullptr);
    } else if (auto obj = dataObject(paths)) {
        SHMultiFileProperties(obj.Get(), 0);
    }
}

void CopyText(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) return;
    EmptyClipboard();
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    if (mem) {
        memcpy(GlobalLock(mem), text.c_str(), (text.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
    }
    CloseClipboard();
}

bool SetFileClipboard(const std::vector<std::wstring>& paths, bool cut) {
    auto obj = dataObject(paths);
    if (!obj) return false;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (!mem) return false;
    *static_cast<DWORD*>(GlobalLock(mem)) = cut ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
    GlobalUnlock(mem);
    FORMATETC fmt{CLIPFORMAT(dropEffectFormat()), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM med{};
    med.tymed = TYMED_HGLOBAL;
    med.hGlobal = mem;
    if (FAILED(obj->SetData(&fmt, &med, TRUE))) GlobalFree(mem);
    if (FAILED(OleSetClipboard(obj.Get()))) return false;
    OleFlushClipboard();  // stays on the clipboard after we close
    return true;
}

bool ClipboardHasFiles() { return IsClipboardFormatAvailable(CF_HDROP) != 0; }

void PasteInto(HWND owner, const std::wstring& dir, std::function<void()> done) {
    runShellJob(
        [owner, dir] {
            ComPtr<IDataObject> obj;
            if (FAILED(OleGetClipboard(&obj))) return;
            DWORD effect = DROPEFFECT_COPY;
            FORMATETC fmt{CLIPFORMAT(dropEffectFormat()), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            STGMEDIUM med{};
            if (SUCCEEDED(obj->GetData(&fmt, &med))) {
                if (auto* p = static_cast<DWORD*>(GlobalLock(med.hGlobal))) {
                    effect = *p;
                    GlobalUnlock(med.hGlobal);
                }
                ReleaseStgMedium(&med);
            }
            bool move = (effect & DROPEFFECT_MOVE) != 0;
            // Pasting a copy into its own folder makes "x - Copy", as in
            // Explorer; anywhere else a clash asks.
            DWORD flags = FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR;
            ComPtr<IShellItemArray> items;
            if (SUCCEEDED(SHCreateShellItemArrayFromDataObject(obj.Get(), IID_PPV_ARGS(&items)))) {
                ComPtr<IShellItem> first, parent;
                PWSTR path = nullptr;
                if (SUCCEEDED(items->GetItemAt(0, &first)) && SUCCEEDED(first->GetParent(&parent)) &&
                    SUCCEEDED(parent->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    if (SamePath(path, dir)) {
                        if (move) {
                            CoTaskMemFree(path);
                            return;  // cut and pasted where it already is
                        }
                        flags |= FOF_RENAMEONCOLLISION;
                    }
                }
                CoTaskMemFree(path);
            }
            ComPtr<IShellItem> dest;
            auto op = fileOp(owner, flags);
            if (!op || FAILED(SHCreateItemFromParsingName(dir.c_str(), nullptr, IID_PPV_ARGS(&dest)))) return;
            if (move) op->MoveItems(obj.Get(), dest.Get());
            else op->CopyItems(obj.Get(), dest.Get());
            op->PerformOperations();
            if (move) OleSetClipboard(nullptr);  // a cut pastes once
        },
        std::move(done));
}

void Recycle(HWND owner, const std::vector<std::wstring>& paths, bool permanently, std::function<void()> done) {
    runShellJob(
        [owner, paths, permanently] {
            auto items = itemArray(paths);
            auto op = fileOp(owner, permanently ? FOF_WANTNUKEWARNING : (FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE | FOF_WANTNUKEWARNING));
            if (!items || !op) return;
            op->DeleteItems(items.Get());
            op->PerformOperations();
        },
        std::move(done));
}

void CopyOrMove(HWND owner, const std::vector<std::wstring>& paths, const std::wstring& dir, bool move,
                std::function<void()> done) {
    runShellJob(
        [owner, paths, dir, move] {
            auto items = itemArray(paths);
            auto op = fileOp(owner, FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR);
            ComPtr<IShellItem> dest;
            if (!items || !op || FAILED(SHCreateItemFromParsingName(dir.c_str(), nullptr, IID_PPV_ARGS(&dest)))) return;
            if (move) op->MoveItems(items.Get(), dest.Get());
            else op->CopyItems(items.Get(), dest.Get());
            op->PerformOperations();
        },
        std::move(done));
}

std::string Rename(const std::wstring& from, const std::wstring& to) {
    if (from == to) return "";
    if (to.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos && !IsLinux(from))
        return "A name can't have any of \\ / : * ? \" < > |";
    if (!MoveFileExW(longPath(from).c_str(), longPath(to).c_str(), 0)) return ErrorText(GetLastError());
    return "";
}

std::string NewFolder(const std::wstring& path) {
    if (!CreateDirectoryW(longPath(path).c_str(), nullptr)) return ErrorText(GetLastError());
    return "";
}

void DragOut(HWND owner, const std::vector<std::wstring>& paths) {
    auto obj = dataObject(paths);
    if (!obj) return;
    DWORD effect = 0;
    SHDoDragDrop(owner, obj.Get(), nullptr, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
}

namespace {
IContextMenu2* menu2 = nullptr;
IContextMenu3* menu3 = nullptr;
}  // namespace

bool HandleMenuMessage(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT& result) {
    result = 0;
    if (menu3) return SUCCEEDED(menu3->HandleMenuMsg2(msg, wParam, lParam, &result));
    if (menu2) return SUCCEEDED(menu2->HandleMenuMsg(msg, wParam, lParam));
    return false;
}

std::string ShellMenu(HWND owner, const std::wstring& dir, const std::vector<std::wstring>& paths, POINT at) {
    ComPtr<IContextMenu> cm;
    if (paths.empty()) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (FAILED(SHParseDisplayName(dir.c_str(), nullptr, &pidl, 0, nullptr))) return "";
        ComPtr<IShellFolder> folder;
        if (SUCCEEDED(SHBindToObject(nullptr, pidl, nullptr, IID_PPV_ARGS(&folder))))
            folder->CreateViewObject(owner, IID_PPV_ARGS(&cm));
        CoTaskMemFree(pidl);
    } else {
        std::vector<PIDLIST_ABSOLUTE> pidls;
        for (const auto& p : paths) {
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (SUCCEEDED(SHParseDisplayName(p.c_str(), nullptr, &pidl, 0, nullptr))) pidls.push_back(pidl);
        }
        ComPtr<IShellFolder> parent;
        PCUITEMID_CHILD child = nullptr;
        if (!pidls.empty() && SUCCEEDED(SHBindToParent(pidls[0], IID_PPV_ARGS(&parent), &child))) {
            std::vector<PCUITEMID_CHILD> children;
            for (auto p : pidls) children.push_back(ILFindLastID(p));
            parent->GetUIObjectOf(owner, UINT(children.size()), children.data(), IID_IContextMenu, nullptr,
                                  reinterpret_cast<void**>(cm.GetAddressOf()));
        }
        for (auto p : pidls) CoTaskMemFree(p);
    }
    if (!cm) return "";

    HMENU menu = CreatePopupMenu();
    UINT flags = CMF_NORMAL | CMF_CANRENAME;
    if (GetKeyState(VK_SHIFT) < 0) flags |= CMF_EXTENDEDVERBS;
    std::string picked;
    if (SUCCEEDED(cm->QueryContextMenu(menu, 0, 1, 0x7FFF, flags))) {
        ComPtr<IContextMenu2> cm2;
        ComPtr<IContextMenu3> cm3;
        cm.As(&cm2);
        cm.As(&cm3);
        menu2 = cm2.Get();
        menu3 = cm3.Get();
        UINT cmd = UINT(TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, at.x, at.y, owner, nullptr));
        menu2 = nullptr;
        menu3 = nullptr;
        if (cmd > 0) {
            wchar_t verb[64] = {};
            if (FAILED(cm->GetCommandString(cmd - 1, GCS_VERBW, nullptr, reinterpret_cast<char*>(verb), ARRAYSIZE(verb))))
                verb[0] = 0;
            if (_wcsicmp(verb, L"rename") == 0) {
                picked = "rename";
            } else {
                CMINVOKECOMMANDINFOEX ici{sizeof(ici)};
                ici.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE | CMIC_MASK_ASYNCOK;
                if (GetKeyState(VK_CONTROL) < 0) ici.fMask |= CMIC_MASK_CONTROL_DOWN;
                if (GetKeyState(VK_SHIFT) < 0) ici.fMask |= CMIC_MASK_SHIFT_DOWN;
                ici.hwnd = owner;
                ici.lpVerb = MAKEINTRESOURCEA(cmd - 1);
                ici.lpVerbW = MAKEINTRESOURCEW(cmd - 1);
                ici.lpDirectoryW = dir.c_str();
                ici.nShow = SW_SHOWNORMAL;
                ici.ptInvoke = at;
                cm->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&ici));
            }
        }
    }
    DestroyMenu(menu);
    return picked;
}

}  // namespace bones
