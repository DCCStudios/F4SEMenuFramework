#include "MCM/SWFVectorMovie.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <unordered_map>
#include <unordered_set>

// Optional verbose tracing for the standalone test harness (same switch as
// SWFLibraryImage). The plugin build leaves this undefined.
#ifdef SWFLIB_DEBUG
    #include <cstdio>
    #define SWF_TRACE(...) std::fprintf(stderr, __VA_ARGS__)
#else
    #define SWF_TRACE(...) ((void)0)
#endif

namespace SWFVectorMovie {

    using namespace SWFParse;

    namespace {

        // =================================================================
        // Internal document model
        // =================================================================

        struct FillStyle {
            enum class Type { Solid, Linear, Radial, Focal, Bitmap };
            struct Stop {
                std::uint8_t ratio = 0;
                Color color{};
            };

            Type type = Type::Solid;
            Color color{};
            Matrix matrix{};  // fill space -> shape (character) space
            std::vector<Stop> stops;
            float focal = 0.0f;              // focal gradients: -1..1
            std::uint16_t bitmapId = 0xFFFF;
            bool bitmapRepeat = true;        // false = clipped
            bool bitmapSmooth = true;        // false = nearest sampling
        };

        struct LineStyle {
            float width = 20.0f;  // twips
            Color color{};
        };

        // One shape edge in character twip space. Curved edges are quadratic
        // Béziers ((x0,y0) -(cx,cy)-> (x1,y1)).
        struct Edge {
            float x0 = 0, y0 = 0, cx = 0, cy = 0, x1 = 0, y1 = 0;
            bool curved = false;
        };

        // A run of edges sharing one (fill0, fill1, line) style selection.
        // Indices are 1-based into the owning layer's style arrays; 0 = none.
        struct SubPath {
            int f0 = 0, f1 = 0, ln = 0;
            std::vector<Edge> edges;
        };

        // A style group. DefineShape2+ can replace the style arrays mid-shape
        // (StateNewStyles); each replacement starts a new layer, drawn after
        // the previous one.
        struct Layer {
            std::vector<FillStyle> fills;
            std::vector<LineStyle> lines;
            std::vector<SubPath> paths;
        };

        struct Shape {
            Rect bounds{};
            std::vector<Layer> layers;
        };

        // A display-list slot: which character sits at a depth, with what
        // transform. `clipDepth` != 0 marks a mask placement (not drawn;
        // actual clipping of the masked range is not supported).
        struct Placement {
            std::uint16_t charId = 0;
            Matrix matrix{};
            ColorTransform cxform{};
            std::uint16_t clipDepth = 0;
        };

        // One timeline frame = the full display list after that frame's tags
        // (deep snapshot; logo timelines are short so this stays small).
        struct TFrame {
            std::vector<std::pair<int, Placement>> list;  // sorted by depth
        };

        struct Timeline {
            std::vector<TFrame> frames;
        };

        // A shape scheduled for rasterization: geometry + the FULL transform
        // from character twips into output raster pixels, + accumulated color
        // transform.
        struct DrawItem {
            const Shape* shape = nullptr;
            Matrix toRaster{};  // char twips -> raster px
            ColorTransform cxform{};
        };

        // =================================================================
        // Style / shape parsing
        // =================================================================

        FillStyle ReadFillStyle(Reader& a_r, int a_shapeVer, bool& a_ok) {
            FillStyle f;
            const std::uint8_t type = a_r.U8();
            const bool rgba = (a_shapeVer >= 3);
            switch (type) {
            case 0x00:
                f.type = FillStyle::Type::Solid;
                f.color = a_r.ReadColor(rgba);
                break;
            case 0x10:
            case 0x12:
            case 0x13: {
                f.type = (type == 0x10) ? FillStyle::Type::Linear
                       : (type == 0x12) ? FillStyle::Type::Radial
                                        : FillStyle::Type::Focal;
                f.matrix = a_r.ReadMatrix();
                const std::uint8_t g = a_r.U8();  // spread(2) | interp(2) | count(4)
                const int numGrads = g & 0x0F;
                for (int k = 0; k < numGrads; ++k) {
                    FillStyle::Stop stop;
                    stop.ratio = a_r.U8();
                    stop.color = a_r.ReadColor(rgba);
                    f.stops.push_back(stop);
                }
                if (type == 0x13) {
                    f.focal = static_cast<std::int16_t>(a_r.U16()) / 256.0f;
                }
                break;
            }
            case 0x40:
            case 0x41:
            case 0x42:
            case 0x43:
                f.type = FillStyle::Type::Bitmap;
                f.bitmapId = a_r.U16();
                f.matrix = a_r.ReadMatrix();
                f.bitmapRepeat = (type == 0x40 || type == 0x42);
                f.bitmapSmooth = (type == 0x40 || type == 0x41);
                break;
            default:
                // Unknown fill style — the stream position is now undefined.
                a_ok = false;
                break;
            }
            return f;
        }

        LineStyle ReadLineStyle(Reader& a_r, int a_shapeVer, bool& a_ok) {
            LineStyle ls;
            ls.width = static_cast<float>(a_r.U16());
            if (a_shapeVer >= 4) {
                // LINESTYLE2 (bit-packed flag fields).
                a_r.AlignByte();
                a_r.Bits(2);                            // StartCapStyle
                const std::uint32_t join = a_r.Bits(2); // JoinStyle
                const bool hasFill = a_r.Bits(1) != 0;
                a_r.Bits(1);  // NoHScale
                a_r.Bits(1);  // NoVScale
                a_r.Bits(1);  // PixelHinting
                a_r.Bits(5);  // Reserved
                a_r.Bits(1);  // NoClose
                a_r.Bits(2);  // EndCapStyle
                a_r.AlignByte();
                if (join == 2) {
                    a_r.U16();  // MiterLimitFactor
                }
                if (hasFill) {
                    // Full fill styles on strokes aren't rasterized; use a
                    // representative solid color instead.
                    FillStyle fs = ReadFillStyle(a_r, a_shapeVer, a_ok);
                    if (fs.type == FillStyle::Type::Solid) {
                        ls.color = fs.color;
                    } else if (!fs.stops.empty()) {
                        ls.color = fs.stops.front().color;
                    } else {
                        ls.color = Color{ 255, 255, 255, 255 };
                    }
                } else {
                    ls.color = a_r.ReadColor(true);
                }
            } else {
                ls.color = a_r.ReadColor(a_shapeVer >= 3);
            }
            return ls;
        }

