// Tests for the pure helpers. Builds anywhere:
//   g++ -std=c++17 -Isrc tests/tests.cpp src/paths.cpp -o tests && ./tests
#include <cstdio>
#include <string>
#include <vector>

#include "paths.h"

using namespace shelf;

static int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                \
        }                                                              \
    } while (0)

static Entry file(const wchar_t* name, uint64_t size = 0, uint64_t created = 0, uint64_t modified = 0) {
    Entry e;
    e.name = name;
    e.ext = LowerExt(name);
    e.size = size;
    e.created = created;
    e.modified = modified;
    return e;
}

static Entry folder(const wchar_t* name) {
    Entry e = file(name);
    e.dir = true;
    e.ext.clear();
    return e;
}

static std::vector<std::wstring> names(const std::vector<Entry>& es, const std::vector<int>& idx) {
    std::vector<std::wstring> out;
    for (int i : idx) out.push_back(es[i].name);
    return out;
}

static std::vector<int> all(size_t n) {
    std::vector<int> idx(n);
    for (size_t i = 0; i < n; i++) idx[i] = int(i);
    return idx;
}

static void testNatural() {
    CHECK(NaturalCompare(L"img2", L"img10") < 0);
    CHECK(NaturalCompare(L"img10", L"img2") > 0);
    CHECK(NaturalCompare(L"Apple", L"apple") == 0);
    CHECK(NaturalCompare(L"a", L"B") < 0);
    CHECK(NaturalCompare(L"file", L"file1") < 0);
    CHECK(NaturalCompare(L"x007", L"x7") > 0);  // same value, more zeros after
    CHECK(NaturalCompare(L"x8", L"x007") > 0);
    CHECK(NaturalCompare(L"99999999999999999999b", L"100000000000000000000a") < 0);  // longer than 64 bits
}

static void testOrder() {
    std::vector<Entry> es = {file(L"b10.txt", 5, 3, 9), folder(L"zeta"), file(L"b2.txt", 50, 1, 7),
                             folder(L"Alpha"), file(L"a.jpg", 50, 2, 8)};
    std::vector<int> idx = all(es.size());
    Order(es, {}, {SortKey::Name, false}, idx);
    CHECK((names(es, idx) == std::vector<std::wstring>{L"Alpha", L"zeta", L"a.jpg", L"b2.txt", L"b10.txt"}));

    Order(es, {}, {SortKey::Name, true}, idx);
    CHECK((names(es, idx) == std::vector<std::wstring>{L"zeta", L"Alpha", L"b10.txt", L"b2.txt", L"a.jpg"}));

    // Size: ties fall back to name A to Z, even descending.
    Order(es, {}, {SortKey::Size, true}, idx);
    CHECK((names(es, idx) == std::vector<std::wstring>{L"Alpha", L"zeta", L"a.jpg", L"b2.txt", L"b10.txt"}));
    Order(es, {}, {SortKey::Size, false}, idx);
    CHECK((names(es, idx) == std::vector<std::wstring>{L"Alpha", L"zeta", L"b10.txt", L"a.jpg", L"b2.txt"}));

    Order(es, {}, {SortKey::Created, false}, idx);
    CHECK((names(es, idx) == std::vector<std::wstring>{L"Alpha", L"zeta", L"b2.txt", L"a.jpg", L"b10.txt"}));
    Order(es, {}, {SortKey::Modified, true}, idx);
    CHECK((names(es, idx) == std::vector<std::wstring>{L"Alpha", L"zeta", L"b10.txt", L"a.jpg", L"b2.txt"}));

    // Folders in with the files.
    Sort mixed{SortKey::Name, false, false};
    Order(es, {}, mixed, idx);
    CHECK((names(es, idx) == std::vector<std::wstring>{L"a.jpg", L"Alpha", L"b2.txt", L"b10.txt", L"zeta"}));
    Sort bySize{SortKey::Size, true, false};
    Order(es, {}, bySize, idx);
    CHECK((names(es, idx) == std::vector<std::wstring>{L"a.jpg", L"b2.txt", L"b10.txt", L"Alpha", L"zeta"}));

    std::vector<std::wstring> types = {L"Text", L"Folder", L"Text", L"Folder", L"JPEG"};
    Order(es, types, {SortKey::Type, false}, idx);
    CHECK((names(es, idx) == std::vector<std::wstring>{L"Alpha", L"zeta", L"a.jpg", L"b2.txt", L"b10.txt"}));
}

