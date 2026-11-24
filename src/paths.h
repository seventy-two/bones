// Pure helpers with no Windows calls: paths and breadcrumbs, the folder
// listing's entries and how they sort and filter, and formatting. Easy to
// test anywhere.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace shelf {

// Entry is one thing in a folder.
struct Entry {
    std::wstring name;
    std::string utf8;   // name, for drawing
    std::wstring ext;   // lower case, with the dot ("" for folders)
    uint64_t size = 0;
    uint64_t created = 0, modified = 0;  // FILETIME ticks (UTC)
    uint32_t attrs = 0;
    bool dir = false;
};

enum class SortKey { Name, Modified, Created, Type, Size };

struct Sort {
    SortKey key = SortKey::Name;
    bool descending = false;
};

// NaturalLess compares names the way Explorer does: ignoring case, with
// runs of digits compared as numbers ("img2" before "img10").
int NaturalCompare(const std::wstring& a, const std::wstring& b);

// Order sorts indices into entries: folders first, then by the key, with
// the name breaking ties. typeOf gives each entry's type name (for
// SortKey::Type); it may be empty for other keys.
void Order(const std::vector<Entry>& entries, const std::vector<std::wstring>& typeOf, Sort sort, std::vector<int>& idx);

// Matches says whether a name passes the filter box: a plain filter
// matches anywhere, ignoring case; with * or ? it's a wildcard over the
// whole name.
bool Matches(const std::wstring& name, const std::wstring& filter);

// Paths. All use backslashes. Roots keep their trailing backslash
// ("C:\", "\\wsl.localhost\Debian\"); other folders don't.
std::wstring Normalise(std::wstring path);
// Root is the root of a path: "C:\" or "\\server\share\".
std::wstring Root(const std::wstring& path);
bool IsRoot(const std::wstring& path);
// Parent is the folder above, or "" at a root.
std::wstring Parent(const std::wstring& path);
std::wstring Join(const std::wstring& dir, const std::wstring& name);
std::wstring BaseName(const std::wstring& path);
// IsLinux says whether the path is inside a WSL distro.
bool IsLinux(const std::wstring& path);
// FromLinux maps a Linux path ("/home/me", "~" not included) into a
// distro: "\\wsl.localhost\<distro>\home\me".
std::wstring FromLinux(const std::wstring& linuxPath, const std::wstring& distro);
// ToLinux gives a WSL path's Linux form ("/home/me"), or "" if it isn't one.
std::wstring ToLinux(const std::wstring& path);
std::wstring LowerExt(const std::wstring& name);
std::wstring Lower(std::wstring s);
bool SamePath(const std::wstring& a, const std::wstring& b);

// Crumb is a step of the breadcrumb bar.
struct Crumb {
    std::wstring label, path;
};
std::vector<Crumb> Crumbs(const std::wstring& path);

// UniqueName gives base, or "base (2)", "base (3)"…, whichever isn't
// taken (names compared ignoring case).
std::wstring UniqueName(const std::vector<Entry>& entries, const std::wstring& base);

// FormatSize: "0 bytes", "812 bytes", "4.2 KB", "38 MB", "1.27 GB".
std::string FormatSize(uint64_t bytes);

// ThumbKind says how a file's picture is made.
enum class ThumbKind {
    Icon,   // its type's icon
    Image,  // decoded ourselves with WIC
    Shell,  // asked of the shell (videos, PDFs, programs' own icons…)
};
ThumbKind KindFor(const std::wstring& lowerExt);
// OwnIcon says whether files of this type each have their own icon
// (programs, shortcuts), so the type's icon won't do.
bool OwnIcon(const std::wstring& lowerExt);

}  // namespace shelf
