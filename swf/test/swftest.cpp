// Standalone validator for SWFLibraryImage. Compiles the real parser plus a
// tiny TGA writer so we can extract a class's bitmap from a lib.swf on disk and
// eyeball / dimension-check it against JPEXS exports. Not shipped.
//
// Usage: swftest <lib.swf> <className> [out.tga]

#include "MCM/SWFLibraryImage.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

static bool WriteTGA(const std::string& path, const SWFLibraryImage::DecodedImage& img) {
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    std::uint8_t header[18] = {};
    header[2] = 2;  // uncompressed true-color
    header[12] = static_cast<std::uint8_t>(img.width & 0xFF);
    header[13] = static_cast<std::uint8_t>((img.width >> 8) & 0xFF);
    header[14] = static_cast<std::uint8_t>(img.height & 0xFF);
    header[15] = static_cast<std::uint8_t>((img.height >> 8) & 0xFF);
    header[16] = 32;    // bpp
    header[17] = 0x28;  // top-down, 8-bit alpha
    f.write(reinterpret_cast<char*>(header), sizeof(header));
    // TGA is BGRA.
    std::vector<std::uint8_t> row(static_cast<std::size_t>(img.width) * 4);
    for (int y = 0; y < img.height; ++y) {
        const std::uint8_t* src = img.rgba.data() + static_cast<std::size_t>(y) * img.width * 4;
        for (int x = 0; x < img.width; ++x) {
            row[x * 4 + 0] = src[x * 4 + 2];  // B
            row[x * 4 + 1] = src[x * 4 + 1];  // G
            row[x * 4 + 2] = src[x * 4 + 0];  // R
            row[x * 4 + 3] = src[x * 4 + 3];  // A
        }
        f.write(reinterpret_cast<char*>(row.data()), row.size());
    }
    return true;
}

#include <wincodec.h>
#include <wrl/client.h>

// Decode any WIC-readable image (PNG here) to straight RGBA for comparison.
static bool LoadRGBA(const std::wstring& path, int& w, int& h, std::vector<std::uint8_t>& rgba) {
    using Microsoft::WRL::ComPtr;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
        return false;
    }
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) {
        return false;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) {
        return false;
    }
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(factory->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        return false;
    }
    UINT uw = 0, uh = 0;
    conv->GetSize(&uw, &uh);
    w = static_cast<int>(uw);
    h = static_cast<int>(uh);
    rgba.assign(static_cast<std::size_t>(w) * h * 4, 0);
    return SUCCEEDED(conv->CopyPixels(nullptr, uw * 4, static_cast<UINT>(rgba.size()), rgba.data()));
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <lib.swf> <className> [out.tga] [ref.png]\n", argv[0]);
        return 2;
    }
    const std::string swf = argv[1];
    const std::string cls = argv[2];
    const std::string out = argc >= 4 ? argv[3] : "out.tga";

    auto img = SWFLibraryImage::Extract(swf, cls);
    if (!img) {
        std::fprintf(stderr, "FAILED to extract '%s' from %s\n", cls.c_str(), swf.c_str());
        return 1;
    }
    std::fprintf(stderr, "OK: %dx%d\n", img->width, img->height);
    if (!WriteTGA(out, *img)) {
        std::fprintf(stderr, "failed to write %s\n", out.c_str());
        return 1;
    }
    std::fprintf(stderr, "wrote %s\n", out.c_str());

    if (argc >= 5) {
        std::wstring refPath(argv[4], argv[4] + std::strlen(argv[4]));
        int rw = 0, rh = 0;
        std::vector<std::uint8_t> ref;
        if (!LoadRGBA(refPath, rw, rh, ref)) {
            std::fprintf(stderr, "compare: failed to load ref %s\n", argv[4]);
            return 1;
        }
        if (rw != img->width || rh != img->height) {
            std::fprintf(stderr, "compare: SIZE MISMATCH mine=%dx%d ref=%dx%d\n", img->width, img->height, rw, rh);
            return 1;
        }
        double sum = 0;
        int maxd = 0;
        std::size_t n = ref.size();
        for (std::size_t i = 0; i < n; ++i) {
            int d = std::abs(static_cast<int>(img->rgba[i]) - static_cast<int>(ref[i]));
            sum += d;
            if (d > maxd) {
                maxd = d;
            }
        }
        std::fprintf(stderr, "compare: avg channel diff=%.3f  max diff=%d  (%zu channels)\n", sum / n, maxd, n);
    }
    return 0;
}
