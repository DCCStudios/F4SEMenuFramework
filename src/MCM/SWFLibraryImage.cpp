#include "MCM/SWFLibraryImage.h"

#include "MCM/SWFParseCommon.h"

#include <algorithm>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>

// Optional verbose tracing for the standalone test harness. The plugin build
// leaves this undefined so nothing is printed at runtime.
#ifdef SWFLIB_DEBUG
    #define SWF_TRACE(...) std::fprintf(stderr, __VA_ARGS__)
#else
    #define SWF_TRACE(...) ((void)0)
#endif

namespace SWFLibraryImage {

    using namespace SWFParse;

    namespace {

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

    }  // namespace

    std::optional<DecodedImage> Extract(const std::filesystem::path& a_swfPath, const std::string& a_className) {
        std::vector<std::uint8_t> body;
        if (!ReadMovieBody(a_swfPath.string(), body)) {
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
                if (tagCode != kTagDefineBitsJPEG2) {
                    rec.jpegAlphaOffset = r.U32();
                    if (tagCode == kTagDefineBitsJPEG4) {
                        r.U16();  // deblock param
                    }
                }
                rec.dataOffset = r.Pos();
                rec.dataLen = tagEnd > rec.dataOffset ? tagEnd - rec.dataOffset : 0;
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
                        // PlaceFlagHasCharacter is bit 1 (0x02) per the SWF
                        // spec (bit 6 is HasClipDepth — a long-standing bug
                        // here used that and missed sprite-placed bitmaps).
                        if (flags & 0x02) {
                            placed.push_back(r.U16());
                        }
                    } else if (sTag == kTagPlaceObject3) {
                        std::uint8_t flags = r.U8();
                        std::uint8_t flags2 = r.U8();
                        r.U16();  // depth
                        const bool hasClassName = (flags2 & 0x08) != 0;
                        const bool hasImage = (flags2 & 0x10) != 0;
                        const bool hasChar = (flags & 0x02) != 0;
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

        Image img;
        if (!DecodeBitmap(rec, body.data(), img)) {
            return std::nullopt;
        }
        if (img.width <= 0 || img.height <= 0 || img.rgba.empty()) {
            return std::nullopt;
        }

        DecodedImage out;
        out.width = img.width;
        out.height = img.height;
        out.rgba = std::move(img.rgba);
        return out;
    }

}  // namespace SWFLibraryImage
