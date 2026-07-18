#include "MCM/SWFParseCommon.h"

#include <cstring>
#include <fstream>

#include <zlib.h>

namespace SWFParse {

    // -----------------------------------------------------------------------
    // zlib inflate that grows its output buffer as needed.
    // -----------------------------------------------------------------------
    bool Inflate(const std::uint8_t* a_src, std::size_t a_srcLen, std::vector<std::uint8_t>& a_out, std::size_t a_hint) {
        z_stream strm{};
        if (inflateInit(&strm) != Z_OK) {
            return false;
        }
        strm.next_in = const_cast<Bytef*>(a_src);
        strm.avail_in = static_cast<uInt>(a_srcLen);

        a_out.clear();
        a_out.resize(a_hint ? a_hint : 65536);

        std::size_t total = 0;
        int ret = Z_OK;
        do {
            if (total == a_out.size()) {
                a_out.resize(a_out.size() * 2);
            }
            strm.next_out = a_out.data() + total;
            strm.avail_out = static_cast<uInt>(a_out.size() - total);
            ret = inflate(&strm, Z_NO_FLUSH);
            total = a_out.size() - strm.avail_out;
            if (ret == Z_STREAM_END) {
                break;
            }
            if (ret != Z_OK) {
                inflateEnd(&strm);
                return false;
            }
        } while (strm.avail_in > 0 || strm.avail_out == 0);

        inflateEnd(&strm);
        a_out.resize(total);
        return true;
    }

    // -----------------------------------------------------------------------
    // Container: read file, validate signature, decompress if CWS.
    // -----------------------------------------------------------------------
    bool ReadMovieBody(const std::string& a_path, std::vector<std::uint8_t>& a_body) {
        std::ifstream file(a_path, std::ios::binary);
        if (!file) {
            return false;
        }
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (bytes.size() < 8) {
            return false;
        }
        const char sig0 = static_cast<char>(bytes[0]);
        if (bytes[1] != 'W' || bytes[2] != 'S') {
            return false;
        }
        const std::uint32_t fileLength =
            bytes[4] | (bytes[5] << 8) | (bytes[6] << 16) | (static_cast<std::uint32_t>(bytes[7]) << 24);

        if (sig0 == 'F') {  // uncompressed
            a_body.assign(bytes.begin() + 8, bytes.end());
            return true;
        }
        if (sig0 == 'C') {  // zlib
            const std::size_t hint = fileLength > 8 ? fileLength - 8 : bytes.size() * 4;
            return Inflate(bytes.data() + 8, bytes.size() - 8, a_body, hint);
        }
        return false;  // ZWS (LZMA) unsupported
    }

    // -----------------------------------------------------------------------
    // Pixel helpers.
    // -----------------------------------------------------------------------
    namespace {

        // Un-premultiply a single ARGB sample into straight RGBA in `dst`.
        inline void StoreUnpremul(std::uint8_t* a_dst, std::uint8_t a_a, std::uint8_t a_r, std::uint8_t a_g, std::uint8_t a_b) {
            if (a_a == 0) {
                a_dst[0] = a_dst[1] = a_dst[2] = 0;
                a_dst[3] = 0;
                return;
            }
            if (a_a == 255) {
                a_dst[0] = a_r;
                a_dst[1] = a_g;
                a_dst[2] = a_b;
                a_dst[3] = 255;
                return;
            }
            auto up = [a_a](std::uint8_t c) -> std::uint8_t {
                int v = (static_cast<int>(c) * 255 + a_a / 2) / a_a;
                return static_cast<std::uint8_t>(v > 255 ? 255 : v);
            };
            a_dst[0] = up(a_r);
            a_dst[1] = up(a_g);
            a_dst[2] = up(a_b);
            a_dst[3] = a_a;
        }

        inline std::uint8_t Expand5(std::uint32_t a_c5) {
            std::uint8_t c = static_cast<std::uint8_t>(a_c5 & 0x1F);
            return static_cast<std::uint8_t>((c << 3) | (c >> 2));
        }

    }  // namespace

