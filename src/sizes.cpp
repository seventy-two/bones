#include "sizes.h"

#include <algorithm>

namespace shelf {

namespace {

std::wstring join(const std::wstring& dir, const std::wstring& name) {
    if (!dir.empty() && dir.back() == L'\\') return dir + name;
    return dir + L'\\' + name;
}

// longPath adds the \\?\ prefix, so paths past MAX_PATH still list.
std::wstring longPath(const std::wstring& p) {
    if (p.rfind(L"\\\\?\\", 0) == 0) return p;
    if (p.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + p.substr(2);
    return L"\\\\?\\" + p;
}

bool sameName(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), int(a.size()), b.c_str(), int(b.size()), TRUE) == CSTR_EQUAL;
}

}  // namespace

std::vector<SizeNode*> SortedKids(const SizeNode* n) {
    std::vector<SizeNode*> out;
    if (!n->listed.load(std::memory_order_acquire)) return out;
    out.reserve(n->kids.size());
    for (auto& k : n->kids)
        if (!k->removed.load()) out.push_back(k.get());
    std::sort(out.begin(), out.end(), [](const SizeNode* a, const SizeNode* b) {
        uint64_t x = a->bytes.load(), y = b->bytes.load();
        return x != y ? x > y : a->name < b->name;
    });
    return out;
}

SizeScan::SizeScan(std::wstring root, std::function<void()> done, int threads)
    : root_(std::move(root)), done_(std::move(done)) {
    started_ = GetTickCount64();
    todo_.push_back(&top_);
    for (int i = 0; i < std::max(1, threads); i++) threads_.emplace_back([this] { work(); });
}

SizeScan::~SizeScan() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    stopping_ = true;
    cv_.notify_all();
    for (auto& t : threads_) t.join();
}

double SizeScan::Seconds() const {
    ULONGLONG end = finished_.load();
    return double((end ? end : GetTickCount64()) - started_) / 1000.0;
}

std::wstring SizeScan::PathOf(const SizeNode* n) const {
    std::vector<const std::wstring*> names;
    for (; n && n->parent; n = n->parent) names.push_back(&n->name);
    std::wstring p = root_;
    for (auto it = names.rbegin(); it != names.rend(); ++it) p = join(p, **it);
    return p;
}

bool SizeScan::Covers(const std::wstring& path) const {
    std::wstring r = root_;
    if (!r.empty() && r.back() == L'\\') r.pop_back();
    if (path.size() < r.size() || !sameName(path.substr(0, r.size()), r)) return false;
    return path.size() == r.size() || path[r.size()] == L'\\';
}

SizeNode* SizeScan::Find(const std::wstring& path) {
    if (!Covers(path)) return nullptr;
    std::wstring r = root_;
    if (!r.empty() && r.back() == L'\\') r.pop_back();
    SizeNode* n = &top_;
    size_t at = r.size();
    while (at < path.size()) {
        if (path[at] == L'\\') {
            at++;
            continue;
        }
        size_t end = path.find(L'\\', at);
        if (end == std::wstring::npos) end = path.size();
        std::wstring part = path.substr(at, end - at);
        at = end;
        if (!n->listed.load(std::memory_order_acquire)) return nullptr;
        SizeNode* next = nullptr;
        for (auto& k : n->kids)
            if (!k->removed.load() && sameName(k->name, part)) {
                next = k.get();
                break;
            }
        if (!next) return nullptr;
        n = next;
    }
    return n;
}

void SizeScan::Removed(SizeNode* n) {
    if (!n || !n->parent || n->removed.exchange(true)) return;
    uint64_t b = n->bytes.load(), f = n->files.load(), d = n->folders.load() + 1;
    for (SizeNode* a = n->parent; a; a = a->parent) {
        a->bytes -= std::min(b, a->bytes.load());
        a->files -= std::min(f, a->files.load());
        a->folders -= std::min(d, a->folders.load());
    }
}

void SizeScan::work() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        SizeNode* n;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [&] { return stop_ || !todo_.empty() || active_ == 0; });
            if (stop_ || todo_.empty()) {
                cv_.notify_all();  // the others are done too
                return;
            }
            n = todo_.back();
            todo_.pop_back();
            active_++;
        }
        list(n);
        std::vector<SizeNode*> kids;
        for (auto& k : n->kids) kids.push_back(k.get());
        {
            std::lock_guard<std::mutex> lock(mu_);
            // Pushed in reverse, so they're taken in listing order.
            for (auto it = kids.rbegin(); it != kids.rend(); ++it) todo_.push_back(*it);
            active_--;
        }
        cv_.notify_all();
        finish(n);
    }
}

// list reads one folder: its files' sizes and its subfolders (which go on
// the stack). Folder links (junctions, symlinks) aren't followed, so
// nothing is counted twice or looped. Cloud files that aren't on the
// disk (OneDrive's online-only ones) count as nothing.
void SizeScan::list(SizeNode* n) {
    std::wstring dir = PathOf(n);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(longPath(join(dir, L"*")).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    uint64_t bytes = 0, files = 0;
    if (h == INVALID_HANDLE_VALUE) {
        n->error = GetLastError();
        if (n->error == ERROR_FILE_NOT_FOUND) n->error = 0;  // an empty root
        else unreadable_++;
    } else {
        do {
            if (stopping_.load(std::memory_order_relaxed)) break;
            const wchar_t* name = fd.cFileName;
            if (name[0] == L'.' && (name[1] == 0 || (name[1] == L'.' && name[2] == 0))) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                auto k = std::make_unique<SizeNode>();
                k->name = name;
                k->parent = n;
                n->kids.push_back(std::move(k));
            } else {
                files++;
                if (fd.dwFileAttributes & (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_OFFLINE)) continue;
                bytes += (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    n->ownBytes = bytes;
    n->ownFiles = files;
    n->kids.shrink_to_fit();
    n->pending.fetch_add(int(n->kids.size()));
    n->listed.store(true, std::memory_order_release);
    uint64_t folders = n->kids.size();
    for (SizeNode* a = n; a; a = a->parent) {
        a->bytes += bytes;
        a->files += files;
        a->folders += folders;
    }
}

// finish marks n listed-and-counted; when a node and all its kids are,
// it's done, and so may its parent be.
void SizeScan::finish(SizeNode* n) {
    while (n && n->pending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        if (!n->parent) {
            finished_ = GetTickCount64();
            if (done_ && !stopping_) done_();
        }
        n = n->parent;
    }
}

}  // namespace shelf
