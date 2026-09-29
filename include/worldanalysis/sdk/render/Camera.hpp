#pragma once

#include <worldanalysis/sdk/Types.hpp>
#include <cstddef>

namespace worldanalysis::sdk::camera {

// Layout verified against the current Bedrock client camera type. Matrix stacks
// occupy 0x40 bytes each, followed by inverse-view/basis and position.
inline constexpr std::size_t ViewMatrixStack = 0x000;
inline constexpr std::size_t InverseViewMatrix = 0x0C0;
inline constexpr std::size_t Position = 0x124;

using MatrixStackGetTopFn = void* (*)(void* matrixStack);
using UpdateViewDependenciesFn = void (*)(void* camera);

MatrixStackGetTopFn resolveMatrixStackGetTop();
UpdateViewDependenciesFn resolveUpdateViewDependencies();

inline Vec3& position(void* camera) {
    return *reinterpret_cast<Vec3*>(reinterpret_cast<unsigned char*>(camera) + Position);
}

inline void* viewMatrixStack(void* camera) {
    return reinterpret_cast<unsigned char*>(camera) + ViewMatrixStack;
}

} // namespace worldanalysis::sdk::camera
