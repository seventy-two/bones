// The sizes view: what's taking the space under a folder, TreeSize-style.
// A background scan (sizes.cpp) fills it in while it's drawn; folders are
// biggest first, each with a bar for its share of the folder above.
#include "app.h"

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace bones {

namespace {

// grouped gives a count with thousands separators: "412,301".
std::string grouped(uint64_t n) {
    std::string d = std::to_string(n), out;
    for (size_t i = 0; i < d.size(); i++) {
        if (i && (d.size() - i) % 3 == 0) out += ',';
        out += d[i];
    }
    return out;
}

std::string plural(uint64_t n, const char* one, const char* many) { return grouped(n) + " " + (n == 1 ? one : many); }

std::string duration(double secs) {
    char buf[32];
    if (secs < 60) snprintf(buf, sizeof buf, "%.0f s", std::max(secs, 1.0));
    else snprintf(buf, sizeof buf, "%dm %02ds", int(secs) / 60, int(secs) % 60);
    return buf;
}

// The kinds of row: a folder, a folder's own files taken together, and
// the rest of a long folder.
enum class RowKind { Folder, Files, More };
struct Row {
    RowKind kind;
    SizeNode* node;  // the folder (for Files and More, the one they're in)
    int depth;
    uint64_t bytes = 0, files = 0, count = 0;  // for Files and More
};

constexpr size_t kMaxKids = 200;  // past this, one "N more folders" row

}  // namespace

void App::startScan() {
    scan_.reset();  // stops the old one first
    sizesOpen_.clear();
    sizesSel_ = sizesMenuNode_ = nullptr;
    removals_.clear();
    scan_ = std::make_unique<SizeScan>(cur_, [wake = wake_] { wake(); });
}

void App::recycleNode(SizeNode* n, bool permanently) {
    if (!scan_ || !n || !n->parent || !n->Done()) return;
    std::wstring path = scan_->PathOf(n);
    removals_.push_back({n, path});
    Recycle(hwnd_, {path}, permanently, [shared = shared_, wake = wake_] {
        shared->dirty = true;
        wake();
    });
}

// recheckRemovals takes recycled folders out of the totals once they've
// actually gone (the shell's dialog may have been cancelled).
void App::recheckRemovals() {
    if (!scan_) return;
    for (auto it = removals_.begin(); it != removals_.end();) {
        if (GetFileAttributesW(it->path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            if (sizesSel_ == it->node) sizesSel_ = nullptr;
            scan_->Removed(it->node);
            it = removals_.erase(it);
        } else {
            ++it;
        }
    }
}

void App::sizesMenu(SizeNode* n) {
    if (!scan_ || !n) return;
    std::wstring path = scan_->PathOf(n);
    if (ImGui::MenuItem("Open")) Navigate(path);
    if (ImGui::MenuItem("Show in folder view")) {
        sizesView_ = false;
        Navigate(path);
    }
    if (ImGui::MenuItem("Open in Explorer"))
        ShellExecuteW(hwnd_, L"explore", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    ImGui::Separator();
    if (ImGui::MenuItem("Copy path")) {
        CopyText(hwnd_, path);
        flash("Copied the path");
    }
    if (ImGui::MenuItem("Pin to sidebar", nullptr, false, !pinned(path))) pin(path);
    ImGui::Separator();
    bool can = n->parent && n->Done();
    if (ImGui::MenuItem("Recycle", "Del", false, can)) recycleNode(n, false);
    if (!can && n->parent && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Once it's been scanned");
    if (ImGui::MenuItem("Properties")) Properties(hwnd_, {path});
}

void App::drawSizes() {
    const Palette& p = Colours();
    float s = scale_;
    if (!scan_ || !scan_->Covers(cur_)) startScan();
    SizeNode* here = scan_->Find(cur_);
    // Done, but this folder isn't in it (made since?): scan it afresh.
    if (!here && scan_->Done()) {
        startScan();
        here = scan_->Find(cur_);
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float W = ImGui::GetContentRegionAvail().x;

    // The summary: the total, what it's made of, and the scan.
    float headH = std::round(64 * s), pad = 20 * s;
    {
        uint64_t bytes = here ? here->bytes.load() : 0, files = here ? here->files.load() : 0,
                 folders = here ? here->folders.load() : 0;
        bool done = here && here->Done();
        ImGui::PushFont(fonts_.semibold, 26);
        std::string total = FormatSize(bytes);
        dl->AddText(ImVec2(origin.x + pad, origin.y + 12 * s), U32(done ? p.text : p.muted), total.c_str());
        float tw = ImGui::CalcTextSize(total.c_str()).x;
        ImGui::PopFont();
        ImGui::PushFont(nullptr, kSmall);
        std::string line = "in " + plural(files, "file", "files") + " and " + plural(folders, "folder", "folders");
        float lx = origin.x + pad + tw + 14 * s;
        dl->AddText(ImVec2(lx, origin.y + 14 * s), U32(p.muted), line.c_str());
        // On a drive's root, how that compares with what the drive says.
        std::string drive;
        for (auto& d : drives_)
            if (SamePath(d.root, cur_) && d.total) {
                uint64_t used = d.total - d.free;
                drive = FormatSize(used) + " used of " + FormatSize(d.total);
                if (done && used > bytes) drive += "  \xC2\xB7  " + FormatSize(used - bytes) + " not in any folder bones can read";
            }
        if (!drive.empty()) dl->AddText(ImVec2(lx, origin.y + 32 * s), U32(p.muted), drive.c_str());
        ImGui::PopFont();

        // Rescan, on the right.
        float bw = std::round(96 * s), bh = std::round(28 * s);
        ImVec2 b0(origin.x + W - pad - bw, origin.y + (headH - bh) / 2);
        ImGui::SetCursorScreenPos(b0);
        if (ImGui::InvisibleButton("##rescan", ImVec2(bw, bh))) startScan();
        bool hov = ImGui::IsItemHovered();
        dl->AddRectFilled(b0, ImVec2(b0.x + bw, b0.y + bh), U32(hov ? p.border : p.surface2), 6 * s);
        DrawCentred(dl, ImVec2(b0.x + 18 * s, b0.y + bh / 2), icon::Refresh, U32(p.muted));
        dl->AddText(ImVec2(b0.x + 34 * s, TextTop(b0.y + bh / 2)), U32(p.text), "Rescan");
        if (hov) ImGui::SetTooltip("Scan this folder again (F5)");
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + headH));
    here = scan_->Find(cur_);  // a rescan may have started
    if (!here) {
        ImGui::SetCursorScreenPos(ImVec2(origin.x + pad, origin.y + headH + 10 * s));
        ImGui::TextDisabled("Scanning\xE2\x80\xA6");
        return;
    }

    // The rows: this folder's folders, and theirs where opened.
    std::vector<Row> rows;
    auto addKids = [&](auto& self, SizeNode* n, int depth) -> void {
        auto kids = SortedKids(n);
        size_t shown = std::min(kids.size(), kMaxKids);
        // A folder's own files sit among its folders, by size.
        bool filesRow = n->listed && n->ownFiles > 0;
        bool filesDone = !filesRow;
        for (size_t i = 0; i < shown; i++) {
            if (!filesDone && n->ownBytes >= kids[i]->bytes.load()) {
                rows.push_back({RowKind::Files, n, depth, n->ownBytes, n->ownFiles});
                filesDone = true;
            }
            rows.push_back({RowKind::Folder, kids[i], depth});
            if (sizesOpen_.count(kids[i])) self(self, kids[i], depth + 1);
        }
        if (!filesDone) rows.push_back({RowKind::Files, n, depth, n->ownBytes, n->ownFiles});
        if (kids.size() > shown) {
            Row more{RowKind::More, n, depth};
            for (size_t i = shown; i < kids.size(); i++) {
                more.bytes += kids[i]->bytes.load();
                more.files += kids[i]->files.load();
            }
            more.count = kids.size() - shown;
            rows.push_back(more);
        }
    };
    addKids(addKids, here, 0);
    sizesRows_.clear();
    for (auto& r : rows)
        if (r.kind == RowKind::Folder) sizesRows_.push_back(r.node);

    ImGuiTableFlags flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit |
                            ImGuiTableFlags_PadOuterX | ImGuiTableFlags_NoBordersInBody;
    ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, p.bg);
    ImGui::PushStyleColor(ImGuiCol_TableBorderLight, p.border);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8 * s, 0));
    if (!ImGui::BeginTable("sizes", 5, flags, ImVec2(0, 0))) {
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoHide);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 90 * s);
    ImGui::TableSetupColumn("Share", ImGuiTableColumnFlags_WidthFixed, 180 * s);
    ImGui::TableSetupColumn("Files", ImGuiTableColumnFlags_WidthFixed, 90 * s);
    ImGui::TableSetupColumn("Folders", ImGuiTableColumnFlags_WidthFixed, 80 * s);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8 * s, 6 * s));
    ImGui::TableHeadersRow();
    ImGui::PopStyleVar();

    float lineH = ImGui::GetTextLineHeight();
    float rowH = std::round(std::max(lineH, 20 * s) + 8 * s);
    float indent = 20 * s;
    SizeNode* openMenu = nullptr;
    ImGuiListClipper clip;
    clip.Begin(int(rows.size()), rowH);
    while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            const Row& r = rows[i];
            SizeNode* n = r.node;
            bool folder = r.kind == RowKind::Folder;
            uint64_t bytes = folder ? n->bytes.load() : r.bytes;
            uint64_t files = folder ? n->files.load() : r.files;
            const SizeNode* parent = folder ? n->parent : n;
            uint64_t parentBytes = parent ? parent->bytes.load() : 0;
            bool done = folder ? n->Done() : r.kind == RowKind::Files || n->Done();
            bool sel = folder && sizesSel_ == n;

            ImGui::TableNextRow(0, rowH);
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(i);
            ImVec2 rowTop = ImGui::GetCursorScreenPos();
            ImGui::PushStyleColor(ImGuiCol_Header, p.accentLo);
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, sel ? p.accentLo : p.surface2);
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, p.accentLo);
            // No item spacing: ImGui grows a selectable by it, and the
            // highlight would spill onto the rows above and below.
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0));
            ImGui::Selectable("##row", sel, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                              ImVec2(0, rowH));
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);
            bool hovered = ImGui::IsItemHovered();
            if (folder) {
                if (ImGui::IsItemClicked(0) || ImGui::IsItemClicked(1)) sizesSel_ = n;
                if (hovered && ImGui::IsMouseDoubleClicked(0)) Navigate(scan_->PathOf(n));
                if (ImGui::IsItemClicked(1)) openMenu = n;
                if (n->error && hovered) ImGui::SetTooltip("Couldn't read this folder (%s)", ErrorText(n->error).c_str());
            } else if (r.kind == RowKind::Files && hovered && ImGui::IsMouseDoubleClicked(0)) {
                // The files themselves: the folder in details, biggest first.
                std::wstring path = scan_->PathOf(n);
                sizesView_ = false;
                Navigate(path);
                setView(false, true);
                SetSort(SortKey::Size, true);
            }
            float cy = rowTop.y + (rowH - lineH) / 2, mid = rowTop.y + rowH / 2;
            float x = rowTop.x + r.depth * indent;

            // The arrow, for a folder with folders in it (or not read yet).
            bool expandable = folder && (!n->listed || !n->kids.empty());
            if (expandable) {
                bool open = sizesOpen_.count(n) > 0;
                ImGui::SetCursorScreenPos(ImVec2(x, rowTop.y));
                if (ImGui::InvisibleButton("##arrow", ImVec2(18 * s, rowH))) {
                    if (open) sizesOpen_.erase(n);
                    else sizesOpen_.insert(n);
                    sizesSel_ = n;
                }
                ImGui::PushFont(nullptr, 10);
                DrawCentred(dl, ImVec2(x + 9 * s, mid), open ? icon::ChevronDown : icon::ChevronRight,
                            U32(ImGui::IsItemHovered() ? p.text : p.muted));
                ImGui::PopFont();
            }
            x += 20 * s;
            const char* glyph = folder ? icon::Folder : r.kind == RowKind::Files ? icon::Files : icon::More;
            ImGui::PushFont(nullptr, 15);
            DrawCentred(dl, ImVec2(x + 8 * s, mid), glyph, U32(folder && !n->error ? p.accent : p.muted, folder ? 0.85f : 1));
            ImGui::PopFont();
            x += 24 * s;
            std::string name = folder ? Narrow(n->name)
                               : r.kind == RowKind::Files
                                   ? plural(r.files, "file", "files")
                                   : grouped(r.count) + " more folder" + (r.count == 1 ? "" : "s");
            if (folder && n->error) name += "  (couldn't read)";
            float nameW = rowTop.x + ImGui::GetContentRegionAvail().x - x;
            dl->AddText(ImVec2(x, cy), U32(folder && !n->error ? p.text : p.muted), Ellipsis(name, nameW).c_str());

            auto right = [&](int col, const std::string& text, ImU32 colour) {
                if (!ImGui::TableSetColumnIndex(col)) return;
                float cx = ImGui::GetCursorScreenPos().x, w = ImGui::GetContentRegionAvail().x;
                std::string t = Ellipsis(text, w);
                dl->AddText(ImVec2(cx + w - ImGui::CalcTextSize(t.c_str()).x, cy), colour, t.c_str());
            };
            // Still counting: the numbers are muted until they're final.
            right(1, FormatSize(bytes), U32(done ? p.text : p.muted));
            if (ImGui::TableSetColumnIndex(2)) {
                float cx = ImGui::GetCursorScreenPos().x, w = ImGui::GetContentRegionAvail().x;
                float f = parentBytes ? float(double(bytes) / double(parentBytes)) : 0;
                char pct[16];
                snprintf(pct, sizeof pct, "%.0f%%", f * 100);
                ImGui::PushFont(nullptr, kSmall);
                float pw = ImGui::CalcTextSize("100%").x;
                float barW = std::max(0.0f, w - pw - 8 * s);
                float by = mid - 3 * s;
                dl->AddRectFilled(ImVec2(cx, by), ImVec2(cx + barW, by + 6 * s), U32(p.border), 3 * s);
                if (f > 0)
                    dl->AddRectFilled(ImVec2(cx, by), ImVec2(cx + std::max(2 * s, barW * f), by + 6 * s),
                                      U32(p.accent, folder ? 1.0f : 0.5f), 3 * s);
                dl->AddText(ImVec2(cx + w - ImGui::CalcTextSize(pct).x, TextTop(mid)), U32(p.muted), pct);
                ImGui::PopFont();
            }
            right(3, grouped(files), U32(p.muted));
            if (folder) right(4, grouped(n->folders.load()), U32(p.muted));
            else if (r.kind == RowKind::More) right(4, grouped(r.count), U32(p.muted));
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);

    if (openMenu) {
        sizesMenuNode_ = openMenu;
        ImGui::OpenPopup("##sizemenu");
    }
    if (ImGui::BeginPopup("##sizemenu")) {
        sizesMenu(sizesMenuNode_);
        ImGui::EndPopup();
    }
}

