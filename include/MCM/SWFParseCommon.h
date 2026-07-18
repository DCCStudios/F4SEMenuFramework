#pragma once

// Shared low-level SWF parsing machinery used by both SWFLibraryImage (bitmap
// extraction) and SWFVectorMovie (vector shape + timeline rendering).
//
// Everything here is dependency-light on purpose: C++ standard library + zlib
// (+ WIC on Windows for the JPEG decoder), no D3D/ImGui/CommonLibF4 — so the
// SWF modules can be compiled into a standalone test harness and validated
// against real mod files outside the game.
//
// References: Adobe "SWF File Format Specification" v19. Coordinates are in
// twips (1/20 px); fixed-point formats are FB16.16 (matrix scale/rotate) and
// 8.8 (color transform multipliers).

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace SWFParse {

    // ---- SWF tag codes shared by the parsers ------------------------------
    inline constexpr std::uint16_t kTagEnd = 0;
    inline constexpr std::uint16_t kTagShowFrame = 1;
    inline constexpr std::uint16_t kTagDefineShape = 2;
    inline constexpr std::uint16_t kTagPlaceObject = 4;
    inline constexpr std::uint16_t kTagRemoveObject = 5;
    inline constexpr std::uint16_t kTagSetBackgroundColor = 9;
    inline constexpr std::uint16_t kTagDefineBitsLossless = 20;
    inline constexpr std::uint16_t kTagDefineBitsJPEG2 = 21;
    inline constexpr std::uint16_t kTagDefineShape2 = 22;
    inline constexpr std::uint16_t kTagPlaceObject2 = 26;
    inline constexpr std::uint16_t kTagRemoveObject2 = 28;
    inline constexpr std::uint16_t kTagDefineShape3 = 32;
    inline constexpr std::uint16_t kTagDefineBitsJPEG3 = 35;
    inline constexpr std::uint16_t kTagDefineBitsLossless2 = 36;
    inline constexpr std::uint16_t kTagDefineSprite = 39;
    inline constexpr std::uint16_t kTagPlaceObject3 = 70;
    inline constexpr std::uint16_t kTagSymbolClass = 76;
    inline constexpr std::uint16_t kTagDefineShape4 = 83;
    inline constexpr std::uint16_t kTagDefineBitsJPEG4 = 90;

    // ---- Basic value types -------------------------------------------------

    // Straight (non-premultiplied) RGBA color, 0..255 per channel.
    struct Color {
        std::uint8_t r = 0, g = 0, b = 0, a = 255;
    };

    // 2x3 affine transform. Maps (x, y) -> (sx*x + r1*y + tx, r0*x + sy*y + ty)
    // — the exact semantics of the SWF MATRIX record (r0 = RotateSkew0 feeds
    // y' from x; r1 = RotateSkew1 feeds x' from y).
    struct Matrix {
        float sx = 1.0f, r0 = 0.0f, r1 = 0.0f, sy = 1.0f;
        float tx = 0.0f, ty = 0.0f;  // twips

        void Apply(float a_x, float a_y, float& a_ox, float& a_oy) const {
            a_ox = sx * a_x + r1 * a_y + tx;
            a_oy = r0 * a_x + sy * a_y + ty;
        }

        // this ∘ a_inner : applies a_inner first, then this.
        Matrix Concat(const Matrix& a_inner) const {
            Matrix m;
            m.sx = sx * a_inner.sx + r1 * a_inner.r0;
            m.r1 = sx * a_inner.r1 + r1 * a_inner.sy;
            m.r0 = r0 * a_inner.sx + sy * a_inner.r0;
            m.sy = r0 * a_inner.r1 + sy * a_inner.sy;
            m.tx = sx * a_inner.tx + r1 * a_inner.ty + tx;
            m.ty = r0 * a_inner.tx + sy * a_inner.ty + ty;
            return m;
        }

        // Inverse transform; returns identity-degenerate result when the
        // determinant is ~0 (caller-visible as a harmless flat mapping).
        Matrix Invert() const {
            const float det = sx * sy - r0 * r1;
            Matrix m;
            if (det > -1e-9f && det < 1e-9f) {
                m.sx = m.sy = 0.0f;
                return m;
            }
            const float inv = 1.0f / det;
            m.sx = sy * inv;
            m.sy = sx * inv;
            m.r0 = -r0 * inv;
            m.r1 = -r1 * inv;
            m.tx = -(m.sx * tx + m.r1 * ty);
            m.ty = -(m.r0 * tx + m.sy * ty);
            return m;
        }

        // Largest singular-value-ish scale estimate — used to pick raster
        // resolution for a character placed through this transform.
        float MaxScale() const {
            const float a = sx * sx + r0 * r0;
            const float b = r1 * r1 + sy * sy;
            return std::sqrt(a > b ? a : b);
        }
    };

    // Color transform (CXFORM / CXFORMWITHALPHA), already normalized to
    // float multipliers (1.0 = unchanged) and integer add terms.
    struct ColorTransform {
        float mulR = 1.0f, mulG = 1.0f, mulB = 1.0f, mulA = 1.0f;
        int addR = 0, addG = 0, addB = 0, addA = 0;

        ColorTransform Concat(const ColorTransform& a_inner) const {
            ColorTransform c;
            c.mulR = mulR * a_inner.mulR;
            c.mulG = mulG * a_inner.mulG;
            c.mulB = mulB * a_inner.mulB;
            c.mulA = mulA * a_inner.mulA;
            c.addR = static_cast<int>(mulR * a_inner.addR) + addR;
            c.addG = static_cast<int>(mulG * a_inner.addG) + addG;
            c.addB = static_cast<int>(mulB * a_inner.addB) + addB;
            c.addA = static_cast<int>(mulA * a_inner.addA) + addA;
            return c;
        }
    };

    // Rectangle in twips.
    struct Rect {
        int xmin = 0, xmax = 0, ymin = 0, ymax = 0;
    };

    // A decoded 32-bit image: straight-alpha RGBA, row-major, tightly packed.
    struct Image {
        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> rgba;  // width * height * 4
    };

    // A raster character (bitmap) definition, kept as a byte range into the
    // decompressed movie body so only the bitmaps actually needed get decoded.
    struct BitmapRecord {
        std::uint16_t tagCode = 0;  // Lossless/Lossless2/JPEG* tag id
        std::uint8_t format = 0;    // 3/4/5 for lossless
        int width = 0;
        int height = 0;
        std::uint16_t colorTableCount = 0;  // format 3 only
        std::size_t dataOffset = 0;         // start of zlib/jpeg payload in body
        std::size_t dataLen = 0;            // payload length
        std::uint32_t jpegAlphaOffset = 0;  // JPEG3/4: split point (jpeg | alpha)
    };

    // ---- Functions ---------------------------------------------------------

    // zlib inflate with a growing output buffer.
    bool Inflate(const std::uint8_t* a_src, std::size_t a_srcLen,
                 std::vector<std::uint8_t>& a_out, std::size_t a_hint);

    // Reads an SWF container file (FWS/CWS; ZWS unsupported) and produces the
    // decompressed body (everything after the 8-byte header). Returns false on
    // read/parse failure.
    bool ReadMovieBody(const std::string& a_path, std::vector<std::uint8_t>& a_body);

    // Decode a DefineBitsLossless / DefineBitsLossless2 record to RGBA.
    bool DecodeLossless(const BitmapRecord& a_rec, const std::uint8_t* a_body, Image& a_out);

    // Decode a JPEG byte stream (WIC; no alpha) to RGBA.
    bool DecodeJPEG(const std::uint8_t* a_jpeg, std::size_t a_len, Image& a_out);

    // Decode any BitmapRecord (lossless or JPEG2/3/4) to RGBA. For JPEG3/4 the
    // zlib-compressed alpha plane is applied when present.
    bool DecodeBitmap(const BitmapRecord& a_rec, const std::uint8_t* a_body, Image& a_out);

    // Sequential reader over the decompressed movie body, with bit-level reads
    // for the SWF's packed RECT / MATRIX / CXFORM structures.
    class Reader {
    public:
        Reader(const std::uint8_t* a_data, std::size_t a_size) :
            _data(a_data), _size(a_size) {}

        std::size_t Pos() const { return _pos; }
        void Seek(std::size_t a_pos) { _pos = a_pos; AlignByte(); }
        bool Eof() const { return _pos >= _size; }
        std::size_t Remaining() const { return _pos < _size ? _size - _pos : 0; }

        std::uint8_t U8() {
            AlignByte();
            return _pos < _size ? _data[_pos++] : 0;
        }
        std::uint16_t U16() {
            std::uint16_t lo = U8();
            std::uint16_t hi = U8();
            return static_cast<std::uint16_t>(lo | (hi << 8));
        }
        std::uint32_t U32() {
            std::uint32_t b0 = U8(), b1 = U8(), b2 = U8(), b3 = U8();
            return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
        }

        // Null-terminated ASCII string.
        std::string CStr() {
            AlignByte();
            std::string s;
            while (_pos < _size) {
                char c = static_cast<char>(_data[_pos++]);
                if (c == '\0') {
                    break;
                }
                s.push_back(c);
            }
            return s;
        }

        void Skip(std::size_t a_n) {
            AlignByte();
            _pos = (_pos + a_n <= _size) ? _pos + a_n : _size;
        }

        // Unsigned bit field.
        std::uint32_t Bits(int a_n) {
            std::uint32_t v = 0;
            for (int i = 0; i < a_n; ++i) {
                if (_bitCount == 0) {
                    _bitBuf = _pos < _size ? _data[_pos++] : 0;
                    _bitCount = 8;
                }
                --_bitCount;
                v = (v << 1) | ((_bitBuf >> _bitCount) & 1u);
            }
            return v;
        }

        // Signed bit field (two's complement sign extension).
        std::int32_t SBits(int a_n) {
            std::uint32_t v = Bits(a_n);
            if (a_n > 0 && a_n < 32 && (v & (1u << (a_n - 1)))) {
                v |= ~((1u << a_n) - 1);
            }
            return static_cast<std::int32_t>(v);
        }

        void AlignByte() { _bitCount = 0; }

        // RECT: UB[5] nbits, then 4 * SB[nbits]. Byte-aligned afterward.
        Rect ReadRect() {
            AlignByte();
            Rect rc;
            int n = static_cast<int>(Bits(5));
            rc.xmin = SBits(n);
            rc.xmax = SBits(n);
            rc.ymin = SBits(n);
            rc.ymax = SBits(n);
            AlignByte();
            return rc;
        }
        void SkipRect() { (void)ReadRect(); }

        // MATRIX: optional scale/rotate (FB16.16) + translate (twips).
        Matrix ReadMatrix() {
            AlignByte();
            Matrix m;
            if (Bits(1)) {  // HasScale
                int n = static_cast<int>(Bits(5));
                m.sx = SBits(n) / 65536.0f;
                m.sy = SBits(n) / 65536.0f;
            }
            if (Bits(1)) {  // HasRotate
                int n = static_cast<int>(Bits(5));
                m.r0 = SBits(n) / 65536.0f;
                m.r1 = SBits(n) / 65536.0f;
            }
            int nt = static_cast<int>(Bits(5));
            m.tx = static_cast<float>(SBits(nt));
            m.ty = static_cast<float>(SBits(nt));
            AlignByte();
            return m;
        }
        void SkipMatrix() { (void)ReadMatrix(); }

        // CXFORM / CXFORMWITHALPHA. Multipliers are 8.8 fixed -> float.
        ColorTransform ReadColorTransform(bool a_hasAlpha) {
            AlignByte();
            ColorTransform c;
            const bool hasAdd = Bits(1) != 0;
            const bool hasMul = Bits(1) != 0;
            int n = static_cast<int>(Bits(4));
            if (hasMul) {
                c.mulR = SBits(n) / 256.0f;
                c.mulG = SBits(n) / 256.0f;
                c.mulB = SBits(n) / 256.0f;
                if (a_hasAlpha) {
                    c.mulA = SBits(n) / 256.0f;
                }
            }
            if (hasAdd) {
                c.addR = SBits(n);
                c.addG = SBits(n);
                c.addB = SBits(n);
                if (a_hasAlpha) {
                    c.addA = SBits(n);
                }
            }
            AlignByte();
            return c;
        }

        // RGB (alpha forced opaque) or RGBA color record.
        Color ReadColor(bool a_hasAlpha) {
            Color c;
            c.r = U8();
            c.g = U8();
            c.b = U8();
            c.a = a_hasAlpha ? U8() : 255;
            return c;
        }

        const std::uint8_t* DataAt(std::size_t a_off) const { return _data + a_off; }

    private:
        const std::uint8_t* _data;
        std::size_t _size;
        std::size_t _pos = 0;
        std::uint8_t _bitBuf = 0;
        int _bitCount = 0;
    };

}  // namespace SWFParse
