#include "app.h"

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <thread>

#include "imgui_internal.h"  // TableSetColumnSortDirection
#include "nlohmann/json.hpp"

namespace shelf {

using json = nlohmann::json;

namespace {

ULONGLONG now() { return GetTickCount64(); }

std::string formatTime(uint64_t ft) {
    if (!ft) return "";
    FILETIME f{DWORD(ft), DWORD(ft >> 32)};
    SYSTEMTIME utc, local;
    if (!FileTimeToSystemTime(&f, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return "";
    char buf[32];
    snprintf(buf, sizeof buf, "%02d/%02d/%04d %02d:%02d", local.wDay, local.wMonth, local.wYear, local.wHour, local.wMinute);
    return buf;
}

uint64_t fileTimeNow() {
    FILETIME f;
    GetSystemTimeAsFileTime(&f);
    return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime;
}

const char* sortName(SortKey k) {
    switch (k) {
    case SortKey::Name: return "Name";
    case SortKey::Modified: return "Date modified";
    case SortKey::Created: return "Date created";
    case SortKey::Type: return "Type";
    case SortKey::Size: return "Size";
    }
    return "";
}

const char* driveIcon(UINT type) {
    switch (type) {
    case DRIVE_REMOVABLE: return icon::Usb;
    case DRIVE_CDROM: return icon::Disc;
    case DRIVE_REMOTE: return icon::Network;
    default: return icon::Drive;
    }
}

// ellipsis cuts text to fit a width, ending in "…".
std::string ellipsis(const std::string& s, float width) {
    if (ImGui::CalcTextSize(s.c_str()).x <= width) return s;
    const char* dots = "\xE2\x80\xA6";
    float dw = ImGui::CalcTextSize(dots).x;
    size_t lo = 0, hi = s.size();
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        while (mid > 0 && (s[mid] & 0xC0) == 0x80) mid--;  // whole characters
        if (mid <= lo) break;
        if (ImGui::CalcTextSize(s.c_str(), s.c_str() + mid).x + dw <= width) lo = mid;
        else hi = mid - 1;
    }
    while (lo > 0 && (s[lo] & 0xC0) == 0x80) lo--;
    return s.substr(0, lo) + dots;
}

// splitTwo splits a name over at most two lines of a width, breaking
// after a space, dot, dash or underscore where it can; the second line
// ends in "…" if it still doesn't fit.
std::pair<std::string, std::string> splitTwo(const std::string& s, float width) {
    if (ImGui::CalcTextSize(s.c_str()).x <= width) return {s, ""};
    size_t fit = 0;
    for (size_t i = 1; i <= s.size(); i++) {
        if (i < s.size() && (s[i] & 0xC0) == 0x80) continue;
        if (ImGui::CalcTextSize(s.c_str(), s.c_str() + i).x > width) break;
        fit = i;
    }
    if (fit == 0) fit = 1;
    size_t brk = fit;
    for (size_t i = fit; i > fit / 2; i--) {
        char c = s[i - 1];
        if (c == ' ' || c == '.' || c == '-' || c == '_') {
            brk = i;
            break;
        }
    }
    return {s.substr(0, brk), ellipsis(s.substr(brk), width)};
}

}  // namespace

App::App(HWND hwnd, ID3D11Device* device, std::function<void()> wake)
    : hwnd_(hwnd), device_(device), wake_(std::move(wake)),
      watcher_([this] {
          shared_->dirty = true;
          wake_();
      }) {
    fonts_ = LoadFonts();
    thumbs_ = std::make_unique<Thumbs>(device_, wake_);
    icons_ = std::make_unique<Icons>(device_);
    distros_ = ListDistros();
    load();
    DrivesChanged();
}

App::~App() {
    watcher_.Stop();
    thumbs_.reset();
}

void App::SetScale(float scale) {
    scale_ = scale;
    ApplyTheme(scale);
}

void App::SetSort(SortKey key, bool descending) {
    sort_.key = key;
    sort_.descending = descending;
    sortApplied_ = false;
    rebuild();
}

void App::SelectName(const std::wstring& name) { arriveFocus_ = name; }

// ---- state ----

void App::load() {
    std::string bytes;
    json j;
    if (ReadFileBytes(DataDir() + L"\\state.json", bytes)) j = json::parse(bytes, nullptr, false);
    if (!j.is_object()) j = json::object();
    thumbView_ = j.value("view", "thumbs") == "thumbs";
    tile_ = std::clamp(j.value("tile", 150.0f), 72.0f, 320.0f);
    sort_.key = SortKey(std::clamp(j.value("sortKey", 0), 0, 4));
    sort_.descending = j.value("sortDescending", false);
    sort_.foldersFirst = j.value("foldersFirst", true);
    showHidden_ = j.value("hidden", false);
    if (j.contains("pins") && j["pins"].is_array()) {
        for (auto& p : j["pins"])
            if (p.is_object()) pins_.push_back({p.value("label", ""), Widen(p.value("path", ""))});
    } else {
        // The first run: the usual places.
        for (auto& k : KnownPlaces())
            if (k.label != "Music") pins_.push_back({k.label, k.path});
    }
    std::string last = j.value("last", "");
    if (!last.empty()) cur_ = Widen(last);  // main navigates there
}

void App::Save() {
    json j;
    j["view"] = thumbView_ ? "thumbs" : "details";
    j["tile"] = tile_;
    j["sortKey"] = int(sort_.key);
    j["sortDescending"] = sort_.descending;
    j["foldersFirst"] = sort_.foldersFirst;
    j["hidden"] = showHidden_;
    j["last"] = Narrow(cur_);
    j["pins"] = json::array();
    for (auto& p : pins_) j["pins"].push_back({{"label", p.label}, {"path", Narrow(p.path)}});
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (GetWindowPlacement(hwnd_, &wp)) {
        RECT r = wp.rcNormalPosition;
        j["window"] = {{"x", r.left}, {"y", r.top}, {"w", r.right - r.left}, {"h", r.bottom - r.top},
                       {"maximised", wp.showCmd == SW_SHOWMAXIMIZED}};
    }
    WriteFileAtomic(DataDir() + L"\\state.json", j.dump(2));
}

bool App::Placement(RECT& r, bool& maximised) {
    std::string bytes;
    if (!ReadFileBytes(DataDir() + L"\\state.json", bytes)) return false;
    auto j = json::parse(bytes, nullptr, false);
    if (!j.is_object() || !j.contains("window") || !j["window"].is_object()) return false;
    auto& w = j["window"];
    int x = w.value("x", 0), y = w.value("y", 0);
    r = {x, y, x + w.value("w", 0), y + w.value("h", 0)};
    maximised = w.value("maximised", false);
    return r.right > r.left && r.bottom > r.top;
}

// ---- navigating and listing ----

void App::Start(const std::wstring& path) {
    std::wstring last = cur_;
    cur_.clear();
    std::wstring first = !path.empty() ? path : !last.empty() && IsDir(last) ? last : HomeDir();
    go(first, false, L"", -1);
}

void App::Navigate(std::wstring path, bool record) { go(std::move(path), record, L"", -1); }

void App::go(std::wstring path, bool record, std::wstring focus, float scroll) {
    path = Normalise(path);
    if (path.empty()) return;
    if (record && !cur_.empty() && !SamePath(path, cur_)) {
        std::wstring f = focus_ >= 0 && focus_ < int(view_.size()) ? at(focus_).name : L"";
        back_.push_back({cur_, f, scrollY_});
        fwd_.clear();
    }
    cur_ = path;
    arriveFocus_ = std::move(focus);
    arriveScroll_ = scroll;
    filter_[0] = 0;
    selected_.clear();
    focus_ = anchor_ = pressed_ = -1;
    renaming_.clear();
    banding_ = false;
    editingPath_ = false;
    entries_.clear();
    reindex();
    rebuild();
    listError_.clear();
    thumbs_->Drop();
    watcher_.Watch(cur_);
    list(false);
}

void App::Back() {
    if (back_.empty()) return;
    Visit v = back_.back();
    back_.pop_back();
    std::wstring f = focus_ >= 0 && focus_ < int(view_.size()) ? at(focus_).name : L"";
    fwd_.push_back({cur_, f, scrollY_});
    go(v.path, false, v.focus, v.scroll);
}

void App::Forward() {
    if (fwd_.empty()) return;
    Visit v = fwd_.back();
    fwd_.pop_back();
    std::wstring f = focus_ >= 0 && focus_ < int(view_.size()) ? at(focus_).name : L"";
    back_.push_back({cur_, f, scrollY_});
    go(v.path, false, v.focus, v.scroll);
}

void App::up() {
    std::wstring parent = Parent(cur_);
    if (!parent.empty()) go(parent, true, BaseName(cur_), -1);
}

void App::list(bool quiet) {
    int gen = ++gen_;
    if (!quiet) {
        loading_ = true;
        loadStart_ = now();
    }
    std::thread([shared = shared_, wake = wake_, gen, dir = cur_] {
        Listing l;
        l.gen = gen;
        l.dir = dir;
        l.ok = ListDir(dir, l.entries, l.error);
        {
            std::lock_guard<std::mutex> lock(shared->mu);
            shared->listings.push_back(std::move(l));
        }
        wake();
    }).detach();
}

void App::took(Listing&& l) {
    loading_ = false;
    // Keep focus and anchor by name through the new listing.
    auto nameAt = [&](int vi) { return vi >= 0 && vi < int(view_.size()) ? lower_[view_[vi]] : std::wstring(); };
    std::wstring focusName = nameAt(focus_), anchorName = nameAt(anchor_);
    focus_ = anchor_ = -1;
    if (!l.ok) {
        listError_ = ErrorText(l.error);
        entries_.clear();
    } else {
        listError_.clear();
        entries_ = std::move(l.entries);
    }
    reindex();
    // Forget selected names that have gone.
    for (auto it = selected_.begin(); it != selected_.end();) {
        if (byName_.count(*it)) ++it;
        else it = selected_.erase(it);
    }
    if (!renaming_.empty() && !byName_.count(Lower(renaming_))) renaming_.clear();
    rebuild();
    if (!focusName.empty()) focus_ = indexOf(focusName);
    if (!anchorName.empty()) anchor_ = indexOf(anchorName);
    if (!arriveFocus_.empty()) {
        int vi = indexOf(arriveFocus_);
        if (vi >= 0) {
            selectOnly(vi);
            if (arriveScroll_ < 0) scrollToFocus_ = true;
        }
        arriveFocus_.clear();
    }
}

void App::reindex() {
    lower_.resize(entries_.size());
    types_.resize(entries_.size());
    byName_.clear();
    byName_.reserve(entries_.size());
    for (size_t i = 0; i < entries_.size(); i++) {
        lower_[i] = Lower(entries_[i].name);
        types_[i] = icons_->TypeName(entries_[i]);
        byName_[lower_[i]] = int(i);
    }
}

bool App::hidden(const Entry& e) const {
    if (showHidden_) return false;
    if (e.attrs & FILE_ATTRIBUTE_HIDDEN) return true;
    return !e.name.empty() && e.name[0] == L'.' && IsLinux(cur_);
}

void App::rebuild() {
    auto nameAt = [&](int vi) {
        return vi >= 0 && vi < int(view_.size()) && size_t(view_[vi]) < lower_.size() ? lower_[view_[vi]] : std::wstring();
    };
    std::wstring focusName = nameAt(focus_), anchorName = nameAt(anchor_);
    std::wstring filter = Widen(filter_);
    view_.clear();
    for (int i = 0; i < int(entries_.size()); i++)
        if (!hidden(entries_[i]) && Matches(entries_[i].name, filter)) view_.push_back(i);
    Order(entries_, types_, sort_, view_);
    viewPos_.assign(entries_.size(), -1);
    for (int vi = 0; vi < int(view_.size()); vi++) viewPos_[view_[vi]] = vi;
    focus_ = focusName.empty() ? -1 : indexOf(focusName);
    anchor_ = anchorName.empty() ? -1 : indexOf(anchorName);
}

std::wstring App::pathOf(int vi) const { return Join(cur_, at(vi).name); }

int App::indexOf(const std::wstring& name) const {
    auto it = byName_.find(Lower(name));
    if (it == byName_.end() || it->second >= int(viewPos_.size())) return -1;
    return viewPos_[it->second];
}

bool App::isSelected(int vi) const { return !selected_.empty() && selected_.count(lower_[view_[vi]]) > 0; }

void App::selectOnly(int vi) {
    selected_.clear();
    if (vi < 0 || vi >= int(view_.size())) return;
    selected_.insert(lower_[view_[vi]]);
    anchor_ = focus_ = vi;
}

void App::selectRange(int a, int b, bool add) {
    if (!add) selected_.clear();
    if (a < 0) a = b;
    for (int i = std::min(a, b); i <= std::max(a, b); i++)
        if (i >= 0 && i < int(view_.size())) selected_.insert(lower_[view_[i]]);
}

void App::toggle(int vi) {
    const std::wstring& n = lower_[view_[vi]];
    if (!selected_.erase(n)) selected_.insert(n);
}

std::vector<int> App::selectedIndices() const {
    std::vector<int> out;
    for (const auto& n : selected_) {
        auto it = byName_.find(n);
        if (it != byName_.end() && viewPos_[it->second] >= 0) out.push_back(viewPos_[it->second]);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::wstring> App::selectedPaths() const {
    std::vector<std::wstring> out;
    for (int vi : selectedIndices()) out.push_back(pathOf(vi));
    return out;
}

void App::focusOn(int vi, bool extend) {
    if (view_.empty()) return;
    vi = std::clamp(vi, 0, int(view_.size()) - 1);
    if (extend) {
        selectRange(anchor_ < 0 ? vi : anchor_, vi, false);
        if (anchor_ < 0) anchor_ = vi;
    } else {
        selectOnly(vi);
    }
    focus_ = vi;
    scrollToFocus_ = true;
}

void App::DrivesChanged() {
    std::thread([shared = shared_, wake = wake_] {
        auto drives = ListDrives();
        {
            std::lock_guard<std::mutex> lock(shared->mu);
            shared->drives = std::move(drives);
            shared->drivesNew = true;
        }
        wake();
    }).detach();
}

void App::Pump() {
    std::vector<Listing> got;
    {
        std::lock_guard<std::mutex> lock(shared_->mu);
        got.swap(shared_->listings);
        if (shared_->drivesNew) {
            drives_ = shared_->drives;
            shared_->drivesNew = false;
        }
    }
    for (auto& l : got)
        if (l.gen == gen_ && SamePath(l.dir, cur_)) took(std::move(l));
    if (shared_->dirty.exchange(false) && !loading_) list(true);
    thumbs_->Pump();

    // Things with modal loops of their own run here, between frames.
    Later later = later_;
    later_ = Later::None;
    if (later == Later::None) return;
    ImGuiIO& io = ImGui::GetIO();
    if (later == Later::Drag) {
        auto paths = selectedPaths();
        if (!paths.empty()) DragOut(hwnd_, paths);
    } else {
        POINT pt;
        GetCursorPos(&pt);
        std::vector<std::wstring> paths;
        if (later == Later::ItemMenu) paths = selectedPaths();
        std::string picked = ShellMenu(hwnd_, cur_, paths, pt);
        if (picked == "rename" && focus_ >= 0) startRename(focus_);
    }
    // The mouse was let go inside the drag or menu, where ImGui didn't see it.
    io.AddMouseButtonEvent(0, false);
    io.AddMouseButtonEvent(1, false);
    list(true);
}

// ---- actions ----

void App::open(int vi) {
    const Entry& e = at(vi);
    std::wstring path = pathOf(vi);
    if (e.dir) {
        Navigate(path);
        return;
    }
    if (e.ext == L".lnk") {
        std::wstring target = LinkTarget(path);
        if (!target.empty() && IsDir(target)) {
            Navigate(target);
            return;
        }
    }
    OpenItem(hwnd_, path);
}

void App::openSelection() {
    auto sel = selectedIndices();
    if (sel.empty()) return;
    if (sel.size() == 1) {
        open(sel[0]);
        return;
    }
    int opened = 0;
    for (int vi : sel)
        if (!at(vi).dir && opened++ < 20) OpenItem(hwnd_, pathOf(vi));
}

void App::startRename(int vi) {
    if (vi < 0 || vi >= int(view_.size())) return;
    selectOnly(vi);
    const Entry& e = at(vi);
    renaming_ = e.name;
    snprintf(renameBuf_, sizeof renameBuf_, "%s", e.utf8.c_str());
    size_t dot = e.utf8.find_last_of('.');
    renameStem_ = int(e.dir || dot == std::string::npos || dot == 0 ? e.utf8.size() : dot);
    renameStart_ = true;
    scrollToFocus_ = true;
}

void App::finishRename(bool keep) {
    std::wstring from = renaming_;
    renaming_.clear();
    std::wstring to = Widen(renameBuf_);
    while (!to.empty() && (to.back() == L' ' || to.back() == L'.')) to.pop_back();
    while (!to.empty() && to.front() == L' ') to.erase(to.begin());
    if (!keep || to.empty() || to == from) return;
    std::string err = Rename(Join(cur_, from), Join(cur_, to));
    if (!err.empty()) {
        flash("Couldn't rename: " + err, true);
        return;
    }
    auto it = byName_.find(Lower(from));
    if (it != byName_.end()) {
        Entry& e = entries_[it->second];
        e.name = to;
        e.utf8 = Narrow(to);
        if (!e.dir) e.ext = LowerExt(to);
        reindex();
        rebuild();
    }
    int vi = indexOf(to);
    if (vi >= 0) {
        selectOnly(vi);
        scrollToFocus_ = true;
    }
}

void App::newFolder() {
    if (cur_.empty()) return;
    std::wstring name = UniqueName(entries_, L"New folder");
    std::string err = NewFolder(Join(cur_, name));
    if (!err.empty()) {
        flash("Couldn't make a folder: " + err, true);
        return;
    }
    Entry e;
    e.name = name;
    e.utf8 = Narrow(name);
    e.dir = true;
    e.attrs = FILE_ATTRIBUTE_DIRECTORY;
    e.created = e.modified = fileTimeNow();
    entries_.push_back(std::move(e));
    filter_[0] = 0;
    reindex();
    rebuild();
    startRename(indexOf(name));
}

void App::copySelection(bool cut) {
    auto paths = selectedPaths();
    if (paths.empty()) return;
    if (!SetFileClipboard(paths, cut)) {
        flash("Couldn't use the clipboard", true);
        return;
    }
    char buf[64];
    snprintf(buf, sizeof buf, "%s %zu item%s", cut ? "Cut" : "Copied", paths.size(), paths.size() == 1 ? "" : "s");
    flash(buf);
}

void App::paste(const std::wstring& into) {
    if (!ClipboardHasFiles()) return;
    PasteInto(hwnd_, into, [shared = shared_, wake = wake_] {
        shared->dirty = true;
        wake();
    });
}

void App::recycle(bool permanently) {
    auto paths = selectedPaths();
    if (paths.empty()) return;
    Recycle(hwnd_, paths, permanently, [shared = shared_, wake = wake_] {
        shared->dirty = true;
        wake();
    });
}

void App::copyPaths(bool linux) {
    auto paths = selectedPaths();
    if (paths.empty()) paths.push_back(cur_);
    std::wstring text;
    for (auto& p : paths) {
        if (!text.empty()) text += L"\r\n";
        text += linux ? ToLinux(p) : p;
    }
    CopyText(hwnd_, text);
    flash(paths.size() == 1 ? "Copied the path" : "Copied the paths");
}

bool App::pinned(const std::wstring& path) const {
    for (auto& p : pins_)
        if (SamePath(p.path, path)) return true;
    return false;
}

void App::pin(const std::wstring& path) {
    if (pinned(path)) return;
    std::wstring name = BaseName(path);
    if (IsRoot(path)) {
        auto crumbs = Crumbs(path);
        if (!crumbs.empty()) name = crumbs[0].label;
    }
    pins_.push_back({Narrow(name), Normalise(path)});
    Save();
}

void App::unpin(const std::wstring& path) {
    pins_.erase(std::remove_if(pins_.begin(), pins_.end(), [&](const Pin& p) { return SamePath(p.path, path); }), pins_.end());
    Save();
}

void App::openTerminal() {
    HINSTANCE r;
    if (IsLinux(cur_)) {
        std::wstring root = Root(cur_);
        std::wstring distro = root.substr(16, root.size() - 17);  // the distro, from the share name
        std::wstring args = L"-d " + distro + L" --cd \"" + ToLinux(cur_) + L"\"";
        r = ShellExecuteW(hwnd_, nullptr, L"wt.exe", (L"wsl.exe " + args).c_str(), nullptr, SW_SHOWNORMAL);
        if (INT_PTR(r) <= 32) ShellExecuteW(hwnd_, nullptr, L"wsl.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    } else {
        r = ShellExecuteW(hwnd_, nullptr, L"wt.exe", (L"-d \"" + cur_ + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        if (INT_PTR(r) <= 32) ShellExecuteW(hwnd_, nullptr, L"cmd.exe", nullptr, cur_.c_str(), SW_SHOWNORMAL);
    }
}

void App::flash(const std::string& text, bool bad) {
    message_ = text;
    messageBad_ = bad;
    messageAt_ = now();
}

void App::Dropped(const std::vector<std::wstring>& paths) {
    if (paths.empty() || cur_.empty()) return;
    bool allHere = true;
    for (auto& p : paths) allHere = allHere && SamePath(Parent(p), cur_);
    if (allHere) return;  // dragged out and back onto the same folder
    // As Explorer: move on the same drive, copy across drives; Ctrl
    // copies, Shift moves.
    bool move = SamePath(Root(paths[0]), Root(cur_));
    if (GetKeyState(VK_CONTROL) < 0) move = false;
    if (GetKeyState(VK_SHIFT) < 0) move = true;
    CopyOrMove(hwnd_, paths, cur_, move, [shared = shared_, wake = wake_] {
        shared->dirty = true;
        wake();
    });
}

// ---- drawing ----

void App::Frame() {
    const Palette& p = Colours();
    thumbs_->NewFrame();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("shelf", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);

    drawToolbar();
    float statusH = std::round(ImGui::GetTextLineHeight() + 12 * scale_);
    float bodyH = ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - statusH - ImGui::GetCursorScreenPos().y;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, p.surface);
    ImGui::BeginChild("side", ImVec2(230 * scale_, bodyH), 0);
    drawSidebar();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 0);

    ImGui::BeginChild("body", ImVec2(0, bodyH), 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (thumbView_) drawGrid();
    else drawDetails();
    drawEmpty();
    ImGui::EndChild();

    drawStatus();
    drawMenus();
    keys();

    // A drag out of the window starts once the mouse has moved a little.
    if (dragArmed_) {
        if (!ImGui::IsMouseDown(0)) {
            dragArmed_ = false;
        } else if (ImGui::IsMouseDragging(0, 6 * scale_)) {
            dragArmed_ = false;
            pressed_ = -1;
            later_ = Later::Drag;
        }
    }
    ImGui::End();
}

// ---- the title bar: name, navigation, path, filter, sort, size, view,
// window buttons. Its empty parts drag the window (see InCaption).

bool App::InCaption(float x, float y) const {
    if (y >= captionH_) return false;
    for (const auto& r : noDrag_)
        if (x >= r.first.x && y >= r.first.y && x < r.second.x && y < r.second.y) return false;
    return true;
}

bool App::iconButton(const char* id, const char* icon, const char* tip, bool enabled, ImVec2 at, ImVec2 size, bool on) {
    const Palette& p = Colours();
    ImGui::SetCursorScreenPos(at);
    ImGui::BeginDisabled(!enabled);
    bool hit = ImGui::InvisibleButton(id, size);
    ImGui::EndDisabled();
    bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    noDrag_.push_back({at, ImVec2(at.x + size.x, at.y + size.y)});
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 b(at.x + size.x, at.y + size.y);
    if (on) dl->AddRectFilled(at, b, U32(p.chip), 6 * scale_);
    else if (hovered && enabled) dl->AddRectFilled(at, b, U32(ImGui::IsItemActive() ? p.border : p.surface2), 6 * scale_);
    ImVec4 col = !enabled ? p.border : on || hovered ? p.text : p.muted;
    DrawCentred(dl, ImVec2(at.x + size.x / 2, at.y + size.y / 2), icon, U32(col));
    if (hovered && tip) {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", tip);
    }
    return hit && enabled;
}

void App::drawToolbar() {
    const Palette& p = Colours();
    float s = scale_;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetWindowPos();
    float W = ImGui::GetWindowWidth();
    captionH_ = std::round(46 * s);
    noDrag_.clear();
    float ctrl = std::round(30 * s);
    float y0 = origin.y + std::round((captionH_ - ctrl) / 2);
    float mid = y0 + ctrl / 2;
    bool active = GetForegroundWindow() == hwnd_;

    // Windows 10 draws no line along the top of a frameless window.
    if (!IsZoomed(hwnd_)) ImGui::GetBackgroundDrawList()->AddLine(ImVec2(0, 0.5f), ImVec2(W, 0.5f), U32(p.border));

    // The name.
    float x = origin.x + 16 * s;
    ImGui::PushFont(fonts_.semibold, kTitle);
    dl->AddText(ImVec2(x, TextTop(mid)), U32(active ? p.accent : p.muted), "shelf");
    x += ImGui::CalcTextSize("shelf").x + 18 * s;
    ImGui::PopFont();

    // Back, forward, up, refresh.
    ImVec2 sq(ctrl, ctrl);
    if (iconButton("##back", icon::Back, "Back (Alt+Left)", !back_.empty(), ImVec2(x, y0), sq)) Back();
    x += ctrl + 2 * s;
    if (iconButton("##fwd", icon::Forward, "Forward (Alt+Right)", !fwd_.empty(), ImVec2(x, y0), sq)) Forward();
    x += ctrl + 2 * s;
    if (iconButton("##up", icon::Up, "Up (Backspace)", !Parent(cur_).empty(), ImVec2(x, y0), sq)) up();
    x += ctrl + 2 * s;
    if (iconButton("##refresh", icon::Refresh, "Refresh (F5)", true, ImVec2(x, y0), sq)) refresh();
    x += ctrl + 10 * s;

    // From the right: window buttons, view, size, sort, filter.
    float right = origin.x + W - windowButtons();
    float gap = 8 * s;
    float viewW = ctrl * 2 + 6 * s, sliderW = 96 * s, sortW = 150 * s, filterW = 220 * s;
    float rx = right - 10 * s - viewW;
    float viewX = rx;
    float sliderX = 0, sortX = 0;
    if (thumbView_) {
        rx -= gap + sliderW;
        sliderX = rx;
        rx -= gap + sortW;
        sortX = rx;
    }
    rx -= gap + filterW;
    float filterX = rx;
    float crumbsW = std::max(filterX - gap - x, 120 * s);
    drawCrumbs(ImVec2(x, y0), crumbsW, ctrl);

    // Filter: the box with a magnifier drawn into its padding.
    {
        ImGui::SetCursorScreenPos(ImVec2(filterX, y0));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(32 * s, std::ceil((ctrl - ImGui::GetFontSize()) / 2)));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6 * s);
        ImGui::SetNextItemWidth(filterW);
        if (focusFilter_) {
            ImGui::SetKeyboardFocusHere();
            focusFilter_ = false;
        }
        if (ImGui::InputTextWithHint("##filter", "Filter", filter_, sizeof filter_)) {
            rebuild();
            if (!view_.empty() && filter_[0]) focusOn(0, false);
        }
        bool focused = ImGui::IsItemActive();
        if (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            filter_[0] = 0;
            rebuild();
        }
        ImGui::PopStyleVar(2);
        noDrag_.push_back({ImVec2(filterX, y0), ImVec2(filterX + filterW, y0 + ctrl)});
        DrawCentred(dl, ImVec2(filterX + 17 * s, mid), icon::Search, U32(focused || filter_[0] ? p.text : p.muted));
    }

    if (thumbView_) {
        // Sort: a button that opens its menu.
        ImGui::SetCursorScreenPos(ImVec2(sortX, y0));
        if (ImGui::InvisibleButton("##sort", ImVec2(sortW, ctrl))) ImGui::OpenPopup("##sortmenu");
        bool hov = ImGui::IsItemHovered();
        noDrag_.push_back({ImVec2(sortX, y0), ImVec2(sortX + sortW, y0 + ctrl)});
        dl->AddRectFilled(ImVec2(sortX, y0), ImVec2(sortX + sortW, y0 + ctrl), U32(hov ? p.border : p.surface2), 6 * s);
        DrawCentred(dl, ImVec2(sortX + 17 * s, mid), icon::Sort, U32(p.muted));
        std::string label = std::string(sortName(sort_.key)) + (sort_.descending ? "  \xE2\x86\x93" : "  \xE2\x86\x91");
        dl->AddText(ImVec2(sortX + 32 * s, TextTop(mid)), U32(p.text), label.c_str());
        ImGui::PushFont(nullptr, 10);
        DrawCentred(dl, ImVec2(sortX + sortW - 15 * s, mid), icon::Down, U32(p.muted));
        ImGui::PopFont();
        ImGui::SetNextWindowPos(ImVec2(sortX, y0 + ctrl + 4 * s));
        ImGui::SetNextWindowSizeConstraints(ImVec2(sortW, 0), ImVec2(FLT_MAX, FLT_MAX));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6 * s, 6 * s));
        if (ImGui::BeginPopup("##sortmenu")) {
            for (int k = 0; k < 5; k++)
                if (ImGui::MenuItem(sortName(SortKey(k)), nullptr, int(sort_.key) == k)) SetSort(SortKey(k), sort_.descending);
            ImGui::Separator();
            if (ImGui::MenuItem("Ascending", nullptr, !sort_.descending)) SetSort(sort_.key, false);
            if (ImGui::MenuItem("Descending", nullptr, sort_.descending)) SetSort(sort_.key, true);
            ImGui::Separator();
            if (ImGui::MenuItem("Folders first", nullptr, sort_.foldersFirst)) {
                sort_.foldersFirst = !sort_.foldersFirst;
                rebuild();
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();

        // Thumbnail size: a thin track and a round knob.
        ImGui::SetCursorScreenPos(ImVec2(sliderX, y0));
        ImGui::InvisibleButton("##tile", ImVec2(sliderW, ctrl));
        bool sh = ImGui::IsItemHovered(), sa = ImGui::IsItemActive();
        noDrag_.push_back({ImVec2(sliderX, y0), ImVec2(sliderX + sliderW, y0 + ctrl)});
        float pad = 9 * s, t0 = sliderX + pad, t1 = sliderX + sliderW - pad;
        if (sa) tile_ = std::clamp(72 + (ImGui::GetIO().MousePos.x - t0) / (t1 - t0) * (320 - 72), 72.0f, 320.0f);
        float f = (tile_ - 72) / (320 - 72);
        float kx = t0 + (t1 - t0) * f;
        dl->AddRectFilled(ImVec2(t0, mid - 2 * s), ImVec2(t1, mid + 2 * s), U32(p.border), 2 * s);
        dl->AddRectFilled(ImVec2(t0, mid - 2 * s), ImVec2(kx, mid + 2 * s), U32(p.accent, 0.7f), 2 * s);
        dl->AddCircleFilled(ImVec2(kx, mid), (sh || sa ? 7.5f : 6.5f) * s, U32(sh || sa ? p.text : p.accent));
        if (sh && !sa && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("Thumbnail size (Ctrl+scroll)");
    }

    // View: one box, the chosen half lit.
    dl->AddRectFilled(ImVec2(viewX, y0), ImVec2(viewX + viewW, y0 + ctrl), U32(p.surface2), 7 * s);
    float inset = 3 * s, half = (viewW - inset * 2) / 2;
    ImVec2 hs(half, ctrl - inset * 2);
    if (iconButton("##thumbs", icon::Grid, "Thumbnails (Ctrl+1)", true, ImVec2(viewX + inset, y0 + inset), hs, thumbView_))
        thumbView_ = true;
    if (iconButton("##details", icon::List, "Details (Ctrl+2)", true, ImVec2(viewX + inset + half, y0 + inset), hs, !thumbView_)) {
        thumbView_ = false;
        sortApplied_ = false;
    }

    dl->AddLine(ImVec2(origin.x, origin.y + captionH_ - 0.5f), ImVec2(origin.x + W, origin.y + captionH_ - 0.5f), U32(p.border));
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + captionH_));
}

// windowButtons draws minimise, maximise and close on the right of the
// title bar, full height, and returns their width.
float App::windowButtons() {
    const Palette& p = Colours();
    const ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetWindowPos();
    float w = std::round(46 * scale_), h = captionH_ - 1;
    float x = origin.x + ImGui::GetWindowWidth() - 3 * w;
    bool active = GetForegroundWindow() == hwnd_;
    bool zoomed = IsZoomed(hwnd_);
    const char* glyphs[] = {icon::Minimise, zoomed ? icon::Restore : icon::Maximise, icon::Close};
    const char* tips[] = {"Minimise", zoomed ? "Restore down" : "Maximise", "Close"};
    ImGui::PushFont(nullptr, 10);
    for (int i = 0; i < 3; i++, x += w) {
        ImVec2 a(x, origin.y), b(x + w, origin.y + h);
        noDrag_.push_back({a, b});
        bool hovered = io.MousePos.x >= a.x && io.MousePos.x < b.x && io.MousePos.y >= a.y && io.MousePos.y < b.y &&
                       ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        auto inside = [&](ImVec2 m) { return m.x >= a.x && m.x < b.x && m.y >= a.y && m.y < b.y; };
        bool held = hovered && io.MouseDown[0] && inside(io.MouseClickedPos[0]);
        bool clicked = hovered && io.MouseReleased[0] && inside(io.MouseClickedPos[0]);
        ImVec4 glyph = active ? p.text : p.muted;
        if (i == 2 && hovered) {
            dl->AddRectFilled(a, b, held ? IM_COL32(0xC4, 0x2B, 0x1C, 0xE6) : IM_COL32(0xC4, 0x2B, 0x1C, 0xFF));
            glyph = ImVec4(1, 1, 1, 1);
        } else if (hovered) {
            dl->AddRectFilled(a, b, U32(ImVec4(1, 1, 1, held ? 0.12f : 0.07f)));
            glyph = p.text;
        }
        DrawCentred(dl, ImVec2(x + w / 2, origin.y + h / 2), glyphs[i], U32(glyph));
        if (hovered && !io.MouseDown[0] && ImGui::GetCurrentContext()->HoveredIdTimer > 0.6f) ImGui::SetTooltip("%s", tips[i]);
        if (clicked) {
            if (i == 0) ShowWindow(hwnd_, SW_MINIMIZE);
            if (i == 1) ShowWindow(hwnd_, zoomed ? SW_RESTORE : SW_MAXIMIZE);
            if (i == 2) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        }
    }
    ImGui::PopFont();
    return 3 * w;
}

void App::drawCrumbs(ImVec2 at, float width, float h) {
    const Palette& p = Colours();
    float s = scale_;
    noDrag_.push_back({at, ImVec2(at.x + width, at.y + h)});
    float mid = at.y + h / 2;
    if (editingPath_) {
        ImGui::SetCursorScreenPos(at);
        ImGui::SetNextItemWidth(width);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12 * s, std::ceil((h - ImGui::GetFontSize()) / 2)));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6 * s);
        if (focusPath_) {
            ImGui::SetKeyboardFocusHere();
            focusPath_ = false;
        }
        bool enter = ImGui::InputText("##path", pathBuf_, sizeof pathBuf_,
                                      ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        bool deactivated = ImGui::IsItemDeactivated();
        ImGui::PopStyleVar(2);
        if (enter) {
            editingPath_ = false;
            goTo(pathBuf_);
        } else if (deactivated) {
            editingPath_ = false;
        }
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(at, ImVec2(at.x + width, at.y + h), U32(p.surface2), 6 * s);

    auto crumbs = Crumbs(cur_);
    const char* sep = "\xE2\x80\xBA";  // ›
    float sepW = 18 * s;
    float pad = 8 * s;
    std::vector<std::string> labels;
    std::vector<float> widths;
    for (size_t i = 0; i < crumbs.size(); i++) {
        std::string l = Narrow(crumbs[i].label);
        labels.push_back(l);
        bool last = i + 1 == crumbs.size();
        if (last) ImGui::PushFont(fonts_.semibold, 0);
        float w = ImGui::CalcTextSize(l.c_str()).x + pad * 2;
        if (last) ImGui::PopFont();
        if (i == 0 && IsLinux(cur_)) w += 22 * s;  // the penguin's stand-in
        widths.push_back(w);
    }
    // Fit from the end; the rest go behind "…".
    float room = width - 50 * s;
    size_t first = crumbs.size();
    float used = 0;
    while (first > 0 && used + widths[first - 1] + sepW <= room) used += widths[--first] + sepW;
    if (first == crumbs.size() && !crumbs.empty()) first = crumbs.size() - 1;

    float x = at.x + 4 * s;
    float ih = h - 6 * s, iy = at.y + 3 * s;
    auto crumb = [&](const char* id, float w) {
        ImGui::SetCursorScreenPos(ImVec2(x, iy));
        bool hit = ImGui::InvisibleButton(id, ImVec2(w, ih));
        if (ImGui::IsItemHovered()) dl->AddRectFilled(ImVec2(x, iy), ImVec2(x + w, iy + ih), U32(p.border), 4 * s);
        return hit;
    };
    if (first > 0) {
        float w = 28 * s;
        if (crumb("##more", w)) ImGui::OpenPopup("##hiddencrumbs");
        DrawCentred(dl, ImVec2(x + w / 2, mid), "\xE2\x80\xA6", U32(p.muted));
        x += w;
        if (ImGui::BeginPopup("##hiddencrumbs")) {
            for (size_t i = first; i-- > 0;)
                if (ImGui::Selectable(labels[i].c_str())) Navigate(crumbs[i].path);
            ImGui::EndPopup();
        }
    }
    for (size_t i = first; i < crumbs.size(); i++) {
        if (i > first || first > 0) {
            DrawCentred(dl, ImVec2(x + sepW / 2, mid), sep, U32(p.muted));
            x += sepW;
        }
        ImGui::PushID(int(i));
        bool last = i + 1 == crumbs.size();
        if (crumb("##c", widths[i])) Navigate(crumbs[i].path);
        float tx = x + pad;
        if (i == 0 && IsLinux(cur_)) {
            DrawCentred(dl, ImVec2(tx + 8 * s, mid), icon::Linux, U32(p.muted));
            tx += 22 * s;
        }
        if (last) ImGui::PushFont(fonts_.semibold, 0);
        dl->AddText(ImVec2(tx, TextTop(mid)), U32(last ? p.text : p.muted), labels[i].c_str());
        if (last) ImGui::PopFont();
        x += widths[i];
        ImGui::PopID();
    }
    // The empty rest of the bar edits the path as text.
    float restW = at.x + width - x;
    if (restW > 4) {
        ImGui::SetCursorScreenPos(ImVec2(x, at.y));
        if (ImGui::InvisibleButton("##editpath", ImVec2(restW, h))) {
            editingPath_ = focusPath_ = true;
            snprintf(pathBuf_, sizeof pathBuf_, "%s", Narrow(cur_).c_str());
        }
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
    }
}

void App::goTo(const std::string& typed) {
    std::wstring w = Widen(typed);
    while (!w.empty() && w.back() == L' ') w.pop_back();
    while (!w.empty() && w.front() == L' ') w.erase(w.begin());
    if (!w.empty() && w[0] == L'/') {
        // A Linux path: in the distro we're in, else the default one.
        std::wstring distro = distros_.empty() ? L"" : distros_[0].name;
        if (IsLinux(cur_)) {
            std::wstring root = Root(cur_);
            distro = root.substr(16, root.size() - 17);
        }
        w = FromLinux(w, distro);
    } else if (!w.empty() && w[0] == L'~') {
        w = HomeDir() + w.substr(1);
    } else {
        wchar_t buf[4096];
        if (ExpandEnvironmentStringsW(w.c_str(), buf, ARRAYSIZE(buf))) w = buf;
    }
    w = Normalise(w);
    if (IsDir(w)) Navigate(w);
    else if (!w.empty() && GetFileAttributesW(w.c_str()) != INVALID_FILE_ATTRIBUTES) go(Parent(w), true, BaseName(w), -1);
    else flash("Can't find " + typed, true);
}

bool App::sideItem(const char* icon, const std::string& label, const std::wstring& path, const char* sub, float fill,
                   bool warn) {
    const Palette& p = Colours();
    float s = scale_;
    float w = ImGui::GetContentRegionAvail().x;
    float lineH = ImGui::GetTextLineHeight();
    float h = sub ? lineH * 2 + 16 * s : lineH + 12 * s;
    ImVec2 a = ImGui::GetCursorScreenPos();
    bool hit = ImGui::InvisibleButton("##item", ImVec2(w, h));
    bool hovered = ImGui::IsItemHovered();
    bool here = !path.empty() && SamePath(path, cur_);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (here || hovered)
        dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), U32(here ? p.accentLo : p.surface2), 6 * s);
    if (here) dl->AddRectFilled(ImVec2(a.x, a.y + 6 * s), ImVec2(a.x + 3 * s, a.y + h - 6 * s), U32(p.accent), 2 * s);
    float x = a.x + 12 * s;
    float lineMid = sub ? a.y + 6 * s + lineH / 2 : a.y + h / 2;
    DrawCentred(dl, ImVec2(x + 8 * s, lineMid), icon, U32(here ? p.accent : p.muted));
    float tx = x + 26 * s;
    std::string shown = ellipsis(label, a.x + w - tx - 8 * s);
    dl->AddText(ImVec2(tx, TextTop(lineMid)), U32(p.text), shown.c_str());
    if (sub) {
        float by = a.y + 6 * s + lineH + 4 * s;
        if (fill >= 0) {
            float bw = a.x + w - tx - 12 * s;
            dl->AddRectFilled(ImVec2(tx, by), ImVec2(tx + bw, by + 4 * s), U32(p.border), 2 * s);
            dl->AddRectFilled(ImVec2(tx, by), ImVec2(tx + bw * std::clamp(fill, 0.0f, 1.0f), by + 4 * s),
                              U32(warn ? p.bad : p.accent), 2 * s);
            by += 7 * s;
        }
        ImGui::PushFont(nullptr, kSmall);
        dl->AddText(ImVec2(tx, by), U32(p.muted), sub);
        ImGui::PopFont();
    }
    return hit;
}