    // -----------------------------------------------------------------------
    // DefineBitsLossless / DefineBitsLossless2 -> RGBA.
    // -----------------------------------------------------------------------
    bool DecodeLossless(const BitmapRecord& a_rec, const std::uint8_t* a_body, Image& a_out) {
        const bool hasAlpha = (a_rec.tagCode == kTagDefineBitsLossless2);

        std::vector<std::uint8_t> raw;
        const std::size_t hint = static_cast<std::size_t>(a_rec.width) * a_rec.height * 4 + 4096;
        if (!Inflate(a_body + a_rec.dataOffset, a_rec.dataLen, raw, hint)) {
            return false;
        }

        a_out.width = a_rec.width;
        a_out.height = a_rec.height;
        a_out.rgba.assign(static_cast<std::size_t>(a_rec.width) * a_rec.height * 4, 0);

        if (a_rec.format == 5) {
            // 32-bit: Lossless2 = ARGB (premultiplied); Lossless = (pad)RGB.
            const std::size_t need = static_cast<std::size_t>(a_rec.width) * a_rec.height * 4;
            if (raw.size() < need) {
                return false;
            }
            for (std::size_t i = 0; i < static_cast<std::size_t>(a_rec.width) * a_rec.height; ++i) {
                const std::uint8_t* p = raw.data() + i * 4;
                std::uint8_t* d = a_out.rgba.data() + i * 4;
                if (hasAlpha) {
                    StoreUnpremul(d, p[0], p[1], p[2], p[3]);  // A,R,G,B
                } else {
                    d[0] = p[1];
                    d[1] = p[2];
                    d[2] = p[3];
                    d[3] = 255;  // p[0] reserved
                }
            }
            return true;
        }

        if (a_rec.format == 4) {
            // 15-bit: 2 bytes/pixel, rows padded to 32-bit boundary.
            const std::size_t stride = ((static_cast<std::size_t>(a_rec.width) * 2 + 3) & ~std::size_t{3});
            if (raw.size() < stride * a_rec.height) {
                return false;
            }
            for (int y = 0; y < a_rec.height; ++y) {
                for (int x = 0; x < a_rec.width; ++x) {
                    const std::uint8_t* p = raw.data() + y * stride + x * 2;
                    std::uint16_t px = static_cast<std::uint16_t>(p[0] | (p[1] << 8));
                    std::uint8_t* d = a_out.rgba.data() + (static_cast<std::size_t>(y) * a_rec.width + x) * 4;
                    d[0] = Expand5(px >> 10);
                    d[1] = Expand5(px >> 5);
                    d[2] = Expand5(px);
                    d[3] = 255;
                }
            }
            return true;
        }

        if (a_rec.format == 3) {
            // 8-bit colormapped. Palette: RGBA (Lossless2, premultiplied) or
            // RGB (Lossless). Index rows padded to 32-bit boundary.
            const int entryBytes = hasAlpha ? 4 : 3;
            const std::size_t paletteBytes = static_cast<std::size_t>(a_rec.colorTableCount) * entryBytes;
            const std::size_t stride = ((static_cast<std::size_t>(a_rec.width) + 3) & ~std::size_t{3});
            if (raw.size() < paletteBytes + stride * a_rec.height) {
                return false;
            }
            std::vector<std::uint8_t> pal(static_cast<std::size_t>(a_rec.colorTableCount) * 4, 0);
            for (int i = 0; i < a_rec.colorTableCount; ++i) {
                const std::uint8_t* s = raw.data() + static_cast<std::size_t>(i) * entryBytes;
                if (hasAlpha) {
                    StoreUnpremul(pal.data() + i * 4, s[3], s[0], s[1], s[2]);  // stored RGBA premul
                } else {
                    pal[i * 4 + 0] = s[0];
                    pal[i * 4 + 1] = s[1];
                    pal[i * 4 + 2] = s[2];
                    pal[i * 4 + 3] = 255;
                }
            }
            const std::uint8_t* idx = raw.data() + paletteBytes;
            for (int y = 0; y < a_rec.height; ++y) {
                for (int x = 0; x < a_rec.width; ++x) {
                    std::uint8_t ci = idx[y * stride + x];
                    if (ci >= a_rec.colorTableCount) {
                        ci = 0;
                    }
                    std::memcpy(a_out.rgba.data() + (static_cast<std::size_t>(y) * a_rec.width + x) * 4,
                                pal.data() + static_cast<std::size_t>(ci) * 4, 4);
                }
            }
            return true;
        }

        return false;
    }