        void ReadStyleArrays(Reader& a_r, int a_shapeVer, Layer& a_layer, bool& a_ok) {
            std::uint32_t fillCount = a_r.U8();
            if (fillCount == 0xFF) {
                fillCount = a_r.U16();
            }
            for (std::uint32_t i = 0; i < fillCount && a_ok; ++i) {
                a_layer.fills.push_back(ReadFillStyle(a_r, a_shapeVer, a_ok));
            }
            std::uint32_t lineCount = a_r.U8();
            if (lineCount == 0xFF) {
                lineCount = a_r.U16();
            }
            for (std::uint32_t i = 0; i < lineCount && a_ok; ++i) {
                a_layer.lines.push_back(ReadLineStyle(a_r, a_shapeVer, a_ok));
            }
        }

        // Parses a full DefineShapeN tag body (reader positioned after the
        // shape id) into geometry. Returns false if an unknown record type
        // desynced the stream (partial geometry is discarded by the caller).
        bool ParseShape(Reader& a_r, int a_shapeVer, std::size_t a_tagEnd, Shape& a_out) {
            a_out.bounds = a_r.ReadRect();
            if (a_shapeVer >= 4) {
                a_r.ReadRect();  // EdgeBounds
                a_r.U8();        // UsesFillWindingRule / NonScalingStrokes / ...
            }

            bool ok = true;
            Layer layer;
            ReadStyleArrays(a_r, a_shapeVer, layer, ok);
            if (!ok) {
                return false;
            }

            a_r.AlignByte();
            int numFillBits = static_cast<int>(a_r.Bits(4));
            int numLineBits = static_cast<int>(a_r.Bits(4));

            float px = 0.0f, py = 0.0f;  // pen, twips
            int f0 = 0, f1 = 0, ln = 0;  // current style selection
            SubPath cur;

            auto beginSubPath = [&]() {
                if (!cur.edges.empty()) {
                    layer.paths.push_back(std::move(cur));
                }
                cur = SubPath{};
                cur.f0 = f0;
                cur.f1 = f1;
                cur.ln = ln;
            };
            beginSubPath();

            while (a_r.Pos() < a_tagEnd) {
                if (a_r.Bits(1) == 0) {
                    // ---- Style-change / end record ----
                    const std::uint32_t flags = a_r.Bits(5);
                    if (flags == 0) {
                        break;  // EndShapeRecord
                    }
                    if (flags & 0x01) {  // StateMoveTo (absolute)
                        const int n = static_cast<int>(a_r.Bits(5));
                        px = static_cast<float>(a_r.SBits(n));
                        py = static_cast<float>(a_r.SBits(n));
                    }
                    if (flags & 0x02) {  // StateFillStyle0
                        f0 = static_cast<int>(a_r.Bits(numFillBits));
                    }
                    if (flags & 0x04) {  // StateFillStyle1
                        f1 = static_cast<int>(a_r.Bits(numFillBits));
                    }
                    if (flags & 0x08) {  // StateLineStyle
                        ln = static_cast<int>(a_r.Bits(numLineBits));
                    }
                    if (flags & 0x10) {  // StateNewStyles (Shape2+)
                        beginSubPath();
                        a_out.layers.push_back(std::move(layer));
                        layer = Layer{};
                        ReadStyleArrays(a_r, a_shapeVer, layer, ok);
                        if (!ok) {
                            return false;
                        }
                        a_r.AlignByte();
                        numFillBits = static_cast<int>(a_r.Bits(4));
                        numLineBits = static_cast<int>(a_r.Bits(4));
                        f0 = f1 = ln = 0;
                    }
                    beginSubPath();
                } else {
                    // ---- Edge record ----
                    if (a_r.Bits(1)) {
                        // Straight edge
                        const int n = static_cast<int>(a_r.Bits(4)) + 2;
                        float dx = 0.0f, dy = 0.0f;
                        if (a_r.Bits(1)) {  // GeneralLine
                            dx = static_cast<float>(a_r.SBits(n));
                            dy = static_cast<float>(a_r.SBits(n));
                        } else if (a_r.Bits(1)) {  // Vertical
                            dy = static_cast<float>(a_r.SBits(n));
                        } else {  // Horizontal
                            dx = static_cast<float>(a_r.SBits(n));
                        }
                        Edge e;
                        e.x0 = px;
                        e.y0 = py;
                        e.x1 = px + dx;
                        e.y1 = py + dy;
                        px = e.x1;
                        py = e.y1;
                        cur.edges.push_back(e);
                    } else {
                        // Curved (quadratic) edge
                        const int n = static_cast<int>(a_r.Bits(4)) + 2;
                        const float cdx = static_cast<float>(a_r.SBits(n));
                        const float cdy = static_cast<float>(a_r.SBits(n));
                        const float adx = static_cast<float>(a_r.SBits(n));
                        const float ady = static_cast<float>(a_r.SBits(n));
                        Edge e;
                        e.curved = true;
                        e.x0 = px;
                        e.y0 = py;
                        e.cx = px + cdx;
                        e.cy = py + cdy;
                        e.x1 = e.cx + adx;
                        e.y1 = e.cy + ady;
                        px = e.x1;
                        py = e.y1;
                        cur.edges.push_back(e);
                    }
                }
            }

            beginSubPath();  // flush trailing edges
            a_out.layers.push_back(std::move(layer));
            return true;
        }

        // =================================================================
        // Timeline (control tag) parsing
        // =================================================================

        // Applies one PlaceObject/PlaceObject2/PlaceObject3 / RemoveObject*
        // tag to the running display list. Reader is positioned after the tag
        // header; the caller seeks to a_tagEnd afterwards regardless.
        void ApplyControlTag(std::uint16_t a_tagCode, Reader& a_r, std::size_t a_tagEnd,
                             std::map<int, Placement>& a_state) {
            switch (a_tagCode) {
            case kTagPlaceObject: {
                Placement p;
                p.charId = a_r.U16();
                const int depth = a_r.U16();
                p.matrix = a_r.ReadMatrix();
                if (a_r.Pos() < a_tagEnd) {
                    p.cxform = a_r.ReadColorTransform(false);
                }
                a_state[depth] = p;
                break;
            }
            case kTagPlaceObject2:
            case kTagPlaceObject3: {
                const std::uint8_t flags = a_r.U8();
                std::uint8_t flags2 = 0;
                if (a_tagCode == kTagPlaceObject3) {
                    flags2 = a_r.U8();
                }
                const int depth = a_r.U16();

                const bool isMove = (flags & 0x01) != 0;
                const bool hasChar = (flags & 0x02) != 0;

                // Move on an existing slot keeps its previous state as the
                // starting point; anything else starts fresh.
                Placement p;
                if (isMove) {
                    if (auto it = a_state.find(depth); it != a_state.end()) {
                        p = it->second;
                    }
                }

                if (a_tagCode == kTagPlaceObject3) {
                    const bool hasClassName = (flags2 & 0x08) != 0;
                    const bool hasImage = (flags2 & 0x10) != 0;
                    if (hasClassName || (hasImage && hasChar)) {
                        a_r.CStr();
                    }
                }
                if (hasChar) {
                    p.charId = a_r.U16();
                }
                if (flags & 0x04) {  // HasMatrix
                    p.matrix = a_r.ReadMatrix();
                }
                if (flags & 0x08) {  // HasColorTransform
                    p.cxform = a_r.ReadColorTransform(true);
                }
                if (flags & 0x10) {  // HasRatio (morph position — unsupported)
                    a_r.U16();
                }
                if (flags & 0x20) {  // HasName
                    a_r.CStr();
                }
                if (flags & 0x40) {  // HasClipDepth — this placement is a mask
                    p.clipDepth = a_r.U16();
                }
                // PlaceObject3 filter list / blend mode etc. and PlaceObject2
                // clip actions follow; nothing there affects us — the caller's
                // Seek(a_tagEnd) skips them.

                if (p.charId != 0 || isMove) {
                    a_state[depth] = p;
                }
                break;
            }
            case kTagRemoveObject:
                a_r.U16();  // character id
                a_state.erase(a_r.U16());
                break;
            case kTagRemoveObject2:
                a_state.erase(a_r.U16());
                break;
            default:
                break;
            }
        }