void App::drawSidebar() {
    const Palette& p = Colours();
    float s = scale_;
    ImGui::SetCursorPos(ImVec2(8 * s, 10 * s));
    ImGui::BeginGroup();
    ImGui::PushItemWidth(-1);
    float width = ImGui::GetWindowWidth() - 16 * s;
    auto heading = [&](const char* text) {
        ImGui::Dummy(ImVec2(0, 4 * s));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 12 * s);
        ImGui::PushFont(fonts_.semibold, kSmall);
        ImGui::PushStyleColor(ImGuiCol_Text, p.muted);
        ImGui::TextUnformatted(text);
        float mid = TextTop(0) * -1 + ImGui::GetItemRectMin().y;  // where its capitals centre
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 2 * s));
        return mid;
    };
    ImGui::BeginChild("sideinner", ImVec2(width, 0), 0);

    // Pinned, with + to pin where we are.
    float headMid = heading("PINNED");
    {
        // + sits level with the heading's letters.
        float bh = std::round(24 * s);
        ImVec2 back = ImGui::GetCursorPos();
        ImVec2 at(ImGui::GetWindowPos().x + width - bh - 6 * s, std::round(headMid - bh / 2));
        if (iconButton("##pin", icon::Add, "Pin this folder (Ctrl+D)", !cur_.empty() && !pinned(cur_), at, ImVec2(bh, bh))) pin(cur_);
        ImGui::SetCursorPos(back);
    }
    int moveFrom = -1, moveTo = -1;
    for (int i = 0; i < int(pins_.size()); i++) {
        ImGui::PushID(i);
        const char* ic = icon::Folder;
        for (auto& k : KnownPlaces())
            if (SamePath(k.path, pins_[i].path)) ic = k.icon;
        if (IsLinux(pins_[i].path)) ic = icon::Linux;
        if (sideItem(ic, pins_[i].label, pins_[i].path)) Navigate(pins_[i].path);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", Narrow(pins_[i].path).c_str());
        if (ImGui::IsItemClicked(1)) {
            pinMenuAt_ = i;
            openPinMenu_ = true;
        }
        // Drag to reorder.
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
            ImGui::SetDragDropPayload("pin", &i, sizeof i);
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (auto* pl = ImGui::AcceptDragDropPayload("pin")) {
                moveFrom = *static_cast<const int*>(pl->Data);
                moveTo = i;
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::PopID();
    }
    if (moveFrom >= 0 && moveFrom != moveTo) {
        Pin moved = pins_[moveFrom];
        pins_.erase(pins_.begin() + moveFrom);
        pins_.insert(pins_.begin() + moveTo, moved);
        Save();
    }
    if (pins_.empty()) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 12 * s);
        ImGui::TextDisabled("Pin folders with +");
    }

    heading("DRIVES");
    for (size_t i = 0; i < drives_.size(); i++) {
        const Drive& d = drives_[i];
        ImGui::PushID(int(100 + i));
        std::string sub;
        float fill = -1;
        bool warn = false;
        if (d.total) {
            sub = FormatSize(d.free) + " free of " + FormatSize(d.total);
            fill = 1.0f - float(double(d.free) / double(d.total));
            warn = d.free < d.total / 10;
        }
        if (sideItem(driveIcon(d.type), d.label, d.root, sub.empty() ? nullptr : sub.c_str(), fill, warn)) Navigate(d.root);
        if (ImGui::IsItemClicked(1)) {
            pinMenuAt_ = -1;
            ImGui::OpenPopup("##drive");
        }
        if (ImGui::BeginPopup("##drive")) {
            if (ImGui::MenuItem("Open")) Navigate(d.root);
            if (ImGui::MenuItem("Properties")) Properties(hwnd_, {d.root});
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (drives_.empty()) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 12 * s);
        ImGui::TextDisabled("Looking\xE2\x80\xA6");
    }

    if (!distros_.empty()) {
        heading("LINUX");
        for (size_t i = 0; i < distros_.size(); i++) {
            const Distro& d = distros_[i];
            ImGui::PushID(int(200 + i));
            std::wstring root = FromLinux(L"/", d.name);
            if (sideItem(icon::Linux, Narrow(d.name), root)) Navigate(root);
            std::wstring homeGuess = d.uid == 0 ? FromLinux(L"/root", d.name) : L"";
            std::string homeLabel = Narrow(d.name) + (d.uid == 0 ? "  /root" : "  home");
            ImGui::PushID("home");
            if (sideItem(icon::Home, homeLabel, homeGuess)) Navigate(DistroHome(d));
            ImGui::PopID();
            ImGui::PopID();
        }
    }
    ImGui::Dummy(ImVec2(0, 10 * s));
    ImGui::EndChild();
    ImGui::PopItemWidth();
    ImGui::EndGroup();

    if (openPinMenu_) {
        ImGui::OpenPopup("##pin");
        openPinMenu_ = false;
    }
    if (ImGui::BeginPopup("##pin")) {
        if (pinMenuAt_ >= 0 && pinMenuAt_ < int(pins_.size())) {
            if (ImGui::MenuItem("Open")) Navigate(pins_[pinMenuAt_].path);
            if (ImGui::MenuItem("Move up", nullptr, false, pinMenuAt_ > 0)) {
                std::swap(pins_[pinMenuAt_], pins_[pinMenuAt_ - 1]);
                Save();
            }
            if (ImGui::MenuItem("Move down", nullptr, false, pinMenuAt_ + 1 < int(pins_.size()))) {
                std::swap(pins_[pinMenuAt_], pins_[pinMenuAt_ + 1]);
                Save();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Unpin")) {
                pins_.erase(pins_.begin() + pinMenuAt_);
                Save();
            }
        }
        ImGui::EndPopup();
    }
}

