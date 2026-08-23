#pragma once

#include "core/material.h"
#include "core/mesh.h"

#include <string>
#include <vector>

namespace engine {

// One renderable piece of a GLTF mesh (a primitive), with its material.
struct GLTFPrimitive {
    Mesh mesh;
    Material material;
};

// Loads a GLTF 2.0 file. Each mesh primitive becomes one GLTFPrimitive
// (positions, normals, uvs, indices, baseColor factor/texture).
// Textures referenced by materials are loaded via texturePathResolver callback —
// the renderer layer owns actual GPU upload, so we return image URIs.
struct GLTFModel {
    std::vector<GLTFPrimitive> primitives;
    // image URI -> loaded for later texture binding by the renderer layer
    bool ok = false;
    std::string error;
};

class TextureCache;

// textureCache may be null: textures then stay unresolved (color-only).
std::vector<GLTFPrimitive> loadGLTF(const std::string& path, TextureCache* textures);

} // namespace engine