        void SnapshotFrame(const std::map<int, Placement>& a_state, Timeline& a_tl) {
            TFrame f;
            f.list.assign(a_state.begin(), a_state.end());
            a_tl.frames.push_back(std::move(f));
        }

        // Parses the control tags of a DefineSprite body into a timeline.
        void ParseSpriteTimeline(Reader& a_r, std::size_t a_spriteEnd, Timeline& a_tl) {
            std::map<int, Placement> state;
            while (a_r.Pos() < a_spriteEnd) {
                const std::uint16_t rh = a_r.U16();
                const std::uint16_t tag = static_cast<std::uint16_t>(rh >> 6);
                std::uint32_t len = rh & 0x3F;
                if (len == 0x3F) {
                    len = a_r.U32();
                }
                const std::size_t end = std::min(a_r.Pos() + len, a_spriteEnd);
                if (tag == kTagEnd) {
                    break;
                }
                if (tag == kTagShowFrame) {
                    SnapshotFrame(state, a_tl);
                } else {
                    ApplyControlTag(tag, a_r, end, state);
                }
                a_r.Seek(end);
            }
            // Malformed sprites without a ShowFrame still get their content.
            if (a_tl.frames.empty() && !state.empty()) {
                SnapshotFrame(state, a_tl);
            }
        }

    }  // namespace

    // =====================================================================
    // Movie implementation container
    // =====================================================================

    struct Movie::Impl {
        float frameRate = 12.0f;
        Rect stage{};

        std::unordered_map<std::uint16_t, Shape> shapes;
        std::unordered_map<std::uint16_t, Timeline> sprites;
        std::unordered_map<std::uint16_t, BitmapRecord> bitmapRecs;
        std::unordered_map<std::uint16_t, Image> bitmaps;  // decoded fills

        Timeline root;
        Rect contentBounds{};  // twips, union over all root frames
        bool animated = false;

        std::vector<std::unique_ptr<RasterChar>> rasters;
        std::unordered_map<std::uint16_t, const RasterChar*> rasterById;

        // --- Display-tree walking --------------------------------------

        // Visits every draw item of root frame `a_frame`. Nested sprites use
        // the same global frame counter modulo their own length (independent
        // looping, all instances in sync). `a_visitShape(shape, charId,
        // cumulativeMatrix, cumulativeCxform)`.
        template <class F>
        void WalkFrame(const Timeline& a_tl, int a_frame, const Matrix& a_m,
                       const ColorTransform& a_cx, int a_depthGuard, F&& a_visitShape) const {
            if (a_tl.frames.empty() || a_depthGuard <= 0) {
                return;
            }
            const auto& frame = a_tl.frames[static_cast<std::size_t>(a_frame) % a_tl.frames.size()];
            for (const auto& [depth, p] : frame.list) {
                if (p.clipDepth != 0) {
                    continue;  // mask placements are never drawn
                }
                const Matrix m = a_m.Concat(p.matrix);
                const ColorTransform cx = a_cx.Concat(p.cxform);
                if (auto it = shapes.find(p.charId); it != shapes.end()) {
                    a_visitShape(it->second, p.charId, m, cx);
                } else if (auto s = sprites.find(p.charId); s != sprites.end()) {
                    WalkFrame(s->second, a_frame, m, cx, a_depthGuard - 1, a_visitShape);
                }
                // Other character types (text, morphs, direct bitmaps) are
                // not renderable here — skipped.
            }
        }
    };

    // =====================================================================
    // Rasterizer
    // =====================================================================

    namespace {

        struct Seg {
            float x0, y0, x1, y1;
        };

        // Flattens one transformed quadratic Bézier into segments.
        void FlattenCurve(float a_x0, float a_y0, float a_cx, float a_cy, float a_x1, float a_y1,
                          std::vector<Seg>& a_out, int a_depth) {
            // Flat enough when the control point is close to the chord.
            const float mx = (a_x0 + a_x1) * 0.5f;
            const float my = (a_y0 + a_y1) * 0.5f;
            const float dx = a_cx - mx;
            const float dy = a_cy - my;
            if (a_depth <= 0 || (dx * dx + dy * dy) < 0.0625f) {  // 0.25px tolerance
                a_out.push_back({ a_x0, a_y0, a_x1, a_y1 });
                return;
            }
            // de Casteljau split at t = 0.5
            const float qx0 = (a_x0 + a_cx) * 0.5f, qy0 = (a_y0 + a_cy) * 0.5f;
            const float qx1 = (a_cx + a_x1) * 0.5f, qy1 = (a_cy + a_y1) * 0.5f;
            const float sx = (qx0 + qx1) * 0.5f, sy = (qy0 + qy1) * 0.5f;
            FlattenCurve(a_x0, a_y0, qx0, qy0, sx, sy, a_out, a_depth - 1);
            FlattenCurve(sx, sy, qx1, qy1, a_x1, a_y1, a_out, a_depth - 1);
        }

        // Transforms + flattens one subpath's edges into raster-space segments.
        void FlattenSubPath(const SubPath& a_path, const Matrix& a_toRaster, std::vector<Seg>& a_out) {
            for (const Edge& e : a_path.edges) {
                float x0, y0, x1, y1;
                a_toRaster.Apply(e.x0, e.y0, x0, y0);
                a_toRaster.Apply(e.x1, e.y1, x1, y1);
                if (e.curved) {
                    float cx, cy;
                    a_toRaster.Apply(e.cx, e.cy, cx, cy);
                    FlattenCurve(x0, y0, cx, cy, x1, y1, a_out, 16);
                } else {
                    a_out.push_back({ x0, y0, x1, y1 });
                }
            }
        }

        inline std::uint8_t ClampU8(float a_v) {
            return static_cast<std::uint8_t>(a_v < 0.0f ? 0 : (a_v > 255.0f ? 255 : a_v + 0.5f));
        }

