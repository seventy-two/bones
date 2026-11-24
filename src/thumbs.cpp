#include "thumbs.h"

#include <commctrl.h>
#include <commoncontrols.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>

#include "fs.h"
#include "imgui_impl_dx11.h"

using Microsoft::WRL::ComPtr;

namespace shelf {

namespace {

constexpr size_t kBudget = size_t(256) << 20;      // GPU bytes of thumbnails kept
constexpr uint64_t kCacheMax = uint64_t(1) << 30;  // disk cache, pruned to 70% when over
constexpr float kJpegQuality = 0.85f;

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

struct Pixels {
    UINT width = 0, height = 0;
    std::vector<BYTE> bgra;  // straight alpha
};

uint64_t fnv1a(const std::wstring& s) {
    uint64_t h = 1469598103934665603ull;
    for (wchar_t c : s) {
        h ^= uint16_t(c);
        h *= 1099511628211ull;
    }
    return h;
}

bool opaque(const Pixels& px) {
    for (size_t i = 3; i < px.bgra.size(); i += 4)
        if (px.bgra[i] != 255) return false;
    return true;
}

// fromBitmap reads an HBITMAP the shell made (premultiplied alpha, or
// none at all) into straight-alpha pixels.
bool fromBitmap(HBITMAP bmp, Pixels& out) {
    BITMAP info{};
    if (!GetObjectW(bmp, sizeof info, &info) || info.bmWidth <= 0 || info.bmHeight <= 0) return false;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = info.bmWidth;
    bi.bmiHeader.biHeight = -info.bmHeight;  // top down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    out.width = UINT(info.bmWidth);
    out.height = UINT(info.bmHeight);
    out.bgra.resize(size_t(out.width) * out.height * 4);
    HDC dc = GetDC(nullptr);
    bool ok = GetDIBits(dc, bmp, 0, out.height, out.bgra.data(), &bi, DIB_RGB_COLORS) == int(out.height);
    ReleaseDC(nullptr, dc);
    if (!ok) return false;
    bool anyAlpha = false;
    for (size_t i = 3; i < out.bgra.size() && !anyAlpha; i += 4) anyAlpha = out.bgra[i] != 0;
    for (size_t i = 0; i < out.bgra.size(); i += 4) {
        BYTE* p = &out.bgra[i];
        if (!anyAlpha) {
            p[3] = 255;
        } else if (p[3] && p[3] != 255) {
            for (int c = 0; c < 3; c++) p[c] = BYTE(std::min(255, p[c] * 255 / p[3]));
        }
    }
    return true;
}

// fromIcon reads an icon's pixels (icons keep straight alpha, or a mask).
bool fromIcon(HICON icon, Pixels& out) {
    ICONINFO ii{};
    if (!GetIconInfo(icon, &ii)) return false;
    bool ok = false;
    BITMAP info{};
    if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof info, &info) && info.bmWidth > 0 && info.bmHeight > 0) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = info.bmWidth;
        bi.bmiHeader.biHeight = -info.bmHeight;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        out.width = UINT(info.bmWidth);
        out.height = UINT(info.bmHeight);
        out.bgra.resize(size_t(out.width) * out.height * 4);
        HDC dc = GetDC(nullptr);
        ok = GetDIBits(dc, ii.hbmColor, 0, out.height, out.bgra.data(), &bi, DIB_RGB_COLORS) == int(out.height);
        bool anyAlpha = false;
        for (size_t i = 3; ok && i < out.bgra.size() && !anyAlpha; i += 4) anyAlpha = out.bgra[i] != 0;
        if (ok && !anyAlpha && ii.hbmMask) {
            std::vector<BYTE> mask(out.bgra.size());
            if (GetDIBits(dc, ii.hbmMask, 0, out.height, mask.data(), &bi, DIB_RGB_COLORS) == int(out.height))
                for (size_t i = 0; i < out.bgra.size(); i += 4) out.bgra[i + 3] = mask[i] ? 0 : 255;
        }
        ReleaseDC(nullptr, dc);
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return ok;
}

