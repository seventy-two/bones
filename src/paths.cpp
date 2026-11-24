#include "paths.h"

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <unordered_set>

namespace shelf {

namespace {

const std::wstring kWsl = L"\\\\wsl.localhost\\";
const std::wstring kWslOld = L"\\\\wsl$\\";

bool startsWithI(const std::wstring& s, const std::wstring& prefix) {
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); i++)
        if (std::towlower(s[i]) != std::towlower(prefix[i])) return false;
    return true;
}

bool isDigit(wchar_t c) { return c >= L'0' && c <= L'9'; }

// wildcard matches * and ? over the whole of s, ignoring case.
bool wildcard(const wchar_t* s, const wchar_t* p) {
    const wchar_t *star = nullptr, *back = nullptr;
    while (*s) {
        if (*p == L'*') {
            star = ++p;
            back = s;
        } else if (*p == L'?' || std::towlower(*p) == std::towlower(*s)) {
            p++;
            s++;
        } else if (star) {
            p = star;
            s = ++back;
        } else {
            return false;
        }
    }
    while (*p == L'*') p++;
    return !*p;
}

}  // namespace

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = wchar_t(std::towlower(c));
    return s;
}

int NaturalCompare(const std::wstring& a, const std::wstring& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (isDigit(a[i]) && isDigit(b[j])) {
            // Compare the numbers: skip leading zeros, then longer is
            // bigger, then digit by digit.
            size_t si = i, sj = j;
            while (si < a.size() && a[si] == L'0') si++;
            while (sj < b.size() && b[sj] == L'0') sj++;
            size_t ei = si, ej = sj;
            while (ei < a.size() && isDigit(a[ei])) ei++;
            while (ej < b.size() && isDigit(b[ej])) ej++;
            if (ei - si != ej - sj) return ei - si < ej - sj ? -1 : 1;
            for (size_t k = 0; k < ei - si; k++)
                if (a[si + k] != b[sj + k]) return a[si + k] < b[sj + k] ? -1 : 1;
            // Equal values: fewer leading zeros first.
            if (ei - i != ej - j) return ei - i < ej - j ? -1 : 1;
            i = ei;
            j = ej;
            continue;
        }
        wchar_t x = wchar_t(std::towlower(a[i])), y = wchar_t(std::towlower(b[j]));
        if (x != y) return x < y ? -1 : 1;
        i++;
        j++;
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    return 0;
}

void Order(const std::vector<Entry>& entries, const std::vector<std::wstring>& typeOf, Sort sort, std::vector<int>& idx) {
    auto byName = [&](int x, int y) { return NaturalCompare(entries[x].name, entries[y].name); };
    std::stable_sort(idx.begin(), idx.end(), [&](int x, int y) {
        const Entry &a = entries[x], &b = entries[y];
        if (sort.foldersFirst && a.dir != b.dir) return a.dir;  // either way round
        int c = 0;
        switch (sort.key) {
        case SortKey::Name: c = byName(x, y); break;
        case SortKey::Modified: c = a.modified < b.modified ? -1 : a.modified > b.modified ? 1 : 0; break;
        case SortKey::Created: c = a.created < b.created ? -1 : a.created > b.created ? 1 : 0; break;
        case SortKey::Size: c = a.size < b.size ? -1 : a.size > b.size ? 1 : 0; break;
        case SortKey::Type:
            if (size_t(x) < typeOf.size() && size_t(y) < typeOf.size()) c = NaturalCompare(typeOf[x], typeOf[y]);
            break;
        }
        if (c == 0 && sort.key != SortKey::Name) c = byName(x, y);  // ties by name, A to Z
        else if (sort.descending) c = -c;
        return c < 0;
    });
}

bool Matches(const std::wstring& name, const std::wstring& filter) {
    if (filter.empty()) return true;
    if (filter.find_first_of(L"*?") != std::wstring::npos) return wildcard(name.c_str(), filter.c_str());
    return Lower(name).find(Lower(filter)) != std::wstring::npos;
}

std::wstring Normalise(std::wstring p) {
    std::replace(p.begin(), p.end(), L'/', L'\\');
    // Trim spaces and quotes pasted along with a path.
    while (!p.empty() && (p.back() == L' ' || p.back() == L'"')) p.pop_back();
    size_t start = 0;
    while (start < p.size() && (p[start] == L' ' || p[start] == L'"')) start++;
    p = p.substr(start);
    if (startsWithI(p, kWslOld)) p = kWsl + p.substr(kWslOld.size());
    if (p.size() == 2 && p[1] == L':') p += L'\\';
    if (p.size() >= 2 && p[1] == L':') p[0] = wchar_t(std::towupper(p[0]));
    // Collapse doubled backslashes after the UNC start.
    std::wstring out;
    for (size_t i = 0; i < p.size(); i++) {
        if (p[i] == L'\\' && !out.empty() && out.back() == L'\\' && i > 1) continue;
        out += p[i];
    }
    if (out.size() > 1 && out.back() == L'\\' && !IsRoot(out)) out.pop_back();
    if (IsRoot(out) && out.back() != L'\\') out += L'\\';
    return out;
}

std::wstring Root(const std::wstring& path) {
    if (path.size() >= 2 && path[1] == L':') return path.substr(0, 2) + L"\\";
    if (startsWithI(path, L"\\\\")) {
        // \\server\share\ (or \\wsl.localhost\distro\)
        size_t a = path.find(L'\\', 2);
        if (a == std::wstring::npos) return path + L"\\";
        size_t b = path.find(L'\\', a + 1);
        if (b == std::wstring::npos) return path + L"\\";
        return path.substr(0, b + 1);
    }
    return L"";
}