static void testMatches() {
    CHECK(Matches(L"Holiday Photo.JPG", L"photo"));
    CHECK(!Matches(L"Holiday Photo.JPG", L"video"));
    CHECK(Matches(L"Holiday Photo.JPG", L"*.jpg"));
    CHECK(!Matches(L"Holiday Photo.JPG.txt", L"*.jpg"));
    CHECK(Matches(L"img_0042.png", L"img_00??.*"));
    CHECK(Matches(L"anything", L""));
}

static void testPaths() {
    CHECK(Normalise(L"c:") == L"C:\\");
    // What CommandLineToArgvW makes of "C:\" from the folder open verb.
    CHECK(Normalise(L"C:\"") == L"C:\\");
    CHECK(Normalise(L"C:/Users/seven/") == L"C:\\Users\\seven");
    CHECK(Normalise(L"\"C:\\Users\\seven\"") == L"C:\\Users\\seven");
    CHECK(Normalise(L"\\\\wsl$\\Debian\\root") == L"\\\\wsl.localhost\\Debian\\root");
    CHECK(Normalise(L"\\\\wsl.localhost\\Debian") == L"\\\\wsl.localhost\\Debian\\");
    CHECK(Normalise(L"C:\\a\\\\b") == L"C:\\a\\b");

    CHECK(Root(L"C:\\Users") == L"C:\\");
    CHECK(Root(L"\\\\wsl.localhost\\Debian\\root\\x") == L"\\\\wsl.localhost\\Debian\\");
    CHECK(IsRoot(L"C:\\"));
    CHECK(IsRoot(L"\\\\nas\\media\\"));
    CHECK(!IsRoot(L"C:\\Users"));

    CHECK(Parent(L"C:\\Users\\seven") == L"C:\\Users");
    CHECK(Parent(L"C:\\Users") == L"C:\\");
    CHECK(Parent(L"C:\\") == L"");
    CHECK(Parent(L"\\\\wsl.localhost\\Debian\\root") == L"\\\\wsl.localhost\\Debian\\");
    CHECK(Parent(L"\\\\wsl.localhost\\Debian\\") == L"");

    CHECK(Join(L"C:\\", L"a") == L"C:\\a");
    CHECK(Join(L"C:\\a", L"b") == L"C:\\a\\b");
    CHECK(BaseName(L"C:\\a\\b.txt") == L"b.txt");

    CHECK(IsLinux(L"\\\\wsl.localhost\\Debian\\root"));
    CHECK(!IsLinux(L"C:\\root"));
    CHECK(FromLinux(L"/root/go", L"Debian") == L"\\\\wsl.localhost\\Debian\\root\\go");
    CHECK(FromLinux(L"/", L"Debian") == L"\\\\wsl.localhost\\Debian\\");
    CHECK(ToLinux(L"\\\\wsl.localhost\\Debian\\root\\go") == L"/root/go");
    CHECK(ToLinux(L"\\\\wsl.localhost\\Debian\\") == L"/");
    CHECK(ToLinux(L"C:\\x") == L"");

    CHECK(LowerExt(L"Photo.JPG") == L".jpg");
    CHECK(LowerExt(L".bashrc") == L"");
    CHECK(LowerExt(L"Makefile") == L"");
    CHECK(SamePath(L"c:/users/", L"C:\\Users"));
}

static void testCrumbs() {
    auto c = Crumbs(L"C:\\Users\\seven");
    CHECK(c.size() == 3);
    CHECK(c[0].label == L"C:" && c[0].path == L"C:\\");
    CHECK(c[2].label == L"seven" && c[2].path == L"C:\\Users\\seven");
    c = Crumbs(L"\\\\wsl.localhost\\Debian\\root\\go");
    CHECK(c.size() == 3);
    CHECK(c[0].label == L"Debian" && c[0].path == L"\\\\wsl.localhost\\Debian\\");
    CHECK(c[1].path == L"\\\\wsl.localhost\\Debian\\root");
    c = Crumbs(L"\\\\nas\\media\\films");
    CHECK(c.size() == 2 && c[0].label == L"\\\\nas\\media");
    CHECK(Crumbs(L"D:\\").size() == 1);
}