// fit shrinks pixels to fit within max a side, if they're bigger.
bool fit(IWICImagingFactory* wic, Pixels& px, UINT max) {
    if (px.width <= max && px.height <= max) return true;
    double s = std::min(double(max) / px.width, double(max) / px.height);
    UINT w = std::max<UINT>(1, UINT(px.width * s + 0.5)), h = std::max<UINT>(1, UINT(px.height * s + 0.5));
    ComPtr<IWICBitmap> bmp;
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(wic->CreateBitmapFromMemory(px.width, px.height, GUID_WICPixelFormat32bppBGRA, px.width * 4,
                                           UINT(px.bgra.size()), px.bgra.data(), &bmp)) ||
        FAILED(wic->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(bmp.Get(), w, h, WICBitmapInterpolationModeHighQualityCubic)))
        return false;
    std::vector<BYTE> out(size_t(w) * h * 4);
    if (FAILED(scaler->CopyPixels(nullptr, w * 4, UINT(out.size()), out.data()))) return false;
    px.width = w;
    px.height = h;
    px.bgra = std::move(out);
    return true;
}

int orientation(IWICBitmapFrameDecode* frame) {
    ComPtr<IWICMetadataQueryReader> q;
    if (FAILED(frame->GetMetadataQueryReader(&q))) return 1;
    int o = 1;
    for (const wchar_t* name : {L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}", L"System.Photo.Orientation"}) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(q->GetMetadataByName(name, &v))) {
            if (v.vt == VT_UI2) o = v.uiVal;
            PropVariantClear(&v);
            break;
        }
    }
    return o >= 1 && o <= 8 ? o : 1;
}

bool isRaw(const std::wstring& e) {
    return e == L".cr2" || e == L".cr3" || e == L".nef" || e == L".arw" || e == L".dng" || e == L".orf" ||
           e == L".rw2" || e == L".raf" || e == L".srw" || e == L".pef";
}