        // Applies a color transform to a straight-alpha color.
        inline Color ApplyCx(const Color& a_c, const ColorTransform& a_cx) {
            Color o;
            o.r = ClampU8(a_c.r * a_cx.mulR + a_cx.addR);
            o.g = ClampU8(a_c.g * a_cx.mulG + a_cx.addG);
            o.b = ClampU8(a_c.b * a_cx.mulB + a_cx.addB);
            o.a = ClampU8(a_c.a * a_cx.mulA + a_cx.addA);
            return o;
        }

        // Per-pixel style color evaluator, pre-baked for one (item, style).
        struct StyleSampler {
            const FillStyle* fill = nullptr;
            const Image* bitmap = nullptr;   // for bitmap fills
            Matrix rasterToFill{};           // raster px -> fill space (twips)
            ColorTransform cxform{};

            Color Sample(float a_px, float a_py) const {
                switch (fill->type) {
                case FillStyle::Type::Solid:
                    return ApplyCx(fill->color, cxform);

                case FillStyle::Type::Linear: {
                    float gx, gy;
                    rasterToFill.Apply(a_px, a_py, gx, gy);
                    const float t = (gx + 16384.0f) / 32768.0f;
                    return ApplyCx(SampleRamp(t), cxform);
                }
                case FillStyle::Type::Radial: {
                    float gx, gy;
                    rasterToFill.Apply(a_px, a_py, gx, gy);
                    const float t = std::sqrt(gx * gx + gy * gy) / 16384.0f;
                    return ApplyCx(SampleRamp(t), cxform);
                }
                case FillStyle::Type::Focal: {
                    float gx, gy;
                    rasterToFill.Apply(a_px, a_py, gx, gy);
                    // Focal point F on the x axis; t = |P-F| / |Q-F| where Q is
                    // the intersection of ray F->P with the r=16384 circle.
                    const float fx = fill->focal * 16384.0f;
                    float dx = gx - fx, dy = gy;
                    const float dist = std::sqrt(dx * dx + dy * dy);
                    float t = 0.0f;
                    if (dist > 1e-4f) {
                        dx /= dist;
                        dy /= dist;
                        // Solve |F + s*D| = 16384 for s > 0.
                        const float b = fx * dx;  // F·D
                        const float c = fx * fx - 16384.0f * 16384.0f;
                        const float disc = b * b - c;
                        const float s = disc > 0.0f ? (-b + std::sqrt(disc)) : 0.0f;
                        t = s > 1e-4f ? dist / s : 1.0f;
                    }
                    return ApplyCx(SampleRamp(t), cxform);
                }
                case FillStyle::Type::Bitmap: {
                    if (!bitmap || bitmap->width <= 0 || bitmap->height <= 0) {
                        return Color{ 0, 0, 0, 0 };
                    }
                    // The fill matrix's input space is bitmap PIXELS (tools
                    // export a x20 scale so one pixel covers 20 twips = 1 px
                    // on stage); its inverse therefore yields pixels directly.
                    float bx, by;
                    rasterToFill.Apply(a_px, a_py, bx, by);
                    return ApplyCx(SampleBitmap(bx, by), cxform);
                }
                }
                return Color{ 0, 0, 0, 0 };
            }

        private:
            Color SampleRamp(float a_t) const {
                const auto& stops = fill->stops;
                if (stops.empty()) {
                    return Color{ 0, 0, 0, 0 };
                }
                float t = a_t < 0.0f ? 0.0f : (a_t > 1.0f ? 1.0f : a_t);
                const float r255 = t * 255.0f;
                if (r255 <= stops.front().ratio) {
                    return stops.front().color;
                }
                for (std::size_t i = 1; i < stops.size(); ++i) {
                    if (r255 <= stops[i].ratio) {
                        const float span = static_cast<float>(stops[i].ratio - stops[i - 1].ratio);
                        const float f = span > 0.0f ? (r255 - stops[i - 1].ratio) / span : 0.0f;
                        const Color& c0 = stops[i - 1].color;
                        const Color& c1 = stops[i].color;
                        Color o;
                        o.r = ClampU8(c0.r + (c1.r - c0.r) * f);
                        o.g = ClampU8(c0.g + (c1.g - c0.g) * f);
                        o.b = ClampU8(c0.b + (c1.b - c0.b) * f);
                        o.a = ClampU8(c0.a + (c1.a - c0.a) * f);
                        return o;
                    }
                }
                return stops.back().color;
            }

            Color SampleBitmap(float a_x, float a_y) const {
                const int w = bitmap->width, h = bitmap->height;
                auto fetch = [&](int x, int y) -> Color {
                    if (fill->bitmapRepeat) {
                        x %= w; if (x < 0) x += w;
                        y %= h; if (y < 0) y += h;
                    } else {
                        x = x < 0 ? 0 : (x >= w ? w - 1 : x);
                        y = y < 0 ? 0 : (y >= h ? h - 1 : y);
                    }
                    const std::uint8_t* p = bitmap->rgba.data() + (static_cast<std::size_t>(y) * w + x) * 4;
                    return Color{ p[0], p[1], p[2], p[3] };
                };
                if (!fill->bitmapSmooth) {
                    return fetch(static_cast<int>(std::floor(a_x)), static_cast<int>(std::floor(a_y)));
                }
                // Bilinear
                const float fx = a_x - 0.5f, fy = a_y - 0.5f;
                const int ix = static_cast<int>(std::floor(fx));
                const int iy = static_cast<int>(std::floor(fy));
                const float tx = fx - ix, ty = fy - iy;
                const Color c00 = fetch(ix, iy), c10 = fetch(ix + 1, iy);
                const Color c01 = fetch(ix, iy + 1), c11 = fetch(ix + 1, iy + 1);
                auto lerp2 = [&](auto get) -> float {
                    const float top = get(c00) + (get(c10) - get(c00)) * tx;
                    const float bot = get(c01) + (get(c11) - get(c01)) * tx;
                    return top + (bot - top) * ty;
                };
                Color o;
                o.r = ClampU8(lerp2([](const Color& c) { return static_cast<float>(c.r); }));
                o.g = ClampU8(lerp2([](const Color& c) { return static_cast<float>(c.g); }));
                o.b = ClampU8(lerp2([](const Color& c) { return static_cast<float>(c.b); }));
                o.a = ClampU8(lerp2([](const Color& c) { return static_cast<float>(c.a); }));
                return o;
            }
        };