void App::drawFolder(int vi, const Entry& e, ImVec2 p0, ImVec2 p1, int order, bool previews) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float s = scale_;
    bool none = false;
    std::vector<const Tex*> pics;
    std::vector<std::pair<std::wstring, Entry>> kids;
    if (previews) kids = thumbs_->WantFolder(pathOf(vi), e, order, none);
    for (auto& [path, kid] : kids) {
        bool failed = false;
        const Tex* t = thumbs_->Want(path, kid, order, failed);
        if (t && !t->icon) pics.push_back(t);
    }
    // A folder like Windows' own: the back with its tab, the pictures
    // inside fanned out, then the front pocket over their lower half.
    float bw = p1.x - p0.x;
    // Small (a details row), it fills the box, centred.
    float W = std::round(bw * (previews ? 0.86f : 0.95f)), H = std::round(W * (previews ? 0.76f : 0.8f));
    float cx = (p0.x + p1.x) / 2, x0 = std::round(cx - W / 2), x1 = x0 + W;
    float yb = previews ? p1.y : std::round((p0.y + p1.y + H) / 2), yt = yb - H;
    float r = std::max(2.0f, W * 0.045f);
    // Slate, in the app's colours, with an accent lip.
    const ImU32 back = IM_COL32(0x26, 0x26, 0x36, 0xFF), front = IM_COL32(0x3A, 0x3A, 0x52, 0xFF);
    const ImU32 frontLo = IM_COL32(0x2F, 0x2F, 0x44, 0xFF), lip = U32(Colours().accent, 0.75f);
    dl->AddRectFilled(ImVec2(x0, yt), ImVec2(x0 + W * 0.40f, yt + H * 0.2f), back, r);
    dl->AddRectFilled(ImVec2(x0, yt + H * 0.09f), ImVec2(x1, yb), back, r);

    float frontTop = std::round(yt + (pics.empty() ? H * 0.22f : H * 0.50f));
    if (!pics.empty()) {
        // Up to three sheets, the first on top.
        struct Place {
            float dx, deg;
        };
        static const Place one[] = {{0, 0}};
        static const Place two[] = {{-0.08f, -4}, {0.12f, 5}};
        static const Place three[] = {{0, 0}, {-0.17f, -7}, {0.17f, 7}};
        const Place* places = pics.size() == 1 ? one : pics.size() == 2 ? two : three;
        int n = int(pics.size());
        float boxW = W * (n == 1 ? 0.82f : 0.62f), boxH = H * 0.66f;
        float border = std::max(1.5f, 2.5f * s);
        for (int k = n - 1; k >= 0; k--) {
            const Tex* t = pics[k];
            float fit = std::min(boxW / t->width, boxH / t->height);
            float hw = t->width * fit / 2, hh = t->height * fit / 2;
            // Sit each sheet's bottom inside the pocket.
            ImVec2 c(cx + places[k].dx * W, frontTop + H * 0.16f - hh);
            float a = places[k].deg * 3.14159265f / 180, ca = std::cos(a), sa = std::sin(a);
            auto corner = [&](float x, float y) { return ImVec2(c.x + x * ca - y * sa, c.y + x * sa + y * ca); };
            float bx = hw + border, by = hh + border;
            dl->AddQuadFilled(corner(-bx + 1, -by + 2), corner(bx + 1, -by + 2), corner(bx + 1, by + 2), corner(-bx + 1, by + 2),
                              IM_COL32(0, 0, 0, 60));
            dl->AddQuadFilled(corner(-bx, -by), corner(bx, -by), corner(bx, by), corner(-bx, by), IM_COL32(0xF4, 0xF4, 0xF6, 0xFF));
            dl->AddImageQuad(t->ref(), corner(-hw, -hh), corner(hw, -hh), corner(hw, hh), corner(-hw, hh));
        }
        // The pocket's shadow on the sheets.
        dl->AddRectFilledMultiColor(ImVec2(x0, frontTop - 5 * s), ImVec2(x1, frontTop), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
                                    IM_COL32(0, 0, 0, 70), IM_COL32(0, 0, 0, 70));
    }
    dl->AddRectFilledMultiColor(ImVec2(x0, frontTop + r), ImVec2(x1, yb - r), front, front, frontLo, frontLo);
    dl->AddRectFilled(ImVec2(x0, frontTop), ImVec2(x1, frontTop + r * 2), front, r, ImDrawFlags_RoundCornersTop);
    dl->AddRectFilled(ImVec2(x0, yb - r * 2), ImVec2(x1, yb), frontLo, r, ImDrawFlags_RoundCornersBottom);
    dl->AddLine(ImVec2(x0 + r, frontTop + 0.5f), ImVec2(x1 - r, frontTop + 0.5f), lip, W > 24 * s ? std::max(1.0f, s) : 1.0f);
}

