#pragma once

#include <cstdint>

namespace worldanalysis::sdk {

struct Vec2 { float x; float y; };
struct Vec3 { float x; float y; float z; };
struct BlockPos { int x; int y; int z; };
struct AABB { Vec3 min; Vec3 max; };
struct Color { float r; float g; float b; float a; };

}