        // Scanline-fills `a_segs` (nonzero winding) into `a_img`, sampling the
        // per-pixel color from `a_sampler`. Antialiasing: 4 vertical subrows
        // per pixel row + analytic horizontal span coverage.
        void FillSegments(Image& a_img, const std::vector<Seg>& a_segs, const StyleSampler& a_sampler) {
            if (a_segs.empty()) {
                return;
            }
            constexpr int SUB = 4;
            const int W = a_img.width, H = a_img.height;

            // Bounding rows of the geometry
            float minY = 1e30f, maxY = -1e30f;
            for (const Seg& s : a_segs) {
                minY = std::min({ minY, s.y0, s.y1 });
                maxY = std::max({ maxY, s.y0, s.y1 });
            }
            int rowStart = std::max(0, static_cast<int>(std::floor(minY)));
            int rowEnd = std::min(H - 1, static_cast<int>(std::ceil(maxY)));

            std::vector<float> cov(static_cast<std::size_t>(W));
            struct Crossing {
                float x;
                int dir;
            };
            std::vector<Crossing> xs;

            for (int row = rowStart; row <= rowEnd; ++row) {
                std::fill(cov.begin(), cov.end(), 0.0f);
                bool any = false;

                for (int k = 0; k < SUB; ++k) {
                    const float sy = row + (k + 0.5f) / SUB;
                    xs.clear();
                    for (const Seg& s : a_segs) {
                        // Half-open interval rule avoids double-count at joins.
                        if ((s.y0 <= sy && s.y1 > sy) || (s.y1 <= sy && s.y0 > sy)) {
                            const float t = (sy - s.y0) / (s.y1 - s.y0);
                            xs.push_back({ s.x0 + (s.x1 - s.x0) * t, s.y1 > s.y0 ? 1 : -1 });
                        }
                    }
                    if (xs.empty()) {
                        continue;
                    }
                    std::sort(xs.begin(), xs.end(), [](const Crossing& a, const Crossing& b) { return a.x < b.x; });

                    int winding = 0;
                    for (std::size_t i = 0; i < xs.size(); ++i) {
                        const int prev = winding;
                        winding += xs[i].dir;
                        if (prev == 0 && winding != 0) {
                            // Span opens at xs[i].x — find where it closes.
                            const float xa = xs[i].x;
                            float xb = xa;
                            std::size_t j = i + 1;
                            int w2 = winding;
                            for (; j < xs.size(); ++j) {
                                w2 += xs[j].dir;
                                if (w2 == 0) {
                                    xb = xs[j].x;
                                    break;
                                }
                            }
                            if (j >= xs.size()) {
                                xb = static_cast<float>(W);
                            }
                            winding = 0;
                            i = j;

                            // Accumulate analytic horizontal coverage
                            const float cxa = std::max(0.0f, xa);
                            const float cxb = std::min(static_cast<float>(W), xb);
                            if (cxb > cxa) {
                                any = true;
                                int p0 = static_cast<int>(std::floor(cxa));
                                int p1 = static_cast<int>(std::ceil(cxb)) - 1;
                                for (int p = p0; p <= p1 && p < W; ++p) {
                                    const float l = std::max(cxa, static_cast<float>(p));
                                    const float r = std::min(cxb, static_cast<float>(p + 1));
                                    if (r > l) {
                                        cov[p] += (r - l) / SUB;
                                    }
                                }
                            }
                        }
                    }
                }

                if (!any) {
                    continue;
                }

                // Composite the covered pixels of this row (src-over).
                std::uint8_t* dstRow = a_img.rgba.data() + static_cast<std::size_t>(row) * W * 4;
                for (int x = 0; x < W; ++x) {
                    const float c = cov[x];
                    if (c <= 0.001f) {
                        continue;
                    }
                    const Color col = a_sampler.Sample(x + 0.5f, row + 0.5f);
                    const float srcA = (col.a / 255.0f) * std::min(c, 1.0f);
                    if (srcA <= 0.0f) {
                        continue;
                    }
                    std::uint8_t* d = dstRow + x * 4;
                    const float dstA = d[3] / 255.0f;
                    const float outA = srcA + dstA * (1.0f - srcA);
                    if (outA <= 0.0f) {
                        continue;
                    }
                    const float inv = 1.0f / outA;
                    d[0] = ClampU8((col.r * srcA + d[0] * dstA * (1.0f - srcA)) * inv);
                    d[1] = ClampU8((col.g * srcA + d[1] * dstA * (1.0f - srcA)) * inv);
                    d[2] = ClampU8((col.b * srcA + d[2] * dstA * (1.0f - srcA)) * inv);
                    d[3] = ClampU8(outA * 255.0f);
                }
            }
        }

        // Builds the stroke outline of a polyline as segment soup: one quad per
        // segment plus an octagon at every vertex (round-ish joins and caps).
        // Nonzero winding merges the overlaps into a solid stroke.
        void BuildStrokeOutline(const std::vector<Seg>& a_center, float a_halfW, std::vector<Seg>& a_out) {
            auto addQuad = [&](float ax, float ay, float bx, float by, float cx2, float cy2, float dx2, float dy2) {
                a_out.push_back({ ax, ay, bx, by });
                a_out.push_back({ bx, by, cx2, cy2 });
                a_out.push_back({ cx2, cy2, dx2, dy2 });
                a_out.push_back({ dx2, dy2, ax, ay });
            };
            constexpr int OCT = 8;
            auto addDisc = [&](float cx2, float cy2) {
                float pxs[OCT], pys[OCT];
                for (int i = 0; i < OCT; ++i) {
                    const float ang = 6.2831853f * i / OCT;
                    pxs[i] = cx2 + a_halfW * std::cos(ang);
                    pys[i] = cy2 + a_halfW * std::sin(ang);
                }
                for (int i = 0; i < OCT; ++i) {
                    const int j = (i + 1) % OCT;
                    a_out.push_back({ pxs[i], pys[i], pxs[j], pys[j] });
                }
            };

            bool firstPoint = true;
            float lastX = 0.0f, lastY = 0.0f;
            for (const Seg& s : a_center) {
                const float dx = s.x1 - s.x0, dy = s.y1 - s.y0;
                const float len = std::sqrt(dx * dx + dy * dy);
                if (len > 1e-4f) {
                    const float nx = -dy / len * a_halfW;
                    const float ny = dx / len * a_halfW;
                    // Counter-clockwise quad so all quads share a winding sign.
                    addQuad(s.x0 + nx, s.y0 + ny, s.x1 + nx, s.y1 + ny,
                            s.x1 - nx, s.y1 - ny, s.x0 - nx, s.y0 - ny);
                }
                // Discs at each vertex (joins between segments + caps at ends;
                // duplicates at shared vertices are harmless under nonzero fill)
                if (firstPoint || s.x0 != lastX || s.y0 != lastY) {
                    addDisc(s.x0, s.y0);
                }
                addDisc(s.x1, s.y1);
                lastX = s.x1;
                lastY = s.y1;
                firstPoint = false;
            }
        }