    // -----------------------------------------------------------------------
    // Any BitmapRecord -> RGBA (lossless or JPEG; JPEG3/4 alpha plane applied).
    // -----------------------------------------------------------------------
    bool DecodeBitmap(const BitmapRecord& a_rec, const std::uint8_t* a_body, Image& a_out) {
        if (a_rec.tagCode == kTagDefineBitsLossless || a_rec.tagCode == kTagDefineBitsLossless2) {
            return DecodeLossless(a_rec, a_body, a_out);
        }

        // JPEG2/3/4: jpegAlphaOffset splits the payload into jpeg | zlib(alpha).
        const std::size_t jpegLen = a_rec.jpegAlphaOffset ? a_rec.jpegAlphaOffset : a_rec.dataLen;
        if (!DecodeJPEG(a_body + a_rec.dataOffset, jpegLen, a_out)) {
            return false;
        }

        // Optional alpha plane (DefineBitsJPEG3/4): zlib-compressed, one byte
        // per pixel, same dimensions as the decoded JPEG.
        if (a_rec.jpegAlphaOffset && a_rec.dataLen > a_rec.jpegAlphaOffset) {
            const std::size_t alphaLen = a_rec.dataLen - a_rec.jpegAlphaOffset;
            std::vector<std::uint8_t> alpha;
            const std::size_t pixels = static_cast<std::size_t>(a_out.width) * a_out.height;
            if (Inflate(a_body + a_rec.dataOffset + a_rec.jpegAlphaOffset, alphaLen, alpha, pixels) &&
                alpha.size() >= pixels) {
                for (std::size_t i = 0; i < pixels; ++i) {
                    a_out.rgba[i * 4 + 3] = alpha[i];
                }
            }
        }
        return true;
    }

}  // namespace SWFParse

// ---------------------------------------------------------------------------
// WIC-based JPEG decoder. Isolated at the bottom so the rest of the module has
// no Windows dependency.
// ---------------------------------------------------------------------------
#include <wincodec.h>
#include <wrl/client.h>

namespace SWFParse {

    bool DecodeJPEG(const std::uint8_t* a_jpeg, std::size_t a_len, Image& a_out) {
        if (!a_jpeg || a_len == 0) {
            return false;
        }
        using Microsoft::WRL::ComPtr;

        HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool didInit = SUCCEEDED(coInit);

        bool ok = false;
        {
            ComPtr<IWICImagingFactory> factory;
            if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
                ComPtr<IWICStream> stream;
                if (SUCCEEDED(factory->CreateStream(&stream)) &&
                    SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(a_jpeg), static_cast<DWORD>(a_len)))) {
                    ComPtr<IWICBitmapDecoder> decoder;
                    if (SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder))) {
                        ComPtr<IWICBitmapFrameDecode> frame;
                        if (SUCCEEDED(decoder->GetFrame(0, &frame))) {
                            ComPtr<IWICFormatConverter> conv;
                            if (SUCCEEDED(factory->CreateFormatConverter(&conv)) &&
                                SUCCEEDED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
                                UINT w = 0, h = 0;
                                conv->GetSize(&w, &h);
                                if (w > 0 && h > 0) {
                                    a_out.width = static_cast<int>(w);
                                    a_out.height = static_cast<int>(h);
                                    a_out.rgba.assign(static_cast<std::size_t>(w) * h * 4, 0);
                                    if (SUCCEEDED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(a_out.rgba.size()), a_out.rgba.data()))) {
                                        ok = true;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        if (didInit) {
            CoUninitialize();
        }
        return ok;
    }

}  // namespace SWFParse