void App::drawIcon(int vi, const Entry& e, ImVec2 p0, ImVec2 p1, bool big, int order) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float bw = p1.x - p0.x, bh = p1.y - p0.y;
    if (e.dir) {
        drawFolder(vi, e, p0, p1, order, big && bw > 40 * scale_);
        return;
    }
    bool failed = true;
    const Tex* t = nullptr;
    if (!e.dir && (big || OwnIcon(e.ext))) t = thumbs_->Want(pathOf(vi), e, order, failed);
    if (t && !t->icon) {
        float k = std::min(bw / t->width, bh / t->height);
        float w = t->width * k, h = t->height * k;
        // Tiles sit pictures on the bottom edge, next to their names.
        ImVec2 a(std::floor(p0.x + (bw - w) / 2), std::floor(big ? p1.y - h : p0.y + (bh - h) / 2));
        ImVec2 b(a.x + w, a.y + h);
        dl->AddImage(t->ref(), a, b);
        if (big) dl->AddRect(a, b, U32(Colours().border, 0.8f));
        return;
    }
    const Tex* ic = t && t->icon ? t : icons_->For(e, big && bw > 40 * scale_);
    if (!ic) return;
    float cw = float(ic->contentW), ch = float(ic->contentH);
    float limit = big ? std::min(bw, bh) * 0.78f : std::min(bw, bh);
    // Small icons stay smallish rather than blowing up blurred.
    float k = std::min(limit / std::max(cw, ch), big ? 1.5f * scale_ : 4.0f);
    float w = cw * k, h = ch * k;
    ImVec2 a(std::floor(p0.x + (bw - w) / 2), std::floor(p0.y + (bh - h) / 2));
    dl->AddImage(ic->ref(), a, ImVec2(a.x + w, a.y + h), ic->uv0, ic->uv1);
}