        // Rasterizes a list of draw items into a W x H image. Item matrices
        // already map character twips to raster pixels.
        Image RasterizeItems(const Movie::Impl& a_impl, const std::vector<DrawItem>& a_items, int a_w, int a_h) {
            Image img;
            img.width = a_w;
            img.height = a_h;
            img.rgba.assign(static_cast<std::size_t>(a_w) * a_h * 4, 0);

            std::vector<Seg> segs;
            std::vector<Seg> stroke;

            SWF_TRACE("[swfvec] rasterize %zu items into %dx%d\n", a_items.size(), a_w, a_h);
            for (const DrawItem& item : a_items) {
                SWF_TRACE("[swfvec]   item: %zu layers, matrix[%.3f %.3f %.3f %.3f | %.1f %.1f]\n",
                          item.shape->layers.size(), item.toRaster.sx, item.toRaster.r0, item.toRaster.r1,
                          item.toRaster.sy, item.toRaster.tx, item.toRaster.ty);
                for (const Layer& layer : item.shape->layers) {
                    SWF_TRACE("[swfvec]     layer: %zu fills, %zu lines, %zu paths\n",
                              layer.fills.size(), layer.lines.size(), layer.paths.size());
                    // ---- Fills, in style order ----
                    for (int s = 1; s <= static_cast<int>(layer.fills.size()); ++s) {
                        segs.clear();
                        for (const SubPath& path : layer.paths) {
                            if (path.f0 != s && path.f1 != s) {
                                continue;
                            }
                            std::vector<Seg> flat;
                            FlattenSubPath(path, item.toRaster, flat);
                            if (path.f0 == s) {
                                // fill0 is the LEFT fill: reversed orientation
                                // relative to fill1 so windings cancel where
                                // both sides use the same style.
                                for (const Seg& sg : flat) {
                                    segs.push_back({ sg.x1, sg.y1, sg.x0, sg.y0 });
                                }
                            }
                            if (path.f1 == s) {
                                segs.insert(segs.end(), flat.begin(), flat.end());
                            }
                        }
                        if (segs.empty()) {
                            continue;
                        }

                        const FillStyle& fs = layer.fills[s - 1];
                        SWF_TRACE("[swfvec]     fill %d: type=%d segs=%zu color=%u,%u,%u,%u bmp=%u\n",
                                  s, static_cast<int>(fs.type), segs.size(), fs.color.r, fs.color.g,
                                  fs.color.b, fs.color.a, fs.bitmapId);
                        StyleSampler sampler;
                        sampler.fill = &fs;
                        sampler.cxform = item.cxform;
                        if (fs.type == FillStyle::Type::Bitmap) {
                            if (auto it = a_impl.bitmaps.find(fs.bitmapId); it != a_impl.bitmaps.end()) {
                                sampler.bitmap = &it->second;
                            }
                        }
                        // raster px -> char twips -> fill space
                        sampler.rasterToFill = fs.matrix.Invert().Concat(item.toRaster.Invert());
                        FillSegments(img, segs, sampler);
                    }

                    // ---- Strokes, above the fills of this layer ----
                    for (int s = 1; s <= static_cast<int>(layer.lines.size()); ++s) {
                        segs.clear();
                        for (const SubPath& path : layer.paths) {
                            if (path.ln == s) {
                                FlattenSubPath(path, item.toRaster, segs);
                            }
                        }
                        if (segs.empty()) {
                            continue;
                        }
                        const LineStyle& ls = layer.lines[s - 1];
                        // Stroke width scales with the transform (toRaster's
                        // scale is px-per-twip); hairlines (width 0) and thin
                        // strokes clamp to 1 px on screen.
                        const float wPx = std::max(1.0f, ls.width * item.toRaster.MaxScale());
                        stroke.clear();
                        BuildStrokeOutline(segs, wPx * 0.5f, stroke);

                        FillStyle solid;
                        solid.type = FillStyle::Type::Solid;
                        solid.color = ls.color;
                        StyleSampler sampler;
                        sampler.fill = &solid;
                        sampler.cxform = item.cxform;
                        FillSegments(img, stroke, sampler);
                    }
                }
            }
            return img;
        }

    }  // namespace

    // =====================================================================
    // Movie public interface
    // =====================================================================

    Movie::Movie(std::unique_ptr<Impl> a_impl) :
        _impl(std::move(a_impl)) {}

    Movie::~Movie() = default;

    float Movie::WidthPx() const {
        return (_impl->contentBounds.xmax - _impl->contentBounds.xmin) / 20.0f;
    }

    float Movie::HeightPx() const {
        return (_impl->contentBounds.ymax - _impl->contentBounds.ymin) / 20.0f;
    }

    float Movie::OriginXPx() const {
        return _impl->contentBounds.xmin / 20.0f;
    }

    float Movie::OriginYPx() const {
        return _impl->contentBounds.ymin / 20.0f;
    }

    float Movie::FrameRate() const {
        return _impl->frameRate > 0.5f ? _impl->frameRate : 12.0f;
    }

    int Movie::FrameCount() const {
        return _impl->root.frames.empty() ? 1 : static_cast<int>(_impl->root.frames.size());
    }

    bool Movie::IsAnimated() const {
        return _impl->animated;
    }

    const std::vector<std::unique_ptr<RasterChar>>& Movie::Rasters() const {
        return _impl->rasters;
    }

    std::optional<SWFParse::Image> Movie::RenderFrame(int a_frame, float a_scale) const {
        const auto& impl = *_impl;
        const float wPx = WidthPx();
        const float hPx = HeightPx();
        if (wPx <= 0.0f || hPx <= 0.0f) {
            return std::nullopt;
        }
        float scale = a_scale > 0.0f ? a_scale : 1.0f;
        // Cap output dimensions.
        constexpr float kMaxDim = 4096.0f;
        scale = std::min({ scale, kMaxDim / wPx, kMaxDim / hPx });

        const int W = std::max(1, static_cast<int>(std::ceil(wPx * scale)));
        const int H = std::max(1, static_cast<int>(std::ceil(hPx * scale)));

        // Movie twips -> raster px (origin at content bounds min).
        Matrix toRaster;
        toRaster.sx = toRaster.sy = scale / 20.0f;
        toRaster.tx = -impl.contentBounds.xmin * scale / 20.0f;
        toRaster.ty = -impl.contentBounds.ymin * scale / 20.0f;

        std::vector<DrawItem> items;
        impl.WalkFrame(impl.root, a_frame, toRaster, ColorTransform{}, 8,
                       [&](const Shape& a_shape, std::uint16_t, const Matrix& a_m, const ColorTransform& a_cx) {
                           items.push_back({ &a_shape, a_m, a_cx });
                       });
        if (items.empty()) {
            return std::nullopt;
        }
        return RasterizeItems(impl, items, W, H);
    }