void App::sizesStatus(float x, float y) {
    const Palette& p = Colours();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!scan_) return;
    std::string text;
    if (scan_->Done()) text = "Scanned " + Narrow(scan_->Root()) + " in " + duration(scan_->Seconds());
    else text = "Scanning\xE2\x80\xA6 " + plural(scan_->Top()->folders.load(), "folder", "folders") + " so far  \xC2\xB7  " +
                duration(scan_->Seconds());
    if (uint64_t u = scan_->Unreadable())
        text += "  \xC2\xB7  " + plural(u, "folder", "folders") + " couldn't be read (no access)";
    dl->AddText(ImVec2(x, y), U32(p.muted), text.c_str());
}

// sizesKeys: arrows move through the rows, Right/Left open and close,
// Enter goes in, Delete recycles, F5 rescans.
void App::sizesKeys() {
    ImGuiIO& io = ImGui::GetIO();
    auto pressed = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };
    if (io.KeyCtrl && pressed(ImGuiKey_1)) setView(true, true);
    if (io.KeyCtrl && pressed(ImGuiKey_2)) setView(false, true);
    if (pressed(ImGuiKey_F5)) startScan();
    if (sizesRows_.empty()) return;
    auto at = std::find(sizesRows_.begin(), sizesRows_.end(), sizesSel_);
    int i = at == sizesRows_.end() ? -1 : int(at - sizesRows_.begin());
    if (pressed(ImGuiKey_DownArrow)) sizesSel_ = sizesRows_[std::min(i + 1, int(sizesRows_.size()) - 1)];
    if (pressed(ImGuiKey_UpArrow)) sizesSel_ = sizesRows_[std::max(i - 1, 0)];
    if (!sizesSel_ || i < 0) return;
    if (pressed(ImGuiKey_RightArrow)) sizesOpen_.insert(sizesSel_);
    if (pressed(ImGuiKey_LeftArrow)) {
        if (sizesOpen_.count(sizesSel_)) sizesOpen_.erase(sizesSel_);
        else if (sizesSel_->parent && std::find(sizesRows_.begin(), sizesRows_.end(), sizesSel_->parent) != sizesRows_.end())
            sizesSel_ = sizesSel_->parent;
    }
    if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter)) Navigate(scan_->PathOf(sizesSel_));
    else if (pressed(ImGuiKey_Delete)) recycleNode(sizesSel_, io.KeyShift);
}

}  // namespace bones