void App::drawName(int vi, float width, bool centred) {
    const Entry& e = at(vi);
    if (!renaming_.empty() && Lower(renaming_) == lower_[view_[vi]]) {
        ImGui::SetNextItemWidth(width);
        if (renameStart_) ImGui::SetKeyboardFocusHere();
        auto cb = [](ImGuiInputTextCallbackData* d) -> int {
            App* a = static_cast<App*>(d->UserData);
            if (a->renameStart_) {
                d->SelectionStart = 0;
                d->SelectionEnd = d->CursorPos = std::min(a->renameStem_, d->BufTextLen);
                a->renameStart_ = false;
            }
            return 0;
        };
        bool enter = ImGui::InputText("##rename", renameBuf_, sizeof renameBuf_,
                                      ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackAlways, cb, this);
        if (enter) finishRename(true);
        else if (ImGui::IsItemDeactivated() && !renameStart_) finishRename(!ImGui::IsKeyPressed(ImGuiKey_Escape, false));
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 at = ImGui::GetCursorScreenPos();
    ImU32 col = U32(Colours().text, hidden(e) || (e.attrs & FILE_ATTRIBUTE_HIDDEN) ? 0.55f : 1.0f);
    if (!centred) {
        std::string s = ellipsis(e.utf8, width);
        dl->AddText(at, col, s.c_str());
        ImGui::Dummy(ImVec2(width, ImGui::GetTextLineHeight()));
        return;
    }
    auto [l1, l2] = splitTwo(e.utf8, width);
    float lh = ImGui::GetTextLineHeight();
    float w1 = ImGui::CalcTextSize(l1.c_str()).x;
    dl->AddText(ImVec2(std::floor(at.x + (width - w1) / 2), at.y), col, l1.c_str());
    if (!l2.empty()) {
        float w2 = ImGui::CalcTextSize(l2.c_str()).x;
        dl->AddText(ImVec2(std::floor(at.x + (width - w2) / 2), at.y + lh), col, l2.c_str());
    }
    ImGui::Dummy(ImVec2(width, lh * 2));
}

void App::itemPressed(int vi) {
    ImGuiIO& io = ImGui::GetIO();
    if (!renaming_.empty() && Lower(renaming_) != lower_[view_[vi]]) finishRename(true);
    if (io.KeyShift) {
        selectRange(anchor_ < 0 ? vi : anchor_, vi, io.KeyCtrl);
        if (anchor_ < 0) anchor_ = vi;
        focus_ = vi;
    } else if (io.KeyCtrl) {
        toggle(vi);
        anchor_ = focus_ = vi;
    } else if (isSelected(vi)) {
        pressed_ = vi;  // keep the rest selected in case this is a drag
        anchor_ = focus_ = vi;
    } else {
        selectOnly(vi);
    }
    dragArmed_ = isSelected(vi);
}

void App::itemReleased(int vi) {
    if (pressed_ == vi && !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift) selectOnly(vi);
    pressed_ = -1;
}

void App::drawGrid() {
    const Palette& p = Colours();
    ImGuiIO& io = ImGui::GetIO();
    float s = scale_;
    bool ctrlWheel = io.KeyCtrl && io.MouseWheel != 0;
    ImGui::BeginChild("grid", ImVec2(0, 0), 0, ctrlWheel ? ImGuiWindowFlags_NoScrollWithMouse : 0);
    if (ctrlWheel && ImGui::IsWindowHovered()) tile_ = std::clamp(tile_ + io.MouseWheel * 16, 72.0f, 320.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    thumbs_->MipSampler(dl);

    float pad = 12 * s;
    float tile = tile_ * s;
    float lineH = ImGui::GetTextLineHeight();
    float avail = ImGui::GetContentRegionAvail().x;
    int cols = std::max(1, int((avail - pad) / (tile + 14 * s)));
    float cellW = (avail - pad) / cols;
    float cellH = tile + lineH * 2 + 22 * s;
    int n = int(view_.size());
    int rows = (n + cols - 1) / cols;
    float winH = ImGui::GetWindowHeight();
    cols_ = cols;
    page_ = std::max(1, int(winH / cellH)) * cols;

    if (arriveScroll_ >= 0) {
        ImGui::SetScrollY(arriveScroll_);
        arriveScroll_ = -1;
    }
    float scrollY = ImGui::GetScrollY();
    if (scrollToFocus_ && focus_ >= 0 && focus_ < n) {
        float y = pad + (focus_ / cols) * cellH;
        if (y < scrollY) ImGui::SetScrollY(y - pad);
        else if (y + cellH > scrollY + winH) ImGui::SetScrollY(y + cellH - winH + pad);
        scrollToFocus_ = false;
    }
    ImVec2 origin = ImGui::GetCursorScreenPos();  // where row 0 would be, scrolled
    int firstRow = std::max(0, int((scrollY - pad) / cellH));
    int lastRow = std::min(rows - 1, int((scrollY + winH) / cellH) + 1);

    int order = 0;
    for (int r = firstRow; r <= lastRow; r++) {
        for (int c = 0; c < cols; c++) {
            int i = r * cols + c;
            if (i >= n) break;
            const Entry& e = at(i);
            ImVec2 a(origin.x + pad / 2 + c * cellW + 3 * s, origin.y + pad + r * cellH);
            ImVec2 size(cellW - 6 * s, cellH - 6 * s);
            ImGui::PushID(i);
            ImGui::SetCursorScreenPos(a);
            ImGui::SetNextItemAllowOverlap();
            ImGui::InvisibleButton("##t", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
            bool hovered = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked(0)) itemPressed(i);
            if (hovered && ImGui::IsMouseReleased(0)) itemReleased(i);
            if (hovered && ImGui::IsMouseDoubleClicked(0)) open(i);
            if (ImGui::IsItemClicked(1)) {
                if (!isSelected(i)) selectOnly(i);
                openItemMenu_ = true;
            }
            bool sel = isSelected(i);
            ImVec2 b(a.x + size.x, a.y + size.y);
            if (sel) {
                dl->AddRectFilled(a, b, U32(p.accentLo), 6 * s);
                dl->AddRect(a, b, U32(p.accent, 0.55f), 6 * s);
            } else if (hovered) {
                dl->AddRectFilled(a, b, U32(p.surface2), 6 * s);
            }
            ImVec2 box0(a.x + (size.x - tile) / 2, a.y + 6 * s);
            drawIcon(i, e, box0, ImVec2(box0.x + tile, box0.y + tile), true, order++);
            ImGui::SetCursorScreenPos(ImVec2(a.x + 6 * s, box0.y + tile + 6 * s));
            drawName(i, size.x - 12 * s, true);
            ImGui::PopID();
        }
    }
    // Make the next screenful too, behind what's showing.
    int ahead = (lastRow - firstRow + 1) * cols;
    for (int i = (lastRow + 1) * cols; i < std::min(n, (lastRow + 1) * cols + ahead); i++) {
        bool failed;
        if (!at(i).dir) thumbs_->Want(pathOf(i), at(i), 100000 + i, failed);
    }

    // Rubber-band selection from empty space.
    bool overItem = ImGui::IsAnyItemHovered();
    if (ImGui::IsWindowHovered() && !overItem) {
        if (ImGui::IsMouseClicked(0)) {
            if (!renaming_.empty()) finishRename(true);
            banding_ = true;
            bandFrom_ = ImVec2(io.MousePos.x - origin.x, io.MousePos.y - origin.y);
            if (io.KeyCtrl) bandBase_ = selected_;
            else {
                bandBase_.clear();
                selected_.clear();
            }
        }
        if (ImGui::IsMouseClicked(1)) {
            selected_.clear();
            openFolderMenu_ = true;
        }
    }
    if (banding_) {
        if (!ImGui::IsMouseDown(0)) {
            banding_ = false;
        } else {
            ImVec2 to(io.MousePos.x - origin.x, io.MousePos.y - origin.y);
            ImVec2 lo(std::min(bandFrom_.x, to.x), std::min(bandFrom_.y, to.y));
            ImVec2 hi(std::max(bandFrom_.x, to.x), std::max(bandFrom_.y, to.y));
            selected_ = bandBase_;
            int r0 = std::max(0, int((lo.y - pad) / cellH)), r1 = std::min(rows - 1, int((hi.y - pad) / cellH));
            int c0 = std::max(0, int((lo.x - pad / 2) / cellW)), c1 = std::min(cols - 1, int((hi.x - pad / 2) / cellW));
            for (int r = r0; r <= r1; r++)
                for (int c = c0; c <= c1; c++) {
                    int i = r * cols + c;
                    if (i >= n) continue;
                    // Only if the band reaches the tile itself, not the gap.
                    float tx0 = pad / 2 + c * cellW + 3 * s, ty0 = pad + r * cellH;
                    if (hi.x >= tx0 && lo.x <= tx0 + cellW - 6 * s && hi.y >= ty0 && lo.y <= ty0 + cellH - 6 * s) {
                        selected_.insert(lower_[view_[i]]);
                        focus_ = i;
                    }
                }
            if (lo.x != hi.x || lo.y != hi.y) {
                ImVec2 a(origin.x + lo.x, origin.y + lo.y), b(origin.x + hi.x, origin.y + hi.y);
                dl->AddRectFilled(a, b, U32(p.accent, 0.15f));
                dl->AddRect(a, b, U32(p.accent, 0.8f));
            }
            // Scroll when the band reaches the edges.
            float top = ImGui::GetWindowPos().y;
            if (io.MousePos.y > top + winH - 20 * s) ImGui::SetScrollY(scrollY + 18 * s);
            else if (io.MousePos.y < top + 20 * s) ImGui::SetScrollY(std::max(0.0f, scrollY - 18 * s));
        }
    }

    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + pad * 2 + rows * cellH));
    ImGui::Dummy(ImVec2(1, 1));
    scrollY_ = ImGui::GetScrollY();
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    ImGui::EndChild();
}