// decodeImage makes a thumbnail with WIC. Files on network and WSL
// paths are read in one go first: WIC's small reads cost a round trip
// each there.
bool decodeImage(IWICImagingFactory* wic, const std::wstring& path, const std::wstring& ext, Pixels& out) {
    ComPtr<IWICBitmapDecoder> dec;
    std::string bytes;
    ComPtr<IWICStream> stream;
    if (path.rfind(L"\\\\", 0) == 0) {
        if (!ReadFileBytes(path, bytes) || FAILED(wic->CreateStream(&stream)) ||
            FAILED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(bytes.data()), DWORD(bytes.size()))) ||
            FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec)))
            return false;
    } else if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) {
        return false;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return false;
    ComPtr<IWICBitmapSource> src = frame;
    int orient = orientation(frame.Get());
    if (isRaw(ext)) {
        ComPtr<IWICBitmapSource> preview;  // the camera's own JPEG, far quicker
        if (SUCCEEDED(dec->GetPreview(&preview))) src = preview;
    }
    UINT w = 0, h = 0;
    if (FAILED(src->GetSize(&w, &h)) || !w || !h) return false;

    // Decode small in the decoder itself where it can (JPEG: 1/2, 1/4,
    // 1/8), to the smallest size still at least kThumbSize.
    ComPtr<IWICBitmapSourceTransform> xf;
    if (src == frame && SUCCEEDED(frame.As(&xf))) {
        for (UINT d : {8u, 4u, 2u}) {
            UINT cw = (w + d - 1) / d, ch = (h + d - 1) / d;
            if (std::max(cw, ch) < UINT(kThumbSize)) continue;
            if (FAILED(xf->GetClosestSize(&cw, &ch)) || cw >= w || std::max(cw, ch) < UINT(kThumbSize)) continue;
            WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
            if (FAILED(xf->GetClosestPixelFormat(&fmt))) break;
            ComPtr<IWICComponentInfo> ci;
            ComPtr<IWICPixelFormatInfo> pfi;
            UINT bpp = 0;
            if (FAILED(wic->CreateComponentInfo(fmt, &ci)) || FAILED(ci.As(&pfi)) || FAILED(pfi->GetBitsPerPixel(&bpp))) break;
            UINT stride = ((cw * bpp + 31) / 32) * 4;
            std::vector<BYTE> buf(size_t(stride) * ch);
            ComPtr<IWICBitmap> bmp;
            if (SUCCEEDED(xf->CopyPixels(nullptr, cw, ch, &fmt, WICBitmapTransformRotate0, stride, UINT(buf.size()), buf.data())) &&
                SUCCEEDED(wic->CreateBitmapFromMemory(cw, ch, fmt, stride, UINT(buf.size()), buf.data(), &bmp))) {
                src = bmp;
                w = cw;
                h = ch;
            }
            break;
        }
    }

    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(src.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)))
        return false;
    ComPtr<IWICBitmapSource> cur = conv;
    if (w > UINT(kThumbSize) || h > UINT(kThumbSize)) {
        double s = std::min(double(kThumbSize) / w, double(kThumbSize) / h);
        UINT nw = std::max<UINT>(1, UINT(w * s + 0.5)), nh = std::max<UINT>(1, UINT(h * s + 0.5));
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(wic->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(conv.Get(), nw, nh, WICBitmapInterpolationModeHighQualityCubic)))
            return false;
        cur = scaler;
    }
    // Into memory, then turned the way the camera was held.
    ComPtr<IWICBitmap> shrunk;
    if (FAILED(wic->CreateBitmapFromSource(cur.Get(), WICBitmapCacheOnLoad, &shrunk))) return false;
    cur = shrunk;
    if (orient > 1) {
        static const WICBitmapTransformOptions opts[9] = {
            WICBitmapTransformRotate0, WICBitmapTransformRotate0, WICBitmapTransformFlipHorizontal,
            WICBitmapTransformRotate180, WICBitmapTransformFlipVertical,
            WICBitmapTransformOptions(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal),
            WICBitmapTransformRotate90,
            WICBitmapTransformOptions(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal),
            WICBitmapTransformRotate270,
        };
        ComPtr<IWICBitmapFlipRotator> rot;
        if (SUCCEEDED(wic->CreateBitmapFlipRotator(&rot)) && SUCCEEDED(rot->Initialize(shrunk.Get(), opts[orient]))) cur = rot;
    }
    if (FAILED(cur->GetSize(&out.width, &out.height))) return false;
    out.bgra.resize(size_t(out.width) * out.height * 4);
    return SUCCEEDED(cur->CopyPixels(nullptr, out.width * 4, UINT(out.bgra.size()), out.bgra.data()));
}

// shellImage asks the shell, as Explorer does: its thumbnail handlers
// (videos, PDFs…), or a file's own icon (programs, shortcuts).
bool shellImage(const std::wstring& path, bool iconOnly, Pixels& out) {
    ComPtr<IShellItemImageFactory> f;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&f)))) return false;
    HBITMAP bmp = nullptr;
    SIIGBF flags = SIIGBF_BIGGERSIZEOK | (iconOnly ? SIIGBF_ICONONLY : SIIGBF_THUMBNAILONLY);
    if (FAILED(f->GetImage(SIZE{kThumbSize, kThumbSize}, flags, &bmp))) return false;
    bool ok = fromBitmap(bmp, out);
    DeleteObject(bmp);
    return ok;
}

bool readCached(IWICImagingFactory* wic, const std::wstring& file, Pixels& out) {
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &frame)) || FAILED(frame->GetSize(&out.width, &out.height)) ||
        FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)))
        return false;
    out.bgra.resize(size_t(out.width) * out.height * 4);
    return SUCCEEDED(conv->CopyPixels(nullptr, out.width * 4, UINT(out.bgra.size()), out.bgra.data()));
}

