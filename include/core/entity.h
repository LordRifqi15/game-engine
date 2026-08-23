#pragma once

#include <cstdint>

// Entity = pure ID. No data, no struct.
namespace engine {

using Entity = uint32_t;
inline constexpr Entity kInvalidEntity = 0xFFFFFFFFu;

} // namespace engine
