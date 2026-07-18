// Standalone validator for the SWF image modules. Compiles the real parsers
// (SWFParseCommon + SWFLibraryImage + SWFVectorMovie) so both the bitmap
// extraction path and the vector/timeline rasterizer can be exercised against
// real mod SWFs on disk, outside the game. Not shipped.
//
// Usage: swftest <lib.swf> <className> [outPrefix] [ref.png]
//   Writes <outPrefix>_bitmap.png (SWFLibraryImage path, if it succeeds) and
//   <outPrefix>_vector.png (SWFVectorMovie static render, if it succeeds),
//   and prints what each path produced. If ref.png is given, the bitmap
//   path's output is diffed against it.

#include "MCM/SWFLibraryImage.h"
#include "MCM/SWFVectorMovie.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <wincodec.h>
#include <wrl/client.h>

// ---------------------------------------------------------------------------
// WIC helpers: PNG writer + PNG loader (for reference comparison).
// ---------------------------------------------------------------------------
static bool WritePNG(const std::wstring& a_path, int a_w, int a_h, const std::uint8_t* a_rgba) {
    using Microsoft::WRL::ComPtr;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
        return false;
    }
    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromFilename(a_path.c_str(), GENERIC_WRITE))) {
        return false;
    }
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) {
        return false;
    }
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr))) {
        return false;
    }
    frame->SetSize(a_w, a_h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppRGBA;
    frame->SetPixelFormat(&fmt);
    if (FAILED(frame->WritePixels(a_h, a_w * 4, a_w * a_h * 4, const_cast<BYTE*>(a_rgba)))) {
        return false;
    }
    return SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

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

static std::wstring Widen(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}

// ---------------------------------------------------------------------------
// Synthetic animated SWF generator ("--selftest"). The mod corpus contains no
// timeline-animated logos, so the animation path is validated with a
// hand-crafted movie: a red square shape tweened rightward over 10 frames,
// plus a green square placed statically at depth 2 with 50% alpha cxform.
// ---------------------------------------------------------------------------
namespace {

    class BitWriter {
    public:
        explicit BitWriter(std::vector<std::uint8_t>& out) : _out(out) {}
        void U8(std::uint8_t v) { Align(); _out.push_back(v); }
        void U16(std::uint16_t v) { U8(v & 0xFF); U8(v >> 8); }
        void U32(std::uint32_t v) { U16(v & 0xFFFF); U16(static_cast<std::uint16_t>(v >> 16)); }
        void Bits(std::uint32_t v, int n) {
            for (int i = n - 1; i >= 0; --i) {
                _cur = static_cast<std::uint8_t>((_cur << 1) | ((v >> i) & 1));
                if (++_fill == 8) { _out.push_back(_cur); _cur = 0; _fill = 0; }
            }
        }
        void SBits(std::int32_t v, int n) { Bits(static_cast<std::uint32_t>(v) & ((n < 32 ? (1u << n) : 0) - 1u), n); }
        void Align() {
            if (_fill > 0) { _out.push_back(static_cast<std::uint8_t>(_cur << (8 - _fill))); _cur = 0; _fill = 0; }
        }
    private:
        std::vector<std::uint8_t>& _out;
        std::uint8_t _cur = 0;
        int _fill = 0;
    };

    void WriteTag(std::vector<std::uint8_t>& out, std::uint16_t code, const std::vector<std::uint8_t>& body) {
        BitWriter w(out);
        if (body.size() < 0x3F) {
            w.U16(static_cast<std::uint16_t>((code << 6) | body.size()));
        } else {
            w.U16(static_cast<std::uint16_t>((code << 6) | 0x3F));
            w.U32(static_cast<std::uint32_t>(body.size()));
        }
        out.insert(out.end(), body.begin(), body.end());
    }

    // A solid-color 100x100 px (2000x2000 twip) square shape.
    std::vector<std::uint8_t> MakeSquareShape(std::uint16_t shapeId, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        std::vector<std::uint8_t> body;
        BitWriter w(body);
        w.U16(shapeId);
        // Bounds RECT: nbits=12, 0..2000 twips both axes
        w.Bits(12, 5); w.SBits(0, 12); w.SBits(2000, 12); w.SBits(0, 12); w.SBits(2000, 12);
        w.Align();
        w.U8(1);            // fill style count
        w.U8(0x00);         // solid
        w.U8(r); w.U8(g); w.U8(b);  // RGB (DefineShape v1)
        w.U8(0);            // line style count
        w.Bits(1, 4);       // numFillBits
        w.Bits(0, 4);       // numLineBits
        // StyleChange: MoveTo(0,0) + FillStyle1 = 1
        w.Bits(0, 1);       // non-edge
        w.Bits(0x05, 5);    // MoveTo | FillStyle1
        w.Bits(1, 5);       // move nbits
        w.SBits(0, 1); w.SBits(0, 1);
        w.Bits(1, 1);       // fill style 1 index (1 bit)
        // Four straight edges around the square
        auto edge = [&](std::int32_t dx, std::int32_t dy) {
            w.Bits(1, 1);   // edge
            w.Bits(1, 1);   // straight
            w.Bits(11, 4);  // nbits-2 = 11 -> 13-bit deltas
            w.Bits(1, 1);   // general line
            w.SBits(dx, 13); w.SBits(dy, 13);
        };
        edge(2000, 0); edge(0, 2000); edge(-2000, 0); edge(0, -2000);
        // End record
        w.Bits(0, 6);
        w.Align();
        return body;
    }

    std::vector<std::uint8_t> MakePlace2(std::uint8_t flags, std::uint16_t depth, std::uint16_t charId,
                                         bool hasChar, bool hasMatrix, std::int32_t tx, std::int32_t ty,
                                         bool hasCx = false, std::uint8_t alphaMul256 = 255) {
        std::vector<std::uint8_t> body;
        BitWriter w(body);
        w.U8(flags);
        w.U16(depth);
        if (hasChar) w.U16(charId);
        if (hasMatrix) {
            w.Bits(0, 1);   // no scale
            w.Bits(0, 1);   // no rotate
            w.Bits(15, 5);  // translate bits
            w.SBits(tx, 15); w.SBits(ty, 15);
            w.Align();
        }
        if (hasCx) {  // CXFORMWITHALPHA: mult only
            w.Bits(0, 1);    // no add
            w.Bits(1, 1);    // has mult
            w.Bits(10, 4);   // nbits (256 needs 10 signed bits)
            w.SBits(256, 10); w.SBits(256, 10); w.SBits(256, 10); w.SBits(alphaMul256, 10);
            w.Align();
        }
        return body;
    }

    std::string MakeTestSWF() {
        std::vector<std::uint8_t> body;
        {
            BitWriter w(body);
            // Stage RECT 550x400 px
            w.Bits(15, 5); w.SBits(0, 15); w.SBits(11000, 15); w.SBits(0, 15); w.SBits(8000, 15);
            w.Align();
            w.U16(12 << 8);  // frame rate 12
            w.U16(10);       // frame count
        }
        WriteTag(body, 2 /*DefineShape*/, MakeSquareShape(1, 255, 0, 0));
        WriteTag(body, 2 /*DefineShape*/, MakeSquareShape(2, 0, 200, 0));
        // Frame 0: red square at depth 1 (origin), green at depth 2 (300,150) 50% alpha
        WriteTag(body, 26, MakePlace2(0x06, 1, 1, true, true, 0, 0));
        WriteTag(body, 26, MakePlace2(0x0E, 2, 2, true, true, 6000, 3000, true, 128));
        WriteTag(body, 1, {});  // ShowFrame
        // Frames 1..9: move the red square right by 400 twips per frame
        for (int f = 1; f < 10; ++f) {
            WriteTag(body, 26, MakePlace2(0x05, 1, 0, false, true, f * 400, 0));
            WriteTag(body, 1, {});
        }
        WriteTag(body, 0, {});  // End

        // Container
        std::vector<std::uint8_t> file;
        file.push_back('F'); file.push_back('W'); file.push_back('S'); file.push_back(6);
        const std::uint32_t total = static_cast<std::uint32_t>(8 + body.size());
        file.push_back(total & 0xFF); file.push_back((total >> 8) & 0xFF);
        file.push_back((total >> 16) & 0xFF); file.push_back((total >> 24) & 0xFF);
        file.insert(file.end(), body.begin(), body.end());

        const std::string path = "out\\selftest.swf";
        FILE* f = std::fopen(path.c_str(), "wb");
        if (f) {
            std::fwrite(file.data(), 1, file.size(), f);
            std::fclose(f);
        }
        return path;
    }

}  // namespace

// Writes a second PNG composited over a dark background (like the MCM panel)
// so white-on-transparent art is visible in previews.
static void WriteDarkPNG(const std::string& path, int w, int h, const std::uint8_t* rgba) {
    std::vector<std::uint8_t> dark(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i) {
        const std::uint8_t* s = rgba + i * 4;
        std::uint8_t* d = dark.data() + i * 4;
        const float a = s[3] / 255.0f;
        d[0] = static_cast<std::uint8_t>(s[0] * a + 32 * (1.0f - a));
        d[1] = static_cast<std::uint8_t>(s[1] * a + 32 * (1.0f - a));
        d[2] = static_cast<std::uint8_t>(s[2] * a + 32 * (1.0f - a));
        d[3] = 255;
    }
    WritePNG(Widen(path), w, h, dark.data());
}

// Validates the timeline/animation machinery against a generated SWF with a
// known tween. Returns 0 on pass.
static int RunSelfTest() {
    const std::string path = MakeTestSWF();
    auto movie = SWFVectorMovie::Load(path, "");
    if (!movie) {
        std::fprintf(stderr, "selftest: FAILED to load generated movie\n");
        return 1;
    }
    int failures = 0;
    auto expect = [&](bool cond, const char* what) {
        std::fprintf(stderr, "selftest: %-40s %s\n", what, cond ? "ok" : "FAIL");
        if (!cond) ++failures;
    };
    expect(movie->FrameCount() == 10, "10 root frames");
    expect(movie->IsAnimated(), "flagged animated");
    // Content bounds: red square sweeps x 0..(3600+2000) twips = 280 px wide,
    // green square sits at 300..400 px; union = 0..400 px wide, 0..250 tall.
    expect(std::abs(movie->WidthPx() - 400.0f) < 2.0f, "content width ~400 px");
    expect(std::abs(movie->HeightPx() - 250.0f) < 2.0f, "content height ~250 px");

    // Frame 0 vs frame 9: red square center should move right by 3600 twips = 180 px.
    std::vector<SWFVectorMovie::FrameDraw> d0, d9;
    movie->BuildFrameDraws(0, d0);
    movie->BuildFrameDraws(9, d9);
    expect(d0.size() == 2 && d9.size() == 2, "2 quads per frame");
    if (d0.size() == 2 && d9.size() == 2) {
        expect(std::abs((d9[0].x0 - d0[0].x0) - 180.0f) < 1.0f, "tween delta = 180 px");
        expect(std::abs(d0[1].mulA - 0.5f) < 0.02f, "green quad alpha mul ~0.5");
    }

    // Static renders for eyeballing.
    if (auto f0 = movie->RenderFrame(0, 1.0f)) {
        WriteDarkPNG("out\\selftest_f0.png", f0->width, f0->height, f0->rgba.data());
        // Probe: red square covers (0..100, 0..100) px on frame 0.
        const std::uint8_t* p = f0->rgba.data() + (50 * f0->width + 50) * 4;
        expect(p[0] > 200 && p[1] < 50 && p[3] > 200, "frame 0: red at (50,50)");
        // Green square with 0.5 alpha at (350,200)
        const std::uint8_t* q = f0->rgba.data() + (200 * f0->width + 350) * 4;
        expect(q[1] > 100 && q[3] > 100 && q[3] < 160, "frame 0: green a~128 at (350,200)");
    } else {
        expect(false, "frame 0 renders");
    }
    if (auto f9 = movie->RenderFrame(9, 1.0f)) {
        WriteDarkPNG("out\\selftest_f9.png", f9->width, f9->height, f9->rgba.data());
        const std::uint8_t* p = f9->rgba.data() + (50 * f9->width + 50) * 4;
        expect(p[3] < 50, "frame 9: (50,50) now empty");
        const std::uint8_t* p2 = f9->rgba.data() + (50 * f9->width + 230) * 4;
        expect(p2[0] > 200 && p2[3] > 200, "frame 9: red at (230,50)");
    } else {
        expect(false, "frame 9 renders");
    }
    std::fprintf(stderr, "selftest: %s\n", failures == 0 ? "ALL PASSED" : "FAILURES");
    return failures == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--selftest") == 0) {
        return RunSelfTest();
    }
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <lib.swf> <className> [outPrefix] [ref.png]\n", argv[0]);
        std::fprintf(stderr, "       %s --selftest\n", argv[0]);
        return 2;
    }
    const std::string swf = argv[1];
    const std::string cls = argv[2];
    const std::string prefix = argc >= 4 ? argv[3] : "out";

    int failures = 0;

    // ---- Path 1: embedded bitmap extraction (SWFLibraryImage) ----
    auto img = SWFLibraryImage::Extract(swf, cls);
    if (img) {
        std::fprintf(stderr, "bitmap : OK %dx%d\n", img->width, img->height);
        const std::string out = prefix + "_bitmap.png";
        if (WritePNG(Widen(out), img->width, img->height, img->rgba.data())) {
            std::fprintf(stderr, "bitmap : wrote %s\n", out.c_str());
        } else {
            std::fprintf(stderr, "bitmap : FAILED to write %s\n", out.c_str());
            ++failures;
        }
    } else {
        std::fprintf(stderr, "bitmap : no embedded bitmap\n");
    }

    // ---- Path 2: vector/timeline rasterizer (SWFVectorMovie) ----
    auto movie = SWFVectorMovie::Load(swf, cls);
    if (movie) {
        std::fprintf(stderr, "vector : OK %.0fx%.0f px, %d frame(s) @ %.1f fps, %s\n",
                     movie->WidthPx(), movie->HeightPx(), movie->FrameCount(), movie->FrameRate(),
                     movie->IsAnimated() ? "ANIMATED" : "static");
        // Render at a scale that puts the long side near 512 px.
        const float longSide = movie->WidthPx() > movie->HeightPx() ? movie->WidthPx() : movie->HeightPx();
        const float scale = longSide > 0.0f ? (512.0f / longSide > 4.0f ? 4.0f : 512.0f / longSide) : 1.0f;
        auto frame0 = movie->RenderFrame(0, scale > 0.05f ? scale : 1.0f);
        if (frame0) {
            const std::string out = prefix + "_vector.png";
            WriteDarkPNG(prefix + "_vector_dark.png", frame0->width, frame0->height, frame0->rgba.data());
            if (WritePNG(Widen(out), frame0->width, frame0->height, frame0->rgba.data())) {
                std::fprintf(stderr, "vector : wrote %s (%dx%d)\n", out.c_str(), frame0->width, frame0->height);
            } else {
                std::fprintf(stderr, "vector : FAILED to write %s\n", out.c_str());
                ++failures;
            }
        } else {
            std::fprintf(stderr, "vector : frame 0 rendered empty\n");
        }
        // For animated movies also dump a mid-animation frame + quad stats.
        if (movie->IsAnimated()) {
            const int mid = movie->FrameCount() / 2;
            if (auto fmid = movie->RenderFrame(mid, scale > 0.05f ? scale : 1.0f)) {
                const std::string out = prefix + "_vector_mid.png";
                WritePNG(Widen(out), fmid->width, fmid->height, fmid->rgba.data());
                std::fprintf(stderr, "vector : wrote %s (frame %d)\n", out.c_str(), mid);
            }
            std::vector<SWFVectorMovie::FrameDraw> draws;
            movie->BuildFrameDraws(0, draws);
            std::fprintf(stderr, "vector : %zu raster chars, %zu quads on frame 0\n",
                         movie->Rasters().size(), draws.size());
        }
    } else {
        std::fprintf(stderr, "vector : not renderable (bitmap-only class, AS3-only, or parse failure)\n");
    }

    if (!img && !movie) {
        std::fprintf(stderr, "RESULT : nothing extractable\n");
        return 1;
    }

    // ---- Optional diff of the bitmap path against a reference PNG ----
    if (argc >= 5 && img) {
        int rw = 0, rh = 0;
        std::vector<std::uint8_t> ref;
        if (!LoadRGBA(Widen(argv[4]), rw, rh, ref)) {
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
    return failures == 0 ? 0 : 1;
}