static void testUnique() {
    std::vector<Entry> es = {folder(L"New folder"), folder(L"new folder (2)"), file(L"a.txt")};
    CHECK(UniqueName(es, L"New folder") == L"New folder (3)");
    CHECK(UniqueName(es, L"Other") == L"Other");
    CHECK(UniqueName(es, L"A.txt") == L"A (2).txt");
}

static void testFormat() {
    CHECK(FormatSize(0) == "0 bytes");
    CHECK(FormatSize(1) == "1 byte");
    CHECK(FormatSize(1023) == "1023 bytes");
    CHECK(FormatSize(4300) == "4.20 KB");
    CHECK(FormatSize(38ull << 20) == "38.0 MB");
    CHECK(FormatSize(uint64_t(1.27 * (1ull << 30))) == "1.27 GB");
    CHECK(FormatSize(500ull << 30) == "500 GB");
}

static void testMediaHeavy() {
    std::vector<Entry> es = {folder(L"a"), folder(L"b"), file(L"1.jpg"), file(L"2.png"), file(L"notes.txt")};
    CHECK(!MediaHeavy(es));  // only two pictures
    es.push_back(file(L"3.mp4"));
    CHECK(MediaHeavy(es));  // three of four files
    for (int i = 0; i < 4; i++) es.push_back(file(L"doc.txt"));
    CHECK(!MediaHeavy(es));  // three of eight
    for (int i = 0; i < 17; i++) es.push_back(file(L"x.webp"));
    CHECK(MediaHeavy(es));  // twenty, among anything
    std::vector<Entry> hiddenPics = {file(L".a.jpg"), file(L".b.jpg"), file(L".c.jpg"), file(L"z.txt")};
    CHECK(!MediaHeavy(hiddenPics));
    CHECK(!MediaHeavy({folder(L"only"), folder(L"folders")}));
    CHECK(IsMedia(L".mkv") && IsMedia(L".jpeg") && !IsMedia(L".pdf") && !IsMedia(L".ico"));
}

static void testKinds() {
    CHECK(KindFor(L".jpg") == ThumbKind::Image);
    CHECK(KindFor(L".mp4") == ThumbKind::Shell);
    CHECK(KindFor(L".txt") == ThumbKind::Icon);
    CHECK(OwnIcon(L".exe"));
    CHECK(!OwnIcon(L".txt"));
}

static void testUses() {
    std::vector<Use> u;
    const int64_t day = 86400, t = 1000 * day;
    RecordUse(u, L"C:\\", t);  // roots aren't counted
    CHECK(u.empty());
    RecordUse(u, L"C:\\a", t);
    RecordUse(u, L"c:\\A\\", t);  // the same folder
    CHECK(u.size() == 1 && u[0].count == 2);
    RecordUse(u, L"C:\\b", t);
    RecordUse(u, L"D:\\x", t);
    RecordUse(u, L"\\\\wsl.localhost\\Debian\\root\\go", t);
    auto top = TopUses(u, L"C:\\", 3, t);
    CHECK(top.size() == 2 && top[0].path == L"C:\\a" && top[1].path == L"C:\\b");
    CHECK(TopUses(u, L"D:\\", 3, t).size() == 1);
    CHECK(TopUses(u, L"\\\\wsl.localhost\\Debian\\", 3, t).size() == 1);
    // Old visits fade: three visits two months ago lose to one today.
    std::vector<Use> v = {{L"C:\\old", 3, t - 60 * day}, {L"C:\\new", 1, t}};
    CHECK(TopUses(v, L"C:\\", 1, t)[0].path == L"C:\\new");
    // Full: the weakest goes.
    RecordUse(v, L"C:\\third", t, 2);
    CHECK(v.size() == 2 && TopUses(v, L"C:\\", 2, t)[1].path == L"C:\\third");
    ForgetUse(v, L"c:\\NEW");
    CHECK(v.size() == 1);
}

int main() {
    testNatural();
    testOrder();
    testMatches();
    testPaths();
    testCrumbs();
    testUnique();
    testFormat();
    testKinds();
    testUses();
    testMediaHeavy();
    if (failures) {
        std::printf("%d failed\n", failures);
        return 1;
    }
    std::printf("all tests passed\n");
    return 0;
}
