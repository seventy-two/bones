// Thumbnails and icons.
//
// Thumbnails are made on worker threads, newest wanted first: whatever is
// on screen now beats what was on screen a moment ago, and anything
// scrolled past and gone is dropped from the queue. Images are decoded
// with WIC straight at a small size (JPEGs decode at 1/2, 1/4 or 1/8 in
// the decoder itself, so a 24 MP photo never decodes in full), turned
// the way the camera says, and kept in our own disk cache keyed by path,
// size and time, which also covers WSL and network folders, where
// Explorer's thumbnail cache doesn't. Videos, PDFs and the like go to the
// shell's thumbnailers on threads of their own.
//
// Icons come from the system image list, one texture per icon, on the
// window's thread.
#pragma once

#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "imgui.h"
#include "paths.h"

namespace shelf {

// Tex is a picture on the GPU, for ImGui. For icons, content is the part
// with anything drawn in it (the image lists pad small icons out to the
// big size).
struct Tex {
    ID3D11ShaderResourceView* srv = nullptr;
    int width = 0, height = 0;
    ImVec2 uv0{0, 0}, uv1{1, 1};  // the content's corners
    int contentW = 0, contentH = 0;
    size_t bytes = 0;
    bool icon = false;  // the shell gave an icon rather than a picture
    Tex() = default;
    Tex(const Tex&) = delete;
    Tex& operator=(const Tex&) = delete;
    ~Tex() {
        if (srv) srv->Release();
    }
    ImTextureRef ref() const { return ImTextureRef((ImTextureID)(intptr_t)srv); }
};

// The long edge thumbnails are made at.
constexpr int kThumbSize = 384;

struct ThumbStats {
    int queued = 0, working = 0;
    int made = 0, fromCache = 0, failed = 0;
    double makeMs = 0, cacheMs = 0;  // totals, for averages
};

class Thumbs {
public:
    Thumbs(ID3D11Device* device, std::function<void()> wake);
    ~Thumbs();

    // NewFrame starts a frame's requests; call before any Want.
    void NewFrame();
    // Want asks for a file's thumbnail. It returns it if ready, else
    // queues it (order: lower comes sooner within a frame). failed says
    // there isn't one (draw the icon).
    const Tex* Want(const std::wstring& path, const Entry& e, int order, bool& failed);
    // Pump frees the least recently drawn when over budget; call once a
    // frame on the window's thread.
    void Pump();
    // Drop forgets queued work (after leaving a folder).
    void Drop();
    ThumbStats Stats();

    // MipSampler adds a draw callback that turns mipmapped sampling on
    // for what follows in the list (ImGui's own sampler only reads the
    // top level, which shimmers when thumbnails are drawn small).
    void MipSampler(ImDrawList* dl);

    // Bench makes thumbnails for every file in a folder with no window,
    // once cold (no cache) and once warm, and returns a report.
    static std::string Bench(ID3D11Device* device, const std::wstring& dir, bool shellToo);

private:
    struct Slot;
    struct Job;
    void imageWorker();
    void shellWorker();
    std::shared_ptr<Slot> next(bool shell);
    void finish(const std::shared_ptr<Slot>& s, std::unique_ptr<Tex> tex, bool viaShell, bool fromCache, double ms);

    ID3D11Device* device_;
    std::function<void()> wake_;
    std::wstring cacheDir_;
    ID3D11SamplerState* sampler_ = nullptr;

    std::mutex mu_;
    std::condition_variable cv_;
    bool quit_ = false;
    uint64_t frame_ = 1;
    std::unordered_map<std::wstring, std::shared_ptr<Slot>> slots_;
    std::vector<std::shared_ptr<Slot>> queue_, shellQueue_;
    size_t bytes_ = 0;
    ThumbStats stats_;
    std::vector<std::thread> threads_;
};

// Icons gives each file type's icon and type name, from the shell, on
// the window's thread.
class Icons {
public:
    explicit Icons(ID3D11Device* device) : device_(device) {}
    // For a folder or file; big is the 256-pixel set (tiles), else 48.
    const Tex* For(const Entry& e, bool big);
    const Tex* ForPath(const std::wstring& path, bool big);  // a drive, a pinned folder
    const std::wstring& TypeName(const Entry& e);

private:
    const Tex* byIndex(int index, bool big);
    ID3D11Device* device_;
    std::unordered_map<std::wstring, int> indexOf_;  // by type ("<dir>", ".txt")
    std::unordered_map<std::wstring, int> pathIndex_;
    std::unordered_map<int, std::unique_ptr<Tex>> big_, small_;
    std::unordered_map<std::wstring, std::wstring> types_;
};

}  // namespace shelf
