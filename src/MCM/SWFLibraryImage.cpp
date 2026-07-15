#include "MCM/SWFLibraryImage.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include <zlib.h>

// Optional verbose tracing for the standalone test harness. The plugin build
// leaves this undefined so nothing is printed at runtime.
#ifdef SWFLIB_DEBUG
    #define SWF_TRACE(...) std::fprintf(stderr, __VA_ARGS__)
#else
    #define SWF_TRACE(...) ((void)0)
#endif

namespace SWFLibraryImage {

    namespace {

        // ---- SWF tag codes we care about -------------------------------------
        constexpr std::uint16_t kTagEnd = 0;
        constexpr std::uint16_t kTagDefineShape = 2;
        constexpr std::uint16_t kTagPlaceObject = 4;
        constexpr std::uint16_t kTagDefineBitsJPEG2 = 21;
        constexpr std::uint16_t kTagDefineShape2 = 22;
        constexpr std::uint16_t kTagPlaceObject2 = 26;
        constexpr std::uint16_t kTagDefineShape3 = 32;
        constexpr std::uint16_t kTagDefineBitsLossless = 20;
        constexpr std::uint16_t kTagDefineBitsJPEG3 = 35;
        constexpr std::uint16_t kTagDefineBitsLossless2 = 36;
        constexpr std::uint16_t kTagDefineSprite = 39;
        constexpr std::uint16_t kTagPlaceObject3 = 70;
        constexpr std::uint16_t kTagSymbolClass = 76;
        constexpr std::uint16_t kTagDefineShape4 = 83;
        constexpr std::uint16_t kTagDefineBitsJPEG4 = 90;

        // A raster character (bitmap) definition, kept as a byte range into the
        // decompressed movie body so we only decode the one we actually need.
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

        // -------------------------------------------------------------------
        // Robust zlib inflate that grows its output buffer as needed.
        // -------------------------------------------------------------------
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

        // -------------------------------------------------------------------
        // Sequential reader over the decompressed movie body, with bit-level
        // reads for the SWF's packed RECT / MATRIX structures.
        // -------------------------------------------------------------------
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

            void AlignByte() { _bitCount = 0; }

            // RECT: UB[5] nbits, then 4 * SB[nbits]. Byte-aligned afterward.
            void SkipRect() {
                int n = static_cast<int>(Bits(5));
                Bits(n);
                Bits(n);
                Bits(n);
                Bits(n);
                AlignByte();
            }

            // MATRIX: optional scale/rotate + translate, all bit-packed.
            void SkipMatrix() {
                if (Bits(1)) {  // HasScale
                    int n = static_cast<int>(Bits(5));
                    Bits(n);
                    Bits(n);
                }
                if (Bits(1)) {  // HasRotate
                    int n = static_cast<int>(Bits(5));
                    Bits(n);
                    Bits(n);
                }
                int nt = static_cast<int>(Bits(5));  // NTranslateBits
                Bits(nt);
                Bits(nt);
                AlignByte();
            }

            const std::uint8_t* DataAt(std::size_t a_off) const { return _data + a_off; }

        private:
            const std::uint8_t* _data;
            std::size_t _size;
            std::size_t _pos = 0;
            std::uint8_t _bitBuf = 0;
            int _bitCount = 0;
        };

        // -------------------------------------------------------------------
        // Fill-style scan of a DefineShapeN body: collects bitmap character IDs
        // referenced by bitmap fills. We only need the fill-style array, so we
        // stop once it has been consumed.
        // -------------------------------------------------------------------
        void CollectShapeBitmapRefs(Reader& a_r, int a_shapeVersion, std::size_t a_tagEnd, std::vector<std::uint16_t>& a_out) {
            a_r.SkipRect();  // ShapeBounds
            if (a_shapeVersion == 4) {
                a_r.SkipRect();  // EdgeBounds
                a_r.U8();        // flags (UsesFillWindingRule / UsesNonScalingStrokes / ...)
            }

            std::uint32_t count = a_r.U8();
            if (count == 0xFF) {
                count = a_r.U16();
            }

            const bool rgba = (a_shapeVersion >= 3);
            for (std::uint32_t i = 0; i < count && a_r.Pos() < a_tagEnd; ++i) {
                std::uint8_t type = a_r.U8();
                if (type == 0x00) {  // solid color
                    a_r.Skip(rgba ? 4 : 3);
                } else if (type == 0x10 || type == 0x12 || type == 0x13) {  // gradients
                    a_r.SkipMatrix();
                    std::uint8_t g = a_r.U8();
                    int numGrads = g & 0x0F;
                    for (int k = 0; k < numGrads; ++k) {
                        a_r.U8();                 // ratio
                        a_r.Skip(rgba ? 4 : 3);   // color
                    }
                    if (type == 0x13) {
                        a_r.U16();  // focal point (FIXED8)
                    }
                } else if (type == 0x40 || type == 0x41 || type == 0x42 || type == 0x43) {  // bitmap fill
                    std::uint16_t bitmapId = a_r.U16();
                    a_r.SkipMatrix();
                    if (bitmapId != 0xFFFF) {
                        a_out.push_back(bitmapId);
                    }
                } else {
                    // Unknown fill style — parsing would desync, so stop here.
                    return;
                }
            }
        }

