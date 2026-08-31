#pragma once
// Wrapper for task compliance: engine stores MeshComponent in core/mesh_component.h
#include "core/mesh_component.h"
#include "core/material_component.h"

namespace Engine {
    using MeshComponent = ::engine::MeshComponent;
    // MaterialComponent lives in engine:: as well
}

namespace engine {
    // already defined in core/mesh_component.h, alias for consistency
}