    void Movie::BuildFrameDraws(int a_frame, std::vector<FrameDraw>& a_out) const {
        const auto& impl = *_impl;
        a_out.clear();

        // Movie twips -> movie px space (origin at content bounds min).
        Matrix toMovie;
        toMovie.sx = toMovie.sy = 1.0f / 20.0f;
        toMovie.tx = -impl.contentBounds.xmin / 20.0f;
        toMovie.ty = -impl.contentBounds.ymin / 20.0f;

        impl.WalkFrame(impl.root, a_frame, toMovie, ColorTransform{}, 8,
                       [&](const Shape&, std::uint16_t a_charId, const Matrix& a_m, const ColorTransform& a_cx) {
                           auto it = impl.rasterById.find(a_charId);
                           if (it == impl.rasterById.end()) {
                               return;
                           }
                           const RasterChar* rc = it->second;
                           FrameDraw fd;
                           fd.image = rc;
                           const float x0t = rc->originX;
                           const float y0t = rc->originY;
                           const float x1t = rc->originX + rc->image.width * rc->twipsPerPixel;
                           const float y1t = rc->originY + rc->image.height * rc->twipsPerPixel;
                           a_m.Apply(x0t, y0t, fd.x0, fd.y0);
                           a_m.Apply(x1t, y0t, fd.x1, fd.y1);
                           a_m.Apply(x1t, y1t, fd.x2, fd.y2);
                           a_m.Apply(x0t, y1t, fd.x3, fd.y3);
                           auto clamp01 = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
                           fd.mulR = clamp01(a_cx.mulR);
                           fd.mulG = clamp01(a_cx.mulG);
                           fd.mulB = clamp01(a_cx.mulB);
                           fd.mulA = clamp01(a_cx.mulA);
                           a_out.push_back(fd);
                       });
    }

    // =====================================================================
    // Loading
    // =====================================================================