void App::drawDetails() {
    const Palette& p = Colours();
    float s = scale_;
    ImGuiTableFlags flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_Resizable | ImGuiTableFlags_Hideable |
                            ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX |
                            ImGuiTableFlags_NoBordersInBody;
    ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, p.bg);
    ImGui::PushStyleColor(ImGuiCol_TableBorderLight, p.border);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8 * s, 0));
    if (!ImGui::BeginTable("details", 5, flags, ImVec2(0, 0))) {
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoHide, 0, 0);
    ImGui::TableSetupColumn("Date modified", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, 140 * s, 1);
    ImGui::TableSetupColumn("Date created", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, 140 * s, 2);
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 150 * s, 3);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, 90 * s, 4);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8 * s, 6 * s));
    ImGui::TableHeadersRow();
    ImGui::PopStyleVar();
    // Tell the table our sort (once its layout is done, on its first frame
    // or after the sort changed elsewhere).
    if (!sortApplied_) {
        ImGui::TableSetColumnSortDirection(int(sort_.key), sort_.descending ? ImGuiSortDirection_Descending : ImGuiSortDirection_Ascending, false);
        sortApplied_ = true;
    }
    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs && specs->SpecsDirty) {
        if (specs->SpecsCount > 0) {
            sort_.key = SortKey(specs->Specs[0].ColumnUserID);
            sort_.descending = specs->Specs[0].SortDirection == ImGuiSortDirection_Descending;
            rebuild();
        }
        specs->SpecsDirty = false;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    thumbs_->MipSampler(dl);

    float lineH = ImGui::GetTextLineHeight();
    float iconS = 20 * s;
    float rowH = std::max(lineH, iconS) + 8 * s;
    int n = int(view_.size());
    float headerH = lineH + 12 * s;
    float viewH = ImGui::GetWindowHeight() - headerH;
    page_ = std::max(1, int(viewH / rowH));
    cols_ = 1;
    if (arriveScroll_ >= 0) {
        ImGui::SetScrollY(arriveScroll_);
        arriveScroll_ = -1;
    }
    if (scrollToFocus_ && focus_ >= 0 && focus_ < n) {
        float y = focus_ * rowH, sy = ImGui::GetScrollY();
        if (y < sy) ImGui::SetScrollY(y);
        else if (y + rowH > sy + viewH) ImGui::SetScrollY(y + rowH - viewH);
        scrollToFocus_ = false;
    }

    ImGuiListClipper clip;
    clip.Begin(n, rowH);
    int order = 0;
    while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            const Entry& e = at(i);
            ImGui::TableNextRow(0, rowH);
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(i);
            bool sel = isSelected(i);
            ImVec2 rowTop = ImGui::GetCursorScreenPos();
            ImGui::PushStyleColor(ImGuiCol_Header, p.accentLo);
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, sel ? p.accentLo : p.surface2);
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, p.accentLo);
            ImGui::Selectable("##row", sel, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0, rowH));
            ImGui::PopStyleColor(3);
            bool hovered = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked(0)) itemPressed(i);
            if (hovered && ImGui::IsMouseReleased(0)) itemReleased(i);
            if (hovered && ImGui::IsMouseDoubleClicked(0)) open(i);
            if (ImGui::IsItemClicked(1)) {
                if (!isSelected(i)) selectOnly(i);
                openItemMenu_ = true;
            }
            float cy = rowTop.y + (rowH - lineH) / 2;
            ImVec2 ic(rowTop.x, rowTop.y + (rowH - iconS) / 2);
            drawIcon(i, e, ic, ImVec2(ic.x + iconS, ic.y + iconS), false, order++);
            float nameX = rowTop.x + iconS + 8 * s;
            float colW = ImGui::GetContentRegionAvail().x - iconS - 8 * s;
            bool editing = !renaming_.empty() && Lower(renaming_) == lower_[view_[i]];
            ImGui::SetCursorScreenPos(ImVec2(nameX, editing ? rowTop.y + (rowH - ImGui::GetFrameHeight()) / 2 : cy));
            drawName(i, colW, false);
            auto cell = [&](int col, const std::string& text, bool rightAlign) {
                if (!ImGui::TableSetColumnIndex(col)) return;
                float x = ImGui::GetCursorScreenPos().x;
                float w = ImGui::GetContentRegionAvail().x;
                std::string t = ellipsis(text, w);
                if (rightAlign) x += w - ImGui::CalcTextSize(t.c_str()).x;
                dl->AddText(ImVec2(x, cy), U32(p.muted), t.c_str());
            };
            cell(1, formatTime(e.modified), false);
            cell(2, formatTime(e.created), false);
            cell(3, Narrow(types_[view_[i]]), false);
            cell(4, e.dir ? "" : FormatSize(e.size), true);
            ImGui::PopID();
        }
    }
    // Empty space below the rows: clear, or the folder's menu.
    if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered()) {
        if (ImGui::IsMouseClicked(0)) {
            if (!renaming_.empty()) finishRename(true);
            selected_.clear();
        }
        if (ImGui::IsMouseClicked(1)) {
            selected_.clear();
            openFolderMenu_ = true;
        }
    }
    scrollY_ = ImGui::GetScrollY();
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    ImGui::EndTable();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