bool IsRoot(const std::wstring& path) {
    if (path.empty()) return false;
    std::wstring r = Root(path);
    return !r.empty() && (path == r || path + L"\\" == r);
}

std::wstring Parent(const std::wstring& path) {
    if (IsRoot(path)) return L"";
    size_t at = path.find_last_of(L'\\');
    if (at == std::wstring::npos) return L"";
    std::wstring up = path.substr(0, at);
    std::wstring root = Root(path);
    if (up.size() + 1 <= root.size()) return root;
    return up;
}

std::wstring Join(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    if (dir.back() == L'\\') return dir + name;
    return dir + L"\\" + name;
}

std::wstring BaseName(const std::wstring& path) {
    if (IsRoot(path)) return path;
    size_t at = path.find_last_of(L'\\');
    return at == std::wstring::npos ? path : path.substr(at + 1);
}

bool IsLinux(const std::wstring& path) { return startsWithI(path, kWsl) || startsWithI(path, kWslOld); }

std::wstring FromLinux(const std::wstring& linuxPath, const std::wstring& distro) {
    std::wstring p = linuxPath;
    std::replace(p.begin(), p.end(), L'/', L'\\');
    return Normalise(kWsl + distro + p);
}

std::wstring ToLinux(const std::wstring& path) {
    if (!IsLinux(path)) return L"";
    std::wstring root = Root(path);
    std::wstring rest = path.size() > root.size() ? path.substr(root.size()) : L"";
    std::replace(rest.begin(), rest.end(), L'\\', L'/');
    return L"/" + rest;
}

std::wstring LowerExt(const std::wstring& name) {
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0) return L"";
    return Lower(name.substr(dot));
}

bool SamePath(const std::wstring& a, const std::wstring& b) { return Lower(Normalise(a)) == Lower(Normalise(b)); }

std::vector<Crumb> Crumbs(const std::wstring& path) {
    std::vector<Crumb> out;
    std::wstring root = Root(path);
    if (root.empty()) return out;
    std::wstring label;
    if (IsLinux(path)) {
        label = root.substr(kWsl.size(), root.size() - kWsl.size() - 1);  // the distro's name
    } else if (root.size() == 3) {
        label = root.substr(0, 2);
    } else {
        label = root.substr(0, root.size() - 1);
    }
    out.push_back({label, root});
    std::wstring at = root;
    size_t i = root.size();
    while (i < path.size()) {
        size_t next = path.find(L'\\', i);
        if (next == std::wstring::npos) next = path.size();
        std::wstring part = path.substr(i, next - i);
        if (!part.empty()) {
            at = Join(at, part);
            out.push_back({part, at});
        }
        i = next + 1;
    }
    return out;
}

std::wstring UniqueName(const std::vector<Entry>& entries, const std::wstring& base) {
    std::unordered_set<std::wstring> taken;
    for (const auto& e : entries) taken.insert(Lower(e.name));
    if (!taken.count(Lower(base))) return base;
    std::wstring stem = base, ext;
    size_t dot = base.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) {
        stem = base.substr(0, dot);
        ext = base.substr(dot);
    }
    for (int n = 2;; n++) {
        std::wstring name = stem + L" (" + std::to_wstring(n) + L")" + ext;
        if (!taken.count(Lower(name))) return name;
    }
}

std::string FormatSize(uint64_t bytes) {
    char buf[32];
    if (bytes < 1024) {
        snprintf(buf, sizeof buf, "%llu %s", (unsigned long long)bytes, bytes == 1 ? "byte" : "bytes");
        return buf;
    }
    const char* units[] = {"KB", "MB", "GB", "TB", "PB"};
    double v = double(bytes) / 1024;
    int u = 0;
    while (v >= 1024 && u < 4) {
        v /= 1024;
        u++;
    }
    // Three significant figures, like Explorer's status bar.
    if (v < 10) snprintf(buf, sizeof buf, "%.2f %s", v, units[u]);
    else if (v < 100) snprintf(buf, sizeof buf, "%.1f %s", v, units[u]);
    else snprintf(buf, sizeof buf, "%.0f %s", v, units[u]);
    return buf;
}

ThumbKind KindFor(const std::wstring& e) {
    static const std::unordered_set<std::wstring> images = {
        L".jpg", L".jpeg", L".jpe", L".jfif", L".png", L".gif", L".bmp", L".dib", L".tif", L".tiff", L".webp",
        L".heic", L".heif", L".avif", L".jxr", L".wdp", L".ico", L".dds", L".jxl",
        // Camera raws, when Windows has the Raw Image Extension.
        L".cr2", L".cr3", L".nef", L".arw", L".dng", L".orf", L".rw2", L".raf", L".srw", L".pef",
    };
    static const std::unordered_set<std::wstring> shell = {
        L".mp4", L".m4v", L".mkv", L".webm", L".mov", L".avi", L".wmv", L".flv", L".ts", L".m2ts", L".mts",
        L".mpg", L".mpeg", L".3gp", L".ogv",
        L".pdf", L".psd", L".svg", L".epub",
        L".docx", L".xlsx", L".pptx", L".3mf", L".stl", L".glb", L".fbx", L".obj",
        L".mp3", L".flac", L".m4a", L".wma",  // album art
        L".exe", L".lnk", L".url", L".msi", L".cur", L".ani", L".scr",
    };
    if (images.count(e)) return ThumbKind::Image;
    if (shell.count(e)) return ThumbKind::Shell;
    return ThumbKind::Icon;
}

bool OwnIcon(const std::wstring& e) {
    return e == L".exe" || e == L".lnk" || e == L".url" || e == L".msi" || e == L".cur" || e == L".ani" ||
           e == L".scr" || e == L".ico";
}

}  // namespace shelf
