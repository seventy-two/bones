// Folder sizes, TreeSize-style: a background scan of everything under a
// folder that adds up bytes, files and folders as it goes, so the tree
// can be drawn while it's still running.
#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace shelf {

// SizeNode is a folder in the scan. The totals are everything under it
// and grow while the scan runs. ownBytes, ownFiles, kids and error are
// set once, before listed turns true, and never change after.
struct SizeNode {
    std::wstring name;  // "" for the top (its path is the scan's root)
    SizeNode* parent = nullptr;
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> files{0};
    std::atomic<uint64_t> folders{0};
    uint64_t ownBytes = 0;  // its own files, not its folders'
    uint64_t ownFiles = 0;
    std::vector<std::unique_ptr<SizeNode>> kids;
    DWORD error = 0;  // why it couldn't be listed (0 if it could)
    std::atomic<bool> listed{false};
    std::atomic<bool> removed{false};  // recycled since the scan
    std::atomic<int> pending{1};       // itself + unfinished kids; 0 = done

    bool Done() const { return pending.load(std::memory_order_acquire) == 0; }
};

// SortedKids gives a listed node's folders that are still there, biggest
// first (an unlisted one has none yet).
std::vector<SizeNode*> SortedKids(const SizeNode* n);

class SizeScan {
public:
    // The scan starts at once on threads of its own (below normal
    // priority); done is called on one of them when it's finished.
    SizeScan(std::wstring root, std::function<void()> done, int threads = 4);
    ~SizeScan();  // stops and waits for the threads
    SizeScan(const SizeScan&) = delete;
    SizeScan& operator=(const SizeScan&) = delete;

    const std::wstring& Root() const { return root_; }
    SizeNode* Top() { return &top_; }
    bool Done() const { return top_.Done(); }
    // Seconds since it started, or how long it took once done.
    double Seconds() const;
    // Unreadable is how many folders couldn't be listed (no access…).
    uint64_t Unreadable() const { return unreadable_.load(); }

    std::wstring PathOf(const SizeNode* n) const;
    // Covers says whether path is the root or inside it.
    bool Covers(const std::wstring& path) const;
    // Find gives the node for a path inside the root, or nullptr if it
    // isn't there or isn't listed yet.
    SizeNode* Find(const std::wstring& path);
    // Removed takes a finished node out of the totals (after it's been
    // recycled or deleted).
    void Removed(SizeNode* n);

private:
    void work();
    void list(SizeNode* n);
    void finish(SizeNode* n);

    std::wstring root_;
    std::function<void()> done_;
    SizeNode top_;
    std::vector<std::thread> threads_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<SizeNode*> todo_;  // a stack: depth first keeps it small
    int active_ = 0;
    bool stop_ = false;
    std::atomic<bool> stopping_{false};
    std::atomic<uint64_t> unreadable_{0};
    ULONGLONG started_ = 0;
    std::atomic<ULONGLONG> finished_{0};
};

}  // namespace shelf