        // -------------------------------------------------------------------
        // Un-premultiply a single ARGB sample into straight RGBA in `dst`.
        // -------------------------------------------------------------------
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

        // -------------------------------------------------------------------
        // Decode a DefineBitsLossless / DefineBitsLossless2 bitmap to RGBA.
        // -------------------------------------------------------------------
        bool DecodeLossless(const BitmapRecord& a_rec, const std::uint8_t* a_body, DecodedImage& a_out) {
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
                // Pre-expand palette to straight RGBA.
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

    }  // namespace

    // JPEG decode is Windows/WIC-only and pulled in through a separate helper so
    // the lossless path (which covers real MCM content) stays dependency-light.
    // Defined at the bottom of the file.
    namespace {
        bool DecodeJPEG(const std::uint8_t* a_jpeg, std::size_t a_len, DecodedImage& a_out);
    }

    std::optional<DecodedImage> Extract(const std::filesystem::path& a_swfPath, const std::string& a_className) {
        // --- Read whole file ------------------------------------------------
        std::ifstream file(a_swfPath, std::ios::binary);
        if (!file) {
            return std::nullopt;
        }
        std::vector<std::uint8_t> file_bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (file_bytes.size() < 8) {
            return std::nullopt;
        }

        // --- Header + container decompression -------------------------------
        const char sig0 = static_cast<char>(file_bytes[0]);
        const char sig1 = static_cast<char>(file_bytes[1]);
        const char sig2 = static_cast<char>(file_bytes[2]);
        if (sig1 != 'W' || sig2 != 'S') {
            return std::nullopt;
        }
        const std::uint32_t fileLength = file_bytes[4] | (file_bytes[5] << 8) | (file_bytes[6] << 16) | (static_cast<std::uint32_t>(file_bytes[7]) << 24);

        std::vector<std::uint8_t> body;  // everything after the 8-byte header
        if (sig0 == 'F') {               // FWS: uncompressed
            body.assign(file_bytes.begin() + 8, file_bytes.end());
        } else if (sig0 == 'C') {  // CWS: zlib
            const std::size_t hint = fileLength > 8 ? fileLength - 8 : file_bytes.size() * 4;
            if (!Inflate(file_bytes.data() + 8, file_bytes.size() - 8, body, hint)) {
                return std::nullopt;
            }
        } else {
            // ZWS (LZMA) — not supported.
            return std::nullopt;
        }

        // --- Walk tags ------------------------------------------------------
        Reader r(body.data(), body.size());
        r.SkipRect();  // FrameSize
        r.U16();       // frameRate (fixed8)
        r.U16();       // frameCount

        std::unordered_map<std::string, std::uint16_t> symbolMap;      // class name -> character id
        std::unordered_map<std::uint16_t, BitmapRecord> bitmaps;       // char id -> bitmap
        std::unordered_map<std::uint16_t, std::vector<std::uint16_t>> shapeRefs;   // shape id -> bitmap ids
        std::unordered_map<std::uint16_t, std::vector<std::uint16_t>> spriteRefs;  // sprite id -> placed char ids

        while (!r.Eof()) {
            std::uint16_t recordHeader = r.U16();
            std::uint16_t tagCode = static_cast<std::uint16_t>(recordHeader >> 6);
            std::uint32_t length = recordHeader & 0x3F;
            if (length == 0x3F) {
                length = r.U32();
            }
            const std::size_t tagStart = r.Pos();
            const std::size_t tagEnd = std::min(tagStart + length, body.size());

            if (tagCode == kTagEnd) {
                break;
            }

            switch (tagCode) {
            case kTagSymbolClass: {
                std::uint16_t numSymbols = r.U16();
                for (std::uint16_t i = 0; i < numSymbols && r.Pos() < tagEnd; ++i) {
                    std::uint16_t cid = r.U16();
                    std::string name = r.CStr();
                    if (!name.empty()) {
                        symbolMap[name] = cid;
                    }
                }
                break;
            }
            case kTagDefineBitsLossless:
            case kTagDefineBitsLossless2: {
                BitmapRecord rec;
                rec.tagCode = tagCode;
                std::uint16_t cid = r.U16();
                rec.format = r.U8();
                rec.width = r.U16();
                rec.height = r.U16();
                if (rec.format == 3) {
                    rec.colorTableCount = static_cast<std::uint16_t>(r.U8()) + 1;
                }
                rec.dataOffset = r.Pos();
                rec.dataLen = tagEnd > rec.dataOffset ? tagEnd - rec.dataOffset : 0;
                if (rec.width > 0 && rec.height > 0) {
                    bitmaps[cid] = rec;
                }
                break;
            }
            case kTagDefineBitsJPEG2:
            case kTagDefineBitsJPEG3:
            case kTagDefineBitsJPEG4: {
                BitmapRecord rec;
                rec.tagCode = tagCode;
                std::uint16_t cid = r.U16();
                if (tagCode == kTagDefineBitsJPEG2) {
                    rec.dataOffset = r.Pos();
                    rec.dataLen = tagEnd > rec.dataOffset ? tagEnd - rec.dataOffset : 0;
                } else {
                    std::uint32_t alphaOff = r.U32();
                    if (tagCode == kTagDefineBitsJPEG4) {
                        r.U16();  // deblock param
                    }
                    rec.dataOffset = r.Pos();
                    rec.dataLen = alphaOff;  // JPEG portion length
                    rec.jpegAlphaOffset = alphaOff;
                }
                // Dimensions unknown until decoded; mark as 0 so lossless bitmaps
                // (with known area) win "largest" comparisons.
                bitmaps[cid] = rec;
                break;
            }
            case kTagDefineShape:
            case kTagDefineShape2:
            case kTagDefineShape3:
            case kTagDefineShape4: {
                int ver = (tagCode == kTagDefineShape) ? 1 : (tagCode == kTagDefineShape2) ? 2 : (tagCode == kTagDefineShape3) ? 3 : 4;
                std::uint16_t shapeId = r.U16();
                std::vector<std::uint16_t> refs;
                CollectShapeBitmapRefs(r, ver, tagEnd, refs);
                if (!refs.empty()) {
                    shapeRefs[shapeId] = std::move(refs);
                }
                break;
            }
            case kTagDefineSprite: {
                std::uint16_t spriteId = r.U16();
                r.U16();  // frame count
                std::vector<std::uint16_t> placed;
                // Walk the sprite's own control tags for character placements.
                while (r.Pos() < tagEnd) {
                    std::uint16_t sh = r.U16();
                    std::uint16_t sTag = static_cast<std::uint16_t>(sh >> 6);
                    std::uint32_t sLen = sh & 0x3F;
                    if (sLen == 0x3F) {
                        sLen = r.U32();
                    }
                    std::size_t sStart = r.Pos();
                    std::size_t sEnd = std::min(sStart + sLen, tagEnd);
                    if (sTag == kTagEnd) {
                        break;
                    }
                    if (sTag == kTagPlaceObject) {
                        placed.push_back(r.U16());  // character id is first field
                    } else if (sTag == kTagPlaceObject2) {
                        std::uint8_t flags = r.U8();
                        r.U16();  // depth
                        if (flags & 0x40) {  // HasCharacter
                            placed.push_back(r.U16());
                        }
                    } else if (sTag == kTagPlaceObject3) {
                        std::uint8_t flags = r.U8();
                        std::uint8_t flags2 = r.U8();
                        r.U16();  // depth
                        const bool hasClassName = (flags2 & 0x08) != 0;
                        const bool hasImage = (flags2 & 0x10) != 0;
                        const bool hasChar = (flags & 0x40) != 0;
                        if (hasClassName || (hasImage && hasChar)) {
                            r.CStr();  // class name
                        }
                        if (hasChar) {
                            placed.push_back(r.U16());
                        }
                    }
                    r.Seek(sEnd);
                }
                if (!placed.empty()) {
                    spriteRefs[spriteId] = std::move(placed);
                }
                break;
            }
            default:
                break;
            }

            r.Seek(tagEnd);
        }

        if (bitmaps.empty()) {
            SWF_TRACE("[swf] no bitmaps in %s\n", a_swfPath.string().c_str());
            return std::nullopt;
        }

        // --- Resolve the requested class to a bitmap ------------------------
        auto areaOf = [&](std::uint16_t cid) -> long long {
            auto it = bitmaps.find(cid);
            if (it == bitmaps.end()) {
                return -1;
            }
            return static_cast<long long>(it->second.width) * it->second.height;
        };

        // BFS from a start character id, collecting reachable bitmap ids.
        auto collectReachableBitmaps = [&](std::uint16_t a_start, std::vector<std::uint16_t>& a_found) {
            std::unordered_set<std::uint16_t> seen;
            std::vector<std::uint16_t> stack{ a_start };
            while (!stack.empty()) {
                std::uint16_t id = stack.back();
                stack.pop_back();
                if (!seen.insert(id).second) {
                    continue;
                }
                if (bitmaps.count(id)) {
                    a_found.push_back(id);
                }
                if (auto s = spriteRefs.find(id); s != spriteRefs.end()) {
                    for (auto c : s->second) {
                        stack.push_back(c);
                    }
                }
                if (auto sh = shapeRefs.find(id); sh != shapeRefs.end()) {
                    for (auto b : sh->second) {
                        stack.push_back(b);
                    }
                }
            }
        };

        std::uint16_t chosen = 0xFFFF;
        bool haveChosen = false;

        // Look up the class id directly, or via the "<name>_bitmap" companion
        // symbol M8r's framework exports for image assets.
        auto lookup = [&](const std::string& n) -> int {
            if (auto it = symbolMap.find(n); it != symbolMap.end()) {
                return it->second;
            }
            return -1;
        };
        int startId = lookup(a_className);
        if (startId < 0) {
            startId = lookup(a_className + "_bitmap");
        }

        if (startId >= 0) {
            std::uint16_t sid = static_cast<std::uint16_t>(startId);
            if (bitmaps.count(sid)) {
                chosen = sid;
                haveChosen = true;
            } else {
                std::vector<std::uint16_t> reachable;
                collectReachableBitmaps(sid, reachable);
                long long best = -1;
                for (auto b : reachable) {
                    long long a = areaOf(b);
                    if (a > best) {
                        best = a;
                        chosen = b;
                        haveChosen = true;
                    }
                }
            }
        }

        // Fallback: the largest bitmap in the whole file.
        if (!haveChosen) {
            long long best = -1;
            for (const auto& [cid, rec] : bitmaps) {
                long long a = static_cast<long long>(rec.width) * rec.height;
                if (a > best) {
                    best = a;
                    chosen = cid;
                    haveChosen = true;
                }
            }
        }

        if (!haveChosen) {
            return std::nullopt;
        }

        const BitmapRecord& rec = bitmaps[chosen];
        SWF_TRACE("[swf] '%s' -> char %u (%s %dx%d)\n", a_className.c_str(), chosen,
                  rec.tagCode == kTagDefineBitsLossless2 ? "Lossless2" : rec.tagCode == kTagDefineBitsLossless ? "Lossless" : "JPEG",
                  rec.width, rec.height);

        DecodedImage out;
        if (rec.tagCode == kTagDefineBitsLossless || rec.tagCode == kTagDefineBitsLossless2) {
            if (!DecodeLossless(rec, body.data(), out)) {
                return std::nullopt;
            }
        } else {
            if (!DecodeJPEG(body.data() + rec.dataOffset, rec.dataLen, out)) {
                return std::nullopt;
            }
        }
        if (out.width <= 0 || out.height <= 0 || out.rgba.empty()) {
            return std::nullopt;
        }
        return out;
    }

}  // namespace SWFLibraryImage

// ---------------------------------------------------------------------------
// WIC-based JPEG decoder (best-effort, no alpha channel). Isolated here so the
// rest of the module has no Windows dependency.
// ---------------------------------------------------------------------------
#include <wincodec.h>
#include <wrl/client.h>

namespace SWFLibraryImage {
    namespace {
        bool DecodeJPEG(const std::uint8_t* a_jpeg, std::size_t a_len, DecodedImage& a_out) {
            if (!a_jpeg || a_len == 0) {
                return false;
            }
            using Microsoft::WRL::ComPtr;

            // WIC needs COM; init per-call in single-threaded apartment-agnostic
            // mode and tolerate "already initialized".
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
    }  // namespace
}  // namespace SWFLibraryImage
