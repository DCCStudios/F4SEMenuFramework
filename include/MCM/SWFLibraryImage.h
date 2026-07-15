#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// Extracts embedded bitmap images from an MCM Flash library (lib.swf).
//
// Real MCM "image" controls do not point at loose image files — they reference
// an exported Flash symbol (className) inside Data/MCM/Config/<libName>/lib.swf.
// The game normally renders these through Scaleform, but our overlay is ImGui,
// so we cannot use the game's Flash renderer. Instead we parse the SWF ourselves
// and pull out the embedded raster bitmaps (DefineBitsLossless/Lossless2 and,
// best-effort, DefineBitsJPEG2/3/4). For the banners/logos real MCM mods ship,
// the embedded bitmap *is* the whole visible image, so this reproduces what the
// player sees in the native menu. Pure vector- or ActionScript-drawn symbols
// have no extractable bitmap; those fall back to a placeholder in the renderer.
//
// This module is intentionally free of any D3D/ImGui/CommonLibF4 dependency
// (only the C++ standard library + zlib) so it can be unit-tested standalone.
namespace SWFLibraryImage {

    // A decoded 32-bit image. Pixels are straight (non-premultiplied) alpha in
    // R,G,B,A byte order, row-major, tightly packed (stride == width * 4).
    struct DecodedImage {
        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> rgba;  // size == width * height * 4
    };

    // Resolves the exported class `a_className` inside the SWF at `a_swfPath`
    // to a single embedded bitmap and decodes it:
    //   * If the class is itself a bitmap symbol, that bitmap is returned.
    //   * If it is a sprite/shape, the largest bitmap reachable from it (via its
    //     placed shapes' bitmap fills) is returned.
    //   * If the class cannot be resolved to any bitmap, the largest bitmap in
    //     the whole file is returned as a best-effort fallback.
    // Returns std::nullopt only when the SWF cannot be read/parsed or contains
    // no decodable bitmaps at all.
    std::optional<DecodedImage> Extract(const std::filesystem::path& a_swfPath,
                                        const std::string& a_className);

}  // namespace SWFLibraryImage