// writeCached saves a thumbnail: JPEG if it's opaque, else PNG. Through a
// temporary file, so a reader never sees half of one.
bool writeCached(IWICImagingFactory* wic, const std::wstring& base, const Pixels& px) {
    bool jpeg = opaque(px);
    std::wstring path = base + (jpeg ? L".jpg" : L".png"), tmp = path + L".tmp";
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    ComPtr<IWICBitmap> bmp;
    ComPtr<IWICFormatConverter> conv;
    bool ok = SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(tmp.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(wic->CreateEncoder(jpeg ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
              SUCCEEDED(enc->CreateNewFrame(&frame, &props));
    if (ok) {
        if (jpeg) {
            PROPBAG2 opt{};
            opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_R4;
            v.fltVal = kJpegQuality;
            props->Write(1, &opt, &v);
        }
        WICPixelFormatGUID want = jpeg ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA;
        WICPixelFormatGUID fmt = want;
        ok = SUCCEEDED(frame->Initialize(props.Get())) && SUCCEEDED(frame->SetSize(px.width, px.height)) &&
             SUCCEEDED(frame->SetPixelFormat(&fmt)) &&
             SUCCEEDED(wic->CreateBitmapFromMemory(px.width, px.height, GUID_WICPixelFormat32bppBGRA, px.width * 4,
                                                   UINT(px.bgra.size()), const_cast<BYTE*>(px.bgra.data()), &bmp)) &&
             SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
             SUCCEEDED(conv->Initialize(bmp.Get(), fmt, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
             SUCCEEDED(frame->WriteSource(conv.Get(), nullptr)) && SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
    }
    frame.Reset();
    enc.Reset();
    stream.Reset();
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

void writeNone(const std::wstring& base) {
    HANDLE f = CreateFileW((base + L".none").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
}

bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// cached looks for a cached thumbnail: 1 found, 0 none, -1 known to have
// no thumbnail.
int cached(IWICImagingFactory* wic, const std::wstring& base, Pixels& px) {
    if (exists(base + L".jpg") && readCached(wic, base + L".jpg", px)) return 1;
    if (exists(base + L".png") && readCached(wic, base + L".png", px)) return 1;
    if (exists(base + L".none")) return -1;
    return 0;
}

// makeTexture uploads pixels with a chain of box-filtered mipmaps, so
// small tiles look smooth. Direct3D 11 devices make resources from any
// thread. With crop, the content box is found for icons.
bool makeTexture(ID3D11Device* device, const Pixels& px, Tex& out, bool crop) {
    std::vector<std::vector<BYTE>> levels;
    std::vector<D3D11_SUBRESOURCE_DATA> data;
    UINT w = px.width, h = px.height;
    const BYTE* src = px.bgra.data();
    data.push_back({src, w * 4, 0});
    size_t bytes = size_t(w) * h * 4;
    while (w > 1 || h > 1) {
        UINT nw = std::max<UINT>(1, w / 2), nh = std::max<UINT>(1, h / 2);
        std::vector<BYTE> next(size_t(nw) * nh * 4);
        for (UINT y = 0; y < nh; y++) {
            const BYTE* r0 = src + size_t(std::min(2 * y, h - 1)) * w * 4;
            const BYTE* r1 = src + size_t(std::min(2 * y + 1, h - 1)) * w * 4;
            BYTE* d = next.data() + size_t(y) * nw * 4;
            for (UINT x = 0; x < nw; x++) {
                UINT x0 = std::min(2 * x, w - 1) * 4, x1 = std::min(2 * x + 1, w - 1) * 4;
                // Weight colour by alpha, so transparent edges don't go dark.
                int a = r0[x0 + 3] + r0[x1 + 3] + r1[x0 + 3] + r1[x1 + 3];
                for (int c = 0; c < 3; c++) {
                    int sum = r0[x0 + c] * r0[x0 + 3] + r0[x1 + c] * r0[x1 + 3] + r1[x0 + c] * r1[x0 + 3] + r1[x1 + c] * r1[x1 + 3];
                    d[x * 4 + c] = BYTE(a ? (sum + a / 2) / a : 0);
                }
                d[x * 4 + 3] = BYTE((a + 2) / 4);
            }
        }
        levels.push_back(std::move(next));
        w = nw;
        h = nh;
        src = levels.back().data();
        data.push_back({src, w * 4, 0});
        bytes += size_t(w) * h * 4;
    }
    D3D11_TEXTURE2D_DESC d{};
    d.Width = px.width;
    d.Height = px.height;
    d.MipLevels = UINT(data.size());
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(device->CreateTexture2D(&d, data.data(), &tex))) return false;
    if (FAILED(device->CreateShaderResourceView(tex.Get(), nullptr, &out.srv))) return false;
    out.width = int(px.width);
    out.height = int(px.height);
    out.bytes = bytes;
    out.contentW = out.width;
    out.contentH = out.height;
    if (crop) {
        int x0 = out.width, y0 = out.height, x1 = -1, y1 = -1;
        for (int y = 0; y < out.height; y++)
            for (int x = 0; x < out.width; x++)
                if (px.bgra[(size_t(y) * out.width + x) * 4 + 3] > 8) {
                    x0 = std::min(x0, x);
                    y0 = std::min(y0, y);
                    x1 = std::max(x1, x);
                    y1 = std::max(y1, y);
                }
        if (x1 >= x0) {
            out.uv0 = ImVec2(float(x0) / out.width, float(y0) / out.height);
            out.uv1 = ImVec2(float(x1 + 1) / out.width, float(y1 + 1) / out.height);
            out.contentW = x1 - x0 + 1;
            out.contentH = y1 - y0 + 1;
        }
    }
    return true;
}

// pruneCache keeps the disk cache under its limit, oldest out first.
void pruneCache(const std::wstring& dir) {
    namespace fs = std::filesystem;
    struct F {
        fs::path path;
        fs::file_time_type time;
        uint64_t size;
    };
    std::vector<F> files;
    uint64_t total = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        uint64_t size = it->file_size(ec);
        files.push_back({it->path(), it->last_write_time(ec), size});
        total += size;
    }
    if (total <= kCacheMax) return;
    std::sort(files.begin(), files.end(), [](const F& a, const F& b) { return a.time < b.time; });
    for (const auto& f : files) {
        if (total <= kCacheMax * 7 / 10) break;
        if (fs::remove(f.path, ec)) total -= f.size;
    }
}

}  // namespace

struct Thumbs::Slot {
    std::wstring key, path, ext;
    bool shell = false, ownIcon = false, network = false;
    enum State { Queued, Working, Done, Failed } state = Queued;
    bool live = true;  // still in the map
    uint64_t lastWanted = 0;
    int order = 0;
    std::unique_ptr<Tex> tex;
};

Thumbs::Thumbs(ID3D11Device* device, std::function<void()> wake)
    : device_(device), wake_(std::move(wake)), cacheDir_(CacheDir() + L"\\thumbs") {
    CreateDirectoryW(cacheDir_.c_str(), nullptr);
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    device_->CreateSamplerState(&sd, &sampler_);
    // Threads at below-normal priority, so games and the like come first:
    // three for images, two for the shell's thumbnailers.
    for (int i = 0; i < 3; i++) threads_.emplace_back([this] { imageWorker(); });
    for (int i = 0; i < 2; i++) threads_.emplace_back([this] { shellWorker(); });
    std::thread([dir = cacheDir_] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
        pruneCache(dir);
    }).detach();
}

Thumbs::~Thumbs() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        quit_ = true;
    }
    cv_.notify_all();
    for (auto& t : threads_) t.join();
    if (sampler_) sampler_->Release();
}

void Thumbs::NewFrame() {
    std::lock_guard<std::mutex> lock(mu_);
    frame_++;
}

const Tex* Thumbs::Want(const std::wstring& path, const Entry& e, int order, bool& failed) {
    failed = false;
    ThumbKind kind = KindFor(e.ext);
    if (e.dir || kind == ThumbKind::Icon) {
        failed = true;
        return nullptr;
    }
    wchar_t tail[64];
    swprintf(tail, 64, L"|%llu|%llu", (unsigned long long)e.size, (unsigned long long)e.modified);
    std::wstring key = Lower(path) + tail;
    std::lock_guard<std::mutex> lock(mu_);
    auto it = slots_.find(key);
    if (it != slots_.end()) {
        Slot& s = *it->second;
        s.lastWanted = frame_;
        s.order = order;
        if (s.state == Slot::Done) return s.tex.get();
        if (s.state == Slot::Failed) failed = true;
        return nullptr;
    }
    auto s = std::make_shared<Slot>();
    s->key = key;
    s->path = path;
    s->ext = e.ext;
    s->shell = kind == ThumbKind::Shell;
    s->ownIcon = OwnIcon(e.ext);
    s->network = path.rfind(L"\\\\", 0) == 0;
    s->lastWanted = frame_;
    s->order = order;
    slots_[key] = s;
    (s->shell ? shellQueue_ : queue_).push_back(s);
    stats_.queued++;
    cv_.notify_all();
    return nullptr;
}

std::shared_ptr<Thumbs::Slot> Thumbs::next(bool shell) {
    std::unique_lock<std::mutex> lock(mu_);
    auto& q = shell ? shellQueue_ : queue_;
    for (;;) {
        if (quit_) return nullptr;
        // Drop what's no longer on screen, then take the newest wanted,
        // top of the screen first.
        size_t best = SIZE_MAX;
        for (size_t i = 0; i < q.size();) {
            auto& s = q[i];
            if (s->lastWanted + 2 < frame_) {
                s->live = false;
                slots_.erase(s->key);
                q[i] = q.back();
                q.pop_back();
                stats_.queued--;
                continue;
            }
            if (best == SIZE_MAX || s->lastWanted > q[best]->lastWanted ||
                (s->lastWanted == q[best]->lastWanted && s->order < q[best]->order))
                best = i;
            i++;
        }
        if (best != SIZE_MAX) {
            auto s = q[best];
            q[best] = q.back();
            q.pop_back();
            s->state = Slot::Working;
            stats_.queued--;
            stats_.working++;
            return s;
        }
        cv_.wait(lock);
    }
}

void Thumbs::finish(const std::shared_ptr<Slot>& s, std::unique_ptr<Tex> tex, bool, bool fromCache, double ms) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stats_.working--;
        if (tex) {
            if (fromCache) {
                stats_.fromCache++;
                stats_.cacheMs += ms;
            } else {
                stats_.made++;
                stats_.makeMs += ms;
            }
        } else {
            stats_.failed++;
        }
        if (!s->live) return;
        s->state = tex ? Slot::Done : Slot::Failed;
        if (tex) bytes_ += tex->bytes;
        s->tex = std::move(tex);
    }
    if (wake_) wake_();
}

