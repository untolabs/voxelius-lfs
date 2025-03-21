// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "mathlib/constexpr.hh"
#include "mathlib/vec2f.hh"
#include "mathlib/vec3f.hh"
#include "shared/world/voxel_def.hh"

// [0] XXXXXXXXYYYYYYYYZZZZZZZZWWWWHHHH
// [1] FFFFTTTTTTTTTTTAAAAA------------
using ChunkQuad = std::array<std::uint32_t, 2>;

constexpr inline static ChunkQuad make_chunk_quad(const Vec3f &position, const Vec2f &size, VoxelFacing facing, std::size_t texture, std::size_t frames)
{
    ChunkQuad result = {};
    result[0] = 0x00000000;
    result[1] = 0x00000000;

    // [0] XXXXXXXXYYYYYYYYZZZZZZZZ--------
    result[0] |= (0x000000FFU & static_cast<std::uint32_t>(position[0] * 16.0f)) << 24U;
    result[0] |= (0x000000FFU & static_cast<std::uint32_t>(position[1] * 16.0f)) << 16U;
    result[0] |= (0x000000FFU & static_cast<std::uint32_t>(position[2] * 16.0f)) << 8U;

    // [0] ------------------------WWWWHHHH
    result[0] |= (0x0000000FU & static_cast<std::uint32_t>(size[0] * 16.0f - 1.0f)) << 4U;
    result[0] |= (0x0000000FU & static_cast<std::uint32_t>(size[1] * 16.0f - 1.0f));

    // [1] FFFF----------------------------
    result[1] |= (0x0000000FU & static_cast<std::uint32_t>(facing)) << 28U;

    // [1] ----TTTTTTTTTTTAAAAA------------
    result[1] |= (0x000007FFU & static_cast<std::uint32_t>(texture)) << 17U;
    result[1] |= (0x0000001FU & static_cast<std::uint32_t>(frames)) << 12U;

    return result;
}