    std::shared_ptr<Movie> Load(const std::filesystem::path& a_swfPath, const std::string& a_className) {
        std::vector<std::uint8_t> body;
        if (!ReadMovieBody(a_swfPath.string(), body)) {
            return nullptr;
        }

        auto impl = std::make_unique<Movie::Impl>();

        Reader r(body.data(), body.size());
        impl->stage = r.ReadRect();
        impl->frameRate = r.U16() / 256.0f;
        r.U16();  // frame count (header)

        std::unordered_map<std::string, std::uint16_t> symbolMap;
        Timeline mainTimeline;
        std::map<int, Placement> mainState;

        while (!r.Eof()) {
            const std::uint16_t rh = r.U16();
            const std::uint16_t tag = static_cast<std::uint16_t>(rh >> 6);
            std::uint32_t len = rh & 0x3F;
            if (len == 0x3F) {
                len = r.U32();
            }
            const std::size_t tagStart = r.Pos();
            const std::size_t tagEnd = std::min(tagStart + len, body.size());
            if (tag == kTagEnd) {
                break;
            }

            switch (tag) {
            case kTagShowFrame:
                SnapshotFrame(mainState, mainTimeline);
                break;

            case kTagPlaceObject:
            case kTagPlaceObject2:
            case kTagPlaceObject3:
            case kTagRemoveObject:
            case kTagRemoveObject2:
                ApplyControlTag(tag, r, tagEnd, mainState);
                break;

            case kTagSymbolClass: {
                const std::uint16_t numSymbols = r.U16();
                for (std::uint16_t i = 0; i < numSymbols && r.Pos() < tagEnd; ++i) {
                    const std::uint16_t cid = r.U16();
                    std::string name = r.CStr();
                    if (!name.empty()) {
                        symbolMap[name] = cid;
                    }
                }
                break;
            }

            case kTagDefineShape:
            case kTagDefineShape2:
            case kTagDefineShape3:
            case kTagDefineShape4: {
                const int ver = (tag == kTagDefineShape) ? 1 : (tag == kTagDefineShape2) ? 2 : (tag == kTagDefineShape3) ? 3 : 4;
                const std::uint16_t sid = r.U16();
                Shape shape;
                if (ParseShape(r, ver, tagEnd, shape)) {
                    impl->shapes[sid] = std::move(shape);
                } else {
                    SWF_TRACE("[swfvec] shape %u: unsupported fill/parse desync, skipped\n", sid);
                }
                break;
            }

            case kTagDefineSprite: {
                const std::uint16_t sid = r.U16();
                r.U16();  // declared frame count
                Timeline tl;
                ParseSpriteTimeline(r, tagEnd, tl);
                impl->sprites[sid] = std::move(tl);
                break;
            }

            case kTagDefineBitsLossless:
            case kTagDefineBitsLossless2: {
                BitmapRecord rec;
                rec.tagCode = tag;
                const std::uint16_t cid = r.U16();
                rec.format = r.U8();
                rec.width = r.U16();
                rec.height = r.U16();
                if (rec.format == 3) {
                    rec.colorTableCount = static_cast<std::uint16_t>(r.U8()) + 1;
                }
                rec.dataOffset = r.Pos();
                rec.dataLen = tagEnd > rec.dataOffset ? tagEnd - rec.dataOffset : 0;
                if (rec.width > 0 && rec.height > 0) {
                    impl->bitmapRecs[cid] = rec;
                }
                break;
            }

            case kTagDefineBitsJPEG2:
            case kTagDefineBitsJPEG3:
            case kTagDefineBitsJPEG4: {
                BitmapRecord rec;
                rec.tagCode = tag;
                const std::uint16_t cid = r.U16();
                if (tag != kTagDefineBitsJPEG2) {
                    rec.jpegAlphaOffset = r.U32();
                    if (tag == kTagDefineBitsJPEG4) {
                        r.U16();  // deblock param
                    }
                }
                rec.dataOffset = r.Pos();
                rec.dataLen = tagEnd > rec.dataOffset ? tagEnd - rec.dataOffset : 0;
                impl->bitmapRecs[cid] = rec;
                break;
            }

            default:
                break;
            }

            r.Seek(tagEnd);
        }

        // ---- Resolve the requested symbol to a root timeline ------------
        auto classId = [&](const std::string& n) -> int {
            auto it = symbolMap.find(n);
            return it != symbolMap.end() ? it->second : -1;
        };

        int startId = -1;
        if (!a_className.empty()) {
            startId = classId(a_className);
            if (startId < 0) {
                startId = classId(a_className + "_bitmap");
            }
        }

        if (startId >= 0) {
            const std::uint16_t cid = static_cast<std::uint16_t>(startId);
            if (auto s = impl->sprites.find(cid); s != impl->sprites.end()) {
                impl->root = s->second;
            } else if (impl->shapes.count(cid)) {
                // Synthetic single frame placing the shape at identity.
                TFrame f;
                Placement p;
                p.charId = cid;
                f.list.emplace_back(1, p);
                impl->root.frames.push_back(std::move(f));
            } else {
                // Plain bitmap or unsupported character — the bitmap
                // extractor handles those better.
                return nullptr;
            }
        } else {
            // No/unknown class: use the file's own main timeline (logo.swf
            // pattern — the stage is the artwork).
            impl->root = std::move(mainTimeline);
        }

        if (impl->root.frames.empty()) {
            return nullptr;
        }

        // ---- Content bounds + animation flag + per-char max scale --------
        Rect bounds;
        bool boundsInit = false;
        bool anySpriteAnimated = false;
        std::unordered_map<std::uint16_t, float> maxScaleByChar;

        // Nested sprites loop independently of the root (frame = global %
        // length), so sampling only the root's frame indices would miss the
        // extents of longer sprite animations. Sample enough global frames to
        // touch every frame of every timeline at least once (capped).
        std::size_t frameSamples = impl->root.frames.size();
        for (const auto& [sid, tl] : impl->sprites) {
            frameSamples = std::max(frameSamples, tl.frames.size());
        }
        frameSamples = std::min<std::size_t>(frameSamples, 512);

        for (std::size_t f = 0; f < frameSamples; ++f) {
            impl->WalkFrame(impl->root, static_cast<int>(f), Matrix{}, ColorTransform{}, 8,
                            [&](const Shape& a_shape, std::uint16_t a_charId, const Matrix& a_m, const ColorTransform&) {
                                // Track scale for raster resolution decisions.
                                float& ms = maxScaleByChar[a_charId];
                                ms = std::max(ms, a_m.MaxScale());
                                // Transform the shape bounds' corners.
                                const float xs[2] = { static_cast<float>(a_shape.bounds.xmin), static_cast<float>(a_shape.bounds.xmax) };
                                const float ys[2] = { static_cast<float>(a_shape.bounds.ymin), static_cast<float>(a_shape.bounds.ymax) };
                                for (int cx = 0; cx < 2; ++cx) {
                                    for (int cy = 0; cy < 2; ++cy) {
                                        float ox, oy;
                                        a_m.Apply(xs[cx], ys[cy], ox, oy);
                                        const int ix0 = static_cast<int>(std::floor(ox));
                                        const int iy0 = static_cast<int>(std::floor(oy));
                                        if (!boundsInit) {
                                            bounds.xmin = bounds.xmax = ix0;
                                            bounds.ymin = bounds.ymax = iy0;
                                            boundsInit = true;
                                        } else {
                                            bounds.xmin = std::min(bounds.xmin, ix0);
                                            bounds.xmax = std::max(bounds.xmax, ix0);
                                            bounds.ymin = std::min(bounds.ymin, iy0);
                                            bounds.ymax = std::max(bounds.ymax, iy0);
                                        }
                                    }
                                }
                            });
        }
        if (!boundsInit || bounds.xmax <= bounds.xmin || bounds.ymax <= bounds.ymin) {
            return nullptr;  // nothing drawable (e.g. AS3-only content)
        }
        impl->contentBounds = bounds;

        // A nested sprite with >1 frame animates even when the root has one.
        for (const auto& frame : impl->root.frames) {
            for (const auto& [depth, p] : frame.list) {
                std::unordered_set<std::uint16_t> visited;
                std::vector<std::uint16_t> stack{ p.charId };
                while (!stack.empty()) {
                    const std::uint16_t id = stack.back();
                    stack.pop_back();
                    if (!visited.insert(id).second) {
                        continue;
                    }
                    if (auto s = impl->sprites.find(id); s != impl->sprites.end()) {
                        if (s->second.frames.size() > 1) {
                            anySpriteAnimated = true;
                        }
                        for (const auto& sf : s->second.frames) {
                            for (const auto& [d2, p2] : sf.list) {
                                stack.push_back(p2.charId);
                            }
                        }
                    }
                }
            }
        }
        impl->animated = impl->root.frames.size() > 1 || anySpriteAnimated;

        // ---- Decode the bitmaps referenced by shape fills -----------------
        for (const auto& [sid, shape] : impl->shapes) {
            for (const Layer& layer : shape.layers) {
                for (const FillStyle& fs : layer.fills) {
                    if (fs.type == FillStyle::Type::Bitmap && fs.bitmapId != 0xFFFF &&
                        !impl->bitmaps.count(fs.bitmapId)) {
                        if (auto it = impl->bitmapRecs.find(fs.bitmapId); it != impl->bitmapRecs.end()) {
                            Image img;
                            if (DecodeBitmap(it->second, body.data(), img)) {
                                impl->bitmaps[fs.bitmapId] = std::move(img);
                            }
                        }
                    }
                }
            }
        }

        // ---- Pre-rasterize characters for the animated path ---------------
        if (impl->animated) {
            for (const auto& [cid, scale] : maxScaleByChar) {
                auto it = impl->shapes.find(cid);
                if (it == impl->shapes.end()) {
                    continue;
                }
                const Shape& shape = it->second;
                const float wTw = static_cast<float>(shape.bounds.xmax - shape.bounds.xmin);
                const float hTw = static_cast<float>(shape.bounds.ymax - shape.bounds.ymin);
                if (wTw <= 0.0f || hTw <= 0.0f) {
                    continue;
                }
                // Raster at the largest placed scale (x1.5 headroom for the
                // final on-screen upscale), capped at 2048 px per side.
                float pxPerTwip = std::max(scale, 0.01f) * 1.5f / 20.0f;
                pxPerTwip = std::min({ pxPerTwip, 2048.0f / wTw, 2048.0f / hTw });
                const int W = std::max(1, static_cast<int>(std::ceil(wTw * pxPerTwip)));
                const int H = std::max(1, static_cast<int>(std::ceil(hTw * pxPerTwip)));

                Matrix toRaster;
                toRaster.sx = toRaster.sy = pxPerTwip;
                toRaster.tx = -shape.bounds.xmin * pxPerTwip;
                toRaster.ty = -shape.bounds.ymin * pxPerTwip;

                std::vector<DrawItem> items{ { &shape, toRaster, ColorTransform{} } };
                auto rc = std::make_unique<RasterChar>();
                rc->charId = cid;
                rc->image = RasterizeItems(*impl, items, W, H);
                rc->originX = static_cast<float>(shape.bounds.xmin);
                rc->originY = static_cast<float>(shape.bounds.ymin);
                rc->twipsPerPixel = 1.0f / pxPerTwip;
                impl->rasterById[cid] = rc.get();
                impl->rasters.push_back(std::move(rc));
            }
        }

        SWF_TRACE("[swfvec] '%s' in %s: %zu shapes, %zu sprites, %d frames, %s, bounds %.0fx%.0f px\n",
                  a_className.c_str(), a_swfPath.string().c_str(), impl->shapes.size(), impl->sprites.size(),
                  static_cast<int>(impl->root.frames.size()), impl->animated ? "ANIMATED" : "static",
                  (bounds.xmax - bounds.xmin) / 20.0f, (bounds.ymax - bounds.ymin) / 20.0f);

        return std::shared_ptr<Movie>(new Movie(std::move(impl)));
    }

}  // namespace SWFVectorMovie