void Thumbs::imageWorker() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> wic;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    while (auto s = next(false)) {
        auto t0 = Clock::now();
        wchar_t name[32];
        uint64_t h = fnv1a(s->key);
        swprintf(name, 32, L"\\%02x\\%016llx", unsigned(h >> 56), (unsigned long long)h);
        std::wstring base = cacheDir_ + name;
        Pixels px;
        int c = wic ? cached(wic.Get(), base, px) : 0;
        bool fromCache = c == 1;
        bool ok = fromCache;
        if (c == 0 && wic && decodeImage(wic.Get(), s->path, s->ext, px)) {
            ok = true;
            CreateDirectoryW(base.substr(0, base.size() - 17).c_str(), nullptr);
            writeCached(wic.Get(), base, px);
        }
        if (!ok && c == 0) {
            // WIC has no codec for it: try the shell's thumbnailer.
            std::lock_guard<std::mutex> lock(mu_);
            stats_.working--;
            stats_.queued++;
            s->state = Slot::Queued;
            s->shell = true;
            shellQueue_.push_back(s);
            cv_.notify_all();
            continue;
        }
        std::unique_ptr<Tex> tex;
        if (ok) {
            tex = std::make_unique<Tex>();
            if (!makeTexture(device_, px, *tex, false)) tex.reset();
        }
        finish(s, std::move(tex), false, fromCache, msSince(t0));
    }
    wic.Reset();
    CoUninitialize();
}