void App::drawEmpty() {
    const char* text = nullptr;
    std::string err;
    if (loading_) {
        if (now() - loadStart_ > 150) text = "Loading\xE2\x80\xA6";
    } else if (!listError_.empty()) {
        err = "Can't open this folder: " + listError_;
        text = err.c_str();
    } else if (view_.empty()) {
        if (filter_[0]) text = "Nothing here matches the filter";
        else if (!entries_.empty()) text = "Only hidden files here (Ctrl+H shows them)";
        else text = "This folder is empty";
    }
    if (!text) return;
    ImVec2 pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
    ImVec2 ts = ImGui::CalcTextSize(text);
    ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x + (size.x - ts.x) / 2, pos.y + size.y * 0.35f),
                                        U32(listError_.empty() ? Colours().muted : Colours().bad), text);
}

void App::drawStatus() {
    const Palette& p = Colours();
    float s = scale_;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float w = ImGui::GetWindowWidth();
    float h = std::round(ImGui::GetTextLineHeight() + 12 * s);
    // On the window's bottom edge, whatever spacing came before.
    ImVec2 a(ImGui::GetWindowPos().x, ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - h);
    ImGui::SetCursorScreenPos(a);
    dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), U32(p.surface));
    dl->AddLine(a, ImVec2(a.x + w, a.y), U32(p.border));
    ImGui::PushFont(nullptr, kSmall);
    float y = TextTop(a.y + h / 2);

    char buf[256];
    size_t total = 0;
    for (auto& e : entries_)
        if (!hidden(e)) total++;
    if (filter_[0]) snprintf(buf, sizeof buf, "%zu of %zu items", view_.size(), total);
    else snprintf(buf, sizeof buf, "%zu item%s", view_.size(), view_.size() == 1 ? "" : "s");
    std::string left = buf;
    if (!selected_.empty()) {
        uint64_t bytes = 0;
        int files = 0;
        auto sel = selectedIndices();
        for (int vi : sel)
            if (!at(vi).dir) {
                bytes += at(vi).size;
                files++;
            }
        snprintf(buf, sizeof buf, "   \xC2\xB7   %zu selected", sel.size());
        left += buf;
        if (files) left += " (" + FormatSize(bytes) + ")";
    }
    dl->AddText(ImVec2(a.x + 12 * s, y), U32(p.muted), left.c_str());

    // Right: messages, thumbnail progress, the Linux path.
    std::string right;
    ImU32 rightCol = U32(p.muted);
    if (!message_.empty() && now() - messageAt_ < 4000) {
        right = message_;
        if (messageBad_) rightCol = U32(p.bad);
    } else {
        message_.clear();
        ThumbStats st = thumbs_->Stats();
        if (st.queued + st.working > 0) {
            snprintf(buf, sizeof buf, "Making thumbnails\xE2\x80\xA6 %d", st.queued + st.working);
            right = buf;
        } else if (IsLinux(cur_)) {
            right = std::string(icon::Linux) + "  " + Narrow(ToLinux(cur_));
        }
    }
    if (!right.empty()) {
        float rw = ImGui::CalcTextSize(right.c_str()).x;
        dl->AddText(ImVec2(a.x + w - rw - 12 * s, y), rightCol, right.c_str());
    }
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(w, h));
}

