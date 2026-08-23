#pragma once

#include "core/mesh.h"

#include <string>

namespace engine {

// Loads a Wavefront OBJ into engine Vertex format (position, normal, uv).
// Missing UVs/normals get defaults ((0,0) / face-generated normals).
Mesh loadOBJ(const std::string& path);

} // namespace engine