void Thumbs::shellWorker() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // the shell's thumbnailers want an STA
    ComPtr<IWICImagingFactory> wic;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    while (auto s = next(true)) {
        auto t0 = Clock::now();
        wchar_t name[32];
        uint64_t h = fnv1a(s->key);
        swprintf(name, 32, L"\\%02x\\%016llx", unsigned(h >> 56), (unsigned long long)h);
        std::wstring base = cacheDir_ + name;
        Pixels px;
        // Programs' and shortcuts' icons are quick and change with the
        // file they point at, so they skip the cache.
        int c = (!s->ownIcon && wic) ? cached(wic.Get(), base, px) : 0;
        bool fromCache = c == 1, ok = fromCache;
        if (c == 0) {
            ok = shellImage(s->path, s->ownIcon, px) && wic && fit(wic.Get(), px, kThumbSize);
            if (!s->ownIcon) {
                CreateDirectoryW(base.substr(0, base.size() - 17).c_str(), nullptr);
                if (ok) writeCached(wic.Get(), base, px);
                else writeNone(base);
            }
        }
        std::unique_ptr<Tex> tex;
        if (ok) {
            tex = std::make_unique<Tex>();
            tex->icon = s->ownIcon;
            if (!makeTexture(device_, px, *tex, s->ownIcon)) tex.reset();
        }
        finish(s, std::move(tex), true, fromCache, msSince(t0));
    }
    wic.Reset();
    CoUninitialize();
}