void App::drawMenus() {
    float s = scale_;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6 * s, 6 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * s, 6 * s));
    if (openItemMenu_) {
        ImGui::OpenPopup("##item");
        openItemMenu_ = false;
    }
    if (openFolderMenu_) {
        ImGui::OpenPopup("##folder");
        openFolderMenu_ = false;
    }
    if (ImGui::BeginPopup("##item")) {
        auto sel = selectedIndices();
        bool single = sel.size() == 1;
        const Entry* e = single ? &at(sel[0]) : nullptr;
        ImGui::PushFont(fonts_.semibold, 0);
        if (ImGui::MenuItem("Open", "Enter")) openSelection();
        ImGui::PopFont();
        if (e && !e->dir && ImGui::MenuItem("Open with\xE2\x80\xA6")) OpenWith(hwnd_, pathOf(sel[0]));
        if (e && e->dir) {
            std::wstring path = pathOf(sel[0]);
            if (pinned(path) ? ImGui::MenuItem("Unpin from sidebar") : ImGui::MenuItem("Pin to sidebar"))
                pinned(path) ? unpin(path) : pin(path);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Cut", "Ctrl+X")) copySelection(true);
        if (ImGui::MenuItem("Copy", "Ctrl+C")) copySelection(false);
        if (e && e->dir && ImGui::MenuItem("Paste into folder", nullptr, false, ClipboardHasFiles())) paste(pathOf(sel[0]));
        if (ImGui::MenuItem("Copy path", "Ctrl+Shift+C")) copyPaths(false);
        if (IsLinux(cur_) && ImGui::MenuItem("Copy Linux path")) copyPaths(true);
        ImGui::Separator();
        if (single && ImGui::MenuItem("Rename", "F2")) startRename(sel[0]);
        if (ImGui::MenuItem("Delete", "Del")) recycle(false);
        ImGui::Separator();
        if (ImGui::MenuItem("Properties", "Alt+Enter")) Properties(hwnd_, selectedPaths());
        if (ImGui::MenuItem("More options\xE2\x80\xA6")) later_ = Later::ItemMenu;
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##folder")) {
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Thumbnails", "Ctrl+1", thumbView_)) thumbView_ = true;
            if (ImGui::MenuItem("Details", "Ctrl+2", !thumbView_)) {
                thumbView_ = false;
                sortApplied_ = false;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Sort by")) {
            for (int k = 0; k < 5; k++)
                if (ImGui::MenuItem(sortName(SortKey(k)), nullptr, int(sort_.key) == k)) SetSort(SortKey(k), sort_.descending);
            ImGui::Separator();
            if (ImGui::MenuItem("Ascending", nullptr, !sort_.descending)) SetSort(sort_.key, false);
            if (ImGui::MenuItem("Descending", nullptr, sort_.descending)) SetSort(sort_.key, true);
            ImGui::Separator();
            if (ImGui::MenuItem("Folders first", nullptr, sort_.foldersFirst)) {
                sort_.foldersFirst = !sort_.foldersFirst;
                rebuild();
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Show hidden files", "Ctrl+H", showHidden_)) {
            showHidden_ = !showHidden_;
            rebuild();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("New folder", "Ctrl+Shift+N")) newFolder();
        if (ImGui::MenuItem("Paste", "Ctrl+V", false, ClipboardHasFiles())) paste(cur_);
        ImGui::Separator();
        if (pinned(cur_) ? ImGui::MenuItem("Unpin this folder") : ImGui::MenuItem("Pin this folder", "Ctrl+D"))
            pinned(cur_) ? unpin(cur_) : pin(cur_);
        if (ImGui::MenuItem("Copy path")) copyPaths(false);
        if (IsLinux(cur_) && ImGui::MenuItem("Copy Linux path")) CopyText(hwnd_, ToLinux(cur_));
        if (ImGui::MenuItem("Open in Terminal")) openTerminal();
        if (ImGui::MenuItem("Open in Explorer"))
            ShellExecuteW(hwnd_, nullptr, L"explorer.exe", (L"\"" + cur_ + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        ImGui::Separator();
        if (ImGui::MenuItem("Refresh", "F5")) refresh();
        if (ImGui::MenuItem("Properties")) Properties(hwnd_, {cur_});
        if (ImGui::MenuItem("More options\xE2\x80\xA6")) later_ = Later::FolderMenu;
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}

void App::keys() {
    ImGuiIO& io = ImGui::GetIO();
    auto pressed = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };
    if (ImGui::IsMouseClicked(3)) Back();
    if (ImGui::IsMouseClicked(4)) Forward();
    if (io.KeyAlt && pressed(ImGuiKey_LeftArrow)) Back();
    if (io.KeyAlt && pressed(ImGuiKey_RightArrow)) Forward();
    if (io.KeyAlt && pressed(ImGuiKey_UpArrow)) up();
    if ((io.KeyCtrl && pressed(ImGuiKey_L)) || (io.KeyAlt && pressed(ImGuiKey_D))) {
        editingPath_ = focusPath_ = true;
        snprintf(pathBuf_, sizeof pathBuf_, "%s", Narrow(cur_).c_str());
    }
    if (io.KeyCtrl && pressed(ImGuiKey_F)) focusFilter_ = true;
    if (pressed(ImGuiKey_F5)) refresh();
    if (io.WantTextInput || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) return;

    int n = int(view_.size());
    bool ctrl = io.KeyCtrl, shift = io.KeyShift, alt = io.KeyAlt;
    if (pressed(ImGuiKey_Backspace)) up();
    if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter)) {
        if (alt) Properties(hwnd_, selectedPaths().empty() ? std::vector<std::wstring>{cur_} : selectedPaths());
        else openSelection();
    }
    if (pressed(ImGuiKey_F2) && focus_ >= 0) startRename(focus_);
    if (pressed(ImGuiKey_Delete)) recycle(shift);
    if (ctrl && !shift && pressed(ImGuiKey_A)) selectRange(0, n - 1, false);
    if (ctrl && !shift && pressed(ImGuiKey_C)) copySelection(false);
    if (ctrl && shift && pressed(ImGuiKey_C)) copyPaths(false);
    if (ctrl && pressed(ImGuiKey_X)) copySelection(true);
    if (ctrl && pressed(ImGuiKey_V)) paste(cur_);
    if (ctrl && shift && pressed(ImGuiKey_N)) newFolder();
    if (ctrl && pressed(ImGuiKey_D)) pin(cur_);
    if (ctrl && pressed(ImGuiKey_1)) thumbView_ = true;
    if (ctrl && pressed(ImGuiKey_2)) {
        thumbView_ = false;
        sortApplied_ = false;
    }
    if (ctrl && pressed(ImGuiKey_H)) {
        showHidden_ = !showHidden_;
        rebuild();
    }
    if (pressed(ImGuiKey_Escape)) {
        if (filter_[0]) {
            filter_[0] = 0;
            rebuild();
        } else {
            selected_.clear();
        }
    }
    if (n > 0 && !alt) {
        int f = focus_ < 0 ? -1 : focus_;
        int step = thumbView_ ? cols_ : 1;
        int to = -2;
        if (pressed(ImGuiKey_DownArrow)) to = f < 0 ? 0 : std::min(n - 1, f + step);
        if (pressed(ImGuiKey_UpArrow)) to = f < 0 ? 0 : std::max(0, f - step);
        if (thumbView_ && pressed(ImGuiKey_RightArrow)) to = f < 0 ? 0 : std::min(n - 1, f + 1);
        if (thumbView_ && pressed(ImGuiKey_LeftArrow)) to = f < 0 ? 0 : std::max(0, f - 1);
        if (pressed(ImGuiKey_PageDown)) to = std::min(n - 1, std::max(f, 0) + page_);
        if (pressed(ImGuiKey_PageUp)) to = std::max(0, f - page_);
        if (pressed(ImGuiKey_Home)) to = 0;
        if (pressed(ImGuiKey_End)) to = n - 1;
        if (to >= 0) focusOn(to, shift);
    }
    // Type the start of a name to jump to it.
    if (!ctrl && !alt && n > 0) {
        bool typedAny = false;
        for (ImWchar c : io.InputQueueCharacters) {
            if (c < 32 || c == 127) continue;
            if (now() - typedAt_ > 1000) typed_.clear();
            typed_ += wchar_t(c);
            typedAt_ = now();
            typedAny = true;
        }
        if (typedAny) {
            std::wstring want = Lower(typed_);
            // The same letter again moves on to the next match.
            bool repeat = want.size() > 1 && want.find_first_not_of(want[0]) == std::wstring::npos;
            if (repeat) want = want.substr(0, 1);
            int start = repeat && focus_ >= 0 ? focus_ + 1 : std::max(focus_, 0);
            for (int k = 0; k < n; k++) {
                int i = (start + k) % n;
                if (lower_[view_[i]].compare(0, want.size(), want) == 0) {
                    focusOn(i, false);
                    break;
                }
            }
        }
    }
}

}  // namespace shelf
