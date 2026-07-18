#pragma once

// Vector-art and timeline-animation renderer for MCM Flash symbols.
//
// SWFLibraryImage covers symbols whose visible content is an embedded raster
// bitmap. This module covers the rest: symbols drawn with Flash vector shapes
// (DefineShape1-4 — solid / gradient / bitmap fills and line strokes) and
// symbols animated on the timeline (PlaceObject matrices per frame).
//
// Two rendering modes:
//   * STATIC  — the whole display tree of one frame is flattened and
//               rasterized on the CPU into a single RGBA image (crisp, done
//               once, uploaded as one texture).
//   * ANIMATED — every shape character is rasterized once; each frame is then
//               described as a list of textured quads (transform + tint) that
//               the ImGui side composites. Tween animations (matrix / alpha
//               per frame) replay exactly; ActionScript-driven animation is
//               out of scope (needs a Flash VM — see the dev reference §39).
//
// Deliberate non-goals (log-and-skip, never fail the whole movie):
//   * DefineMorphShape, DefineText/DefineFont glyph rendering, filters,
//     blend modes other than normal, clipping masks (clipDepth).
//
// Like SWFParseCommon, this module is std-only so the swftest harness can
// exercise it against real mod SWFs outside the game.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "MCM/SWFParseCommon.h"

namespace SWFVectorMovie {

    // A shape character pre-rasterized for the animated path. The image covers
    // the character's bounds; the fields map image pixels back to the
    // character's own twip coordinate system.
    struct RasterChar {
        std::uint16_t charId = 0;
        SWFParse::Image image;
        float originX = 0.0f;        // twip x of image pixel (0,0)
        float originY = 0.0f;        // twip y of image pixel (0,0)
        float twipsPerPixel = 20.0f; // image resolution
    };

    // One textured quad of an animated frame, in movie pixel space (origin at
    // the movie bounds' top-left, 1 unit = 1/20 twip). Corners are ordered
    // p0 = image top-left, p1 = top-right, p2 = bottom-right, p3 = bottom-left.
    struct FrameDraw {
        const RasterChar* image = nullptr;
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        float x2 = 0, y2 = 0, x3 = 0, y3 = 0;
        // Color-transform multipliers, clamped to 0..1 (additive terms are not
        // representable as an ImGui tint and are dropped).
        float mulR = 1, mulG = 1, mulB = 1, mulA = 1;
    };

    class Movie {
    public:
        ~Movie();

        // Content bounds (union over all root frames), in movie pixels.
        float WidthPx() const;
        float HeightPx() const;

        // Offset of the content bounds' top-left from the symbol's own
        // registration point, in movie pixels (usually <= 0). Lets callers
        // place rendered art exactly where Flash would place the sprite.
        float OriginXPx() const;
        float OriginYPx() const;

        float FrameRate() const;   // frames per second (>= 1)
        int FrameCount() const;    // root timeline length (>= 1)
        bool IsAnimated() const;   // more than one frame anywhere in the tree

        // STATIC path: flatten root frame `a_frame` into one RGBA image at
        // `a_scale` output pixels per movie pixel. Returns std::nullopt when
        // the frame has no drawable content.
        std::optional<SWFParse::Image> RenderFrame(int a_frame, float a_scale) const;

        // ANIMATED path: the pre-rasterized characters (build textures from
        // these once) ...
        const std::vector<std::unique_ptr<RasterChar>>& Rasters() const;

        // ... and the quad list for root frame `a_frame` (wraps modulo the
        // timeline length; nested sprite timelines loop independently).
        void BuildFrameDraws(int a_frame, std::vector<FrameDraw>& a_out) const;

        struct Impl;

    private:
        friend std::shared_ptr<Movie> Load(const std::filesystem::path&, const std::string&);
        explicit Movie(std::unique_ptr<Impl> a_impl);
        std::unique_ptr<Impl> _impl;
    };

    // Parses the SWF and resolves `a_className` (exported symbol) to a movie:
    //   * class is a sprite -> that sprite's timeline becomes the root;
    //   * class is a shape  -> a synthetic single-frame movie;
    //   * class not found / empty -> the file's own main timeline (covers
    //     logo.swf-style files whose stage IS the artwork);
    //   * class is a plain bitmap, or nothing drawable -> nullptr (callers
    //     fall back to SWFLibraryImage bitmap extraction).
    // For animated movies the reachable shape characters are rasterized
    // eagerly, so a returned movie is immediately usable on any thread.
    std::shared_ptr<Movie> Load(const std::filesystem::path& a_swfPath,
                                const std::string& a_className);

}  // namespace SWFVectorMovie