void Thumbs::Pump() {
    std::lock_guard<std::mutex> lock(mu_);
    if (bytes_ > kBudget) {
        std::vector<std::shared_ptr<Slot>> done;
        for (auto& [k, s] : slots_)
            if (s->state == Slot::Done && s->lastWanted + 1 < frame_) done.push_back(s);
        std::sort(done.begin(), done.end(), [](auto& a, auto& b) { return a->lastWanted < b->lastWanted; });
        for (auto& s : done) {
            if (bytes_ <= kBudget * 3 / 4) break;
            bytes_ -= s->tex->bytes;
            s->live = false;
            slots_.erase(s->key);
        }
    }
    // Failed ones are small, but don't let them pile up for ever.
    if (slots_.size() > 50000) {
        for (auto it = slots_.begin(); it != slots_.end();) {
            if (it->second->state == Slot::Failed && it->second->lastWanted + 1 < frame_) it = slots_.erase(it);
            else ++it;
        }
    }
}

void Thumbs::Drop() {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto* q : {&queue_, &shellQueue_}) {
        for (auto& s : *q) {
            s->live = false;
            slots_.erase(s->key);
        }
        stats_.queued -= int(q->size());
        q->clear();
    }
}

ThumbStats Thumbs::Stats() {
    std::lock_guard<std::mutex> lock(mu_);
    return stats_;
}

namespace {
ID3D11SamplerState* mipSampler = nullptr;
}

void Thumbs::MipSampler(ImDrawList* dl) {
    mipSampler = sampler_;
    dl->AddCallback(
        [](const ImDrawList*, const ImDrawCmd*) {
            auto* rs = static_cast<ImGui_ImplDX11_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
            if (rs && mipSampler) rs->DeviceContext->PSSetSamplers(0, 1, &mipSampler);
        },
        nullptr);
}

