#pragma once

#include <d3d11.h>
#include "imgui.h"
#include "nanosvg.h"
class TextureLoader {
private:

    class TextureLoaderImpl {
        static inline ID3D11Device* device = NULL;
        static inline ID3D11DeviceContext* context = NULL;
        static ID3D11ShaderResourceView* ReadSVG(NSVGimage* image, ImVec2 size);
    public:

        static void Init(ID3D11Device* device, ID3D11DeviceContext* context);
        static ID3D11ShaderResourceView* LoadTextureFromDDSFile(std::string path);
        static ID3D11ShaderResourceView* LoadTextureFromWICFile(std::string path);
        static ID3D11ShaderResourceView* LoadTextureFromSVGFile(std::string path, ImVec2 size);
    };
    static ImTextureID LoadDDS(std::string imagePath);
    static ImTextureID LoadWIC(std::string imagePath);
    static ImTextureID LoadSVG(std::string imagePath, ImVec2 size);
    static ImTextureID LoadTextureAny(std::string imagePath, ImVec2 size);

    static inline std::map<std::string, ImTextureID> textures;

public:
    static void DisposeTexture(std::string path);
    static ImTextureID GetTexture(std::string path, ImVec2 size = {0,0});
    static void Init(ID3D11Device* device, ID3D11DeviceContext* context);
};