std::string Thumbs::Bench(ID3D11Device* device, const std::wstring& dir, bool shellToo) {
    std::vector<Entry> entries;
    DWORD err = 0;
    if (!ListDir(dir, entries, err)) return "can't list: " + ErrorText(err) + "\n";
    std::vector<int> files;
    for (int i = 0; i < int(entries.size()); i++)
        if (!entries[i].dir && KindFor(entries[i].ext) != ThumbKind::Icon) files.push_back(i);
    std::string report;
    char line[256];
    snprintf(line, sizeof line, "%zu files with thumbnails in %s\n", files.size(), Narrow(dir).c_str());
    report += line;

    auto run = [&](const char* label) {
        Thumbs t(device, nullptr);
        auto t0 = Clock::now();
        for (;;) {
            t.NewFrame();
            size_t settled = 0;
            for (size_t k = 0; k < files.size(); k++) {
                bool failed = false;
                const Entry& e = entries[files[k]];
                if (t.Want(Join(dir, e.name), e, int(k), failed) || failed) settled++;
            }
            if (settled == files.size()) break;
            Sleep(5);
        }
        double ms = msSince(t0);
        ThumbStats s = t.Stats();
        snprintf(line, sizeof line,
                 "%s: %.0f ms all, %d made (%.1f ms each on a thread), %d from cache (%.1f ms each), %d none\n", label, ms,
                 s.made, s.made ? s.makeMs / s.made : 0.0, s.fromCache, s.fromCache ? s.cacheMs / s.fromCache : 0.0, s.failed);
        report += line;
    };
    run("shelf, first time");
    run("shelf, again (disk cache)");

    if (shellToo) {
        // Explorer's way: the shell's image factory, from Windows'
        // thumbnail cache when it has them, on as many threads.
        std::atomic<size_t> nextFile{0};
        std::atomic<int> got{0};
        auto t0 = Clock::now();
        std::vector<std::thread> ts;
        for (int i = 0; i < 5; i++)
            ts.emplace_back([&] {
                CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                for (size_t k; (k = nextFile++) < files.size();) {
                    Pixels px;
                    if (shellImage(Join(dir, entries[files[k]].name), false, px)) got++;
                }
                CoUninitialize();
            });
        for (auto& t : ts) t.join();
        snprintf(line, sizeof line, "shell (Explorer's thumbnail route), 5 threads: %.0f ms, %d made\n", msSince(t0), got.load());
        report += line;
    }
    return report;
}

const Tex* Icons::byIndex(int index, bool big) {
    auto& map = big ? big_ : small_;
    auto it = map.find(index);
    if (it != map.end()) return it->second.get();
    auto& slot = map[index];
    ComPtr<IImageList> list;
    if (FAILED(SHGetImageList(big ? SHIL_JUMBO : SHIL_EXTRALARGE, IID_PPV_ARGS(&list)))) return nullptr;
    HICON icon = nullptr;
    if (FAILED(list->GetIcon(index, ILD_TRANSPARENT, &icon)) || !icon) return nullptr;
    Pixels px;
    bool ok = fromIcon(icon, px);
    DestroyIcon(icon);
    if (!ok) return nullptr;
    auto tex = std::make_unique<Tex>();
    tex->icon = true;
    if (!makeTexture(device_, px, *tex, true)) return nullptr;
    slot = std::move(tex);
    return slot.get();
}

const Tex* Icons::For(const Entry& e, bool big) {
    std::wstring key = e.dir ? L"<dir>" : e.ext.empty() ? L"<none>" : e.ext;
    auto it = indexOf_.find(key);
    if (it == indexOf_.end()) {
        SHFILEINFOW sfi{};
        std::wstring fake = e.dir ? L"folder" : L"file" + e.ext;
        DWORD attrs = e.dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        int index = -1;
        if (SHGetFileInfoW(fake.c_str(), attrs, &sfi, sizeof sfi, SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX)) index = sfi.iIcon;
        it = indexOf_.emplace(key, index).first;
    }
    return it->second < 0 ? nullptr : byIndex(it->second, big);
}

const Tex* Icons::ForPath(const std::wstring& path, bool big) {
    auto it = pathIndex_.find(path);
    if (it == pathIndex_.end()) {
        SHFILEINFOW sfi{};
        int index = -1;
        if (SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof sfi, SHGFI_SYSICONINDEX)) index = sfi.iIcon;
        it = pathIndex_.emplace(path, index).first;
    }
    return it->second < 0 ? nullptr : byIndex(it->second, big);
}

const std::wstring& Icons::TypeName(const Entry& e) {
    std::wstring key = e.dir ? L"<dir>" : e.ext.empty() ? L"<none>" : e.ext;
    auto it = types_.find(key);
    if (it != types_.end()) return it->second;
    SHFILEINFOW sfi{};
    std::wstring fake = e.dir ? L"folder" : L"file" + e.ext;
    DWORD attrs = e.dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    std::wstring name;
    if (SHGetFileInfoW(fake.c_str(), attrs, &sfi, sizeof sfi, SHGFI_USEFILEATTRIBUTES | SHGFI_TYPENAME)) name = sfi.szTypeName;
    if (name.empty()) name = e.dir ? L"File folder" : L"File";
    return types_.emplace(key, name).first->second;
}

}  // namespace shelf
