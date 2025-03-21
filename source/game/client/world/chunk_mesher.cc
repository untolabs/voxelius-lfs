// SPDX-License-Identifier: BSD-2-Clause
#include "client/precompiled.hh"
#include "client/world/chunk_mesher.hh"

#include "common/crc64.hh"

#include "shared/entity/chunk.hh"

#include "shared/event/chunk_create.hh"
#include "shared/event/chunk_update.hh"
#include "shared/event/voxel_set.hh"

#include "shared/world/chunk_coord.hh"
#include "shared/world/local_coord.hh"
#include "shared/world/voxel_def.hh"
#include "shared/world/voxel_coord.hh"
#include "shared/world/world.hh"

#include "client/world/chunk_quad.hh"
#include "client/world/chunk_visibility.hh"
#include "client/world/voxel_atlas.hh"

#include "client/globals.hh"


using QuadBuilder = std::vector<ChunkQuad>;

using CachedChunkCoord = unsigned short;
constexpr static CachedChunkCoord CPOS_ITSELF = 0x0000;
constexpr static CachedChunkCoord CPOS_NORTH  = 0x0001;
constexpr static CachedChunkCoord CPOS_SOUTH  = 0x0002;
constexpr static CachedChunkCoord CPOS_EAST   = 0x0003;
constexpr static CachedChunkCoord CPOS_WEST   = 0x0004;
constexpr static CachedChunkCoord CPOS_TOP    = 0x0005;
constexpr static CachedChunkCoord CPOS_BOTTOM = 0x0006;
constexpr static const size_t NUM_CACHED_CPOS = 7;

struct WorkerContext final {
    std::array<VoxelStorage, NUM_CACHED_CPOS> cache {};
    std::vector<QuadBuilder> quads_nb {};
    std::vector<QuadBuilder> quads_b {};
    std::future<void> future {};
    bool is_cancelled {};
    ChunkCoord coord {};
};

static const CachedChunkCoord get_cached_cpos(const ChunkCoord &pivot, const ChunkCoord &cpos)
{
    static const CachedChunkCoord nx[3] = {CPOS_WEST, 0, CPOS_EAST};
    static const CachedChunkCoord ny[3] = {CPOS_BOTTOM, 0, CPOS_TOP};
    static const CachedChunkCoord nz[3] = {CPOS_NORTH, 0, CPOS_SOUTH};

    if(pivot != cpos) {
        ChunkCoord delta = pivot - cpos;
        delta[0] = cxpr::clamp<std::int64_t>(delta[0], -1, 1);
        delta[1] = cxpr::clamp<std::int64_t>(delta[1], -1, 1);
        delta[2] = cxpr::clamp<std::int64_t>(delta[2], -1, 1);

        if(delta[0])
            return nx[delta[0] + 1];
        if(delta[1])
            return ny[delta[1] + 1];
        return nz[delta[2] + 1];
    }

    return CPOS_ITSELF;
}

static bool vis_test(WorkerContext *ctx, VoxelID voxel, const VoxelInfo *info, const LocalCoord &lpos)
{
    const auto pvpos = ChunkCoord::to_voxel(ctx->coord, lpos);
    const auto pcpos = VoxelCoord::to_chunk(pvpos);
    const auto plpos = VoxelCoord::to_local(pvpos);
    const auto index = LocalCoord::to_index(plpos);

    const auto cached_cpos = get_cached_cpos(ctx->coord, pcpos);
    const auto &voxels = ctx->cache.at(cached_cpos);
    const auto neighbour = voxels[index];

    if(neighbour == NULL_VOXEL)
        return true;
    if(neighbour == voxel)
        return false;

    if(const VoxelInfo *neighbour_info = voxel_def::find(neighbour)) {
        if(neighbour_info->blending != info->blending) {
            // Voxel types that use blending are semi-transparent;
            // this means they're rendered using a different setup
            // and they must have visible faces with opaque voxels
            return neighbour_info->blending;
        }
    }

    return false;
}

static VoxelFacing get_facing(VoxelFace face, VoxelType type)
{
    if(type == VoxelType::Cross) {
        switch(face) {
            case VoxelFace::CrossNESW:  return FACING_NESW;
            case VoxelFace::CrossNWSE:  return FACING_NWSE;
            default:                    return FACING_NORTH;
        }
    }

    switch(face) {
        case VoxelFace::CubeNorth:  return FACING_NORTH;
        case VoxelFace::CubeSouth:  return FACING_SOUTH;
        case VoxelFace::CubeEast:   return FACING_EAST;
        case VoxelFace::CubeWest:   return FACING_WEST;
        case VoxelFace::CubeTop:    return FACING_UP;
        case VoxelFace::CubeBottom: return FACING_DOWN;
        default:                    return FACING_NORTH;
    }
}

static void push_quad_a(WorkerContext *ctx, const VoxelInfo *info, const Vec3f &pos, const Vec2f &size, VoxelFace face)
{
    const VoxelFacing facing = get_facing(face, info->type);
    const VoxelTexture &vtex = info->textures[static_cast<std::size_t>(face)];

    if(info->blending)
        ctx->quads_b[vtex.cached_plane].push_back(make_chunk_quad(pos, size, facing, vtex.cached_offset, vtex.paths.size()));
    else ctx->quads_nb[vtex.cached_plane].push_back(make_chunk_quad(pos, size, facing, vtex.cached_offset, vtex.paths.size()));
}

static void push_quad_v(WorkerContext *ctx, const VoxelInfo *info, const Vec3f &pos, const Vec2f &size, VoxelFace face, std::size_t entropy)
{
    const VoxelFacing facing = get_facing(face, info->type);
    const VoxelTexture &vtex = info->textures[static_cast<std::size_t>(face)];
    const std::size_t entropy_mod = entropy % vtex.paths.size();

    if(info->blending)
        ctx->quads_b[vtex.cached_plane].push_back(make_chunk_quad(pos, size, facing, vtex.cached_offset + entropy_mod, 0));
    else ctx->quads_nb[vtex.cached_plane].push_back(make_chunk_quad(pos, size, facing, vtex.cached_offset + entropy_mod, 0));
}

static void make_cube(WorkerContext *ctx, VoxelID voxel, const VoxelInfo *info, const LocalCoord &lpos, VoxelVis vis, std::size_t entropy)
{
    const Vec3f fpos = LocalCoord::to_vec3f(lpos);
    const Vec2f fsize = Vec2f(1.0f, 1.0f);

    if(info->animated) {
        if(vis & VIS_NORTH) push_quad_a(ctx, info, fpos, fsize, VoxelFace::CubeNorth);
        if(vis & VIS_SOUTH) push_quad_a(ctx, info, fpos, fsize, VoxelFace::CubeSouth);
        if(vis & VIS_EAST)  push_quad_a(ctx, info, fpos, fsize, VoxelFace::CubeEast);
        if(vis & VIS_WEST)  push_quad_a(ctx, info, fpos, fsize, VoxelFace::CubeWest);
        if(vis & VIS_UP)    push_quad_a(ctx, info, fpos, fsize, VoxelFace::CubeTop);
        if(vis & VIS_DOWN)  push_quad_a(ctx, info, fpos, fsize, VoxelFace::CubeBottom);
    }
    else {
        if(vis & VIS_NORTH) push_quad_v(ctx, info, fpos, fsize, VoxelFace::CubeNorth, entropy);
        if(vis & VIS_SOUTH) push_quad_v(ctx, info, fpos, fsize, VoxelFace::CubeSouth, entropy);
        if(vis & VIS_EAST)  push_quad_v(ctx, info, fpos, fsize, VoxelFace::CubeEast, entropy);
        if(vis & VIS_WEST)  push_quad_v(ctx, info, fpos, fsize, VoxelFace::CubeWest, entropy);
        if(vis & VIS_UP)    push_quad_v(ctx, info, fpos, fsize, VoxelFace::CubeTop, entropy);
        if(vis & VIS_DOWN)  push_quad_v(ctx, info, fpos, fsize, VoxelFace::CubeBottom, entropy);
    }
}

static void cache_chunk(WorkerContext *ctx, const ChunkCoord &cpos)
{
    const auto index = get_cached_cpos(ctx->coord, cpos);
    if(const Chunk *chunk = world::find(cpos)) {
        ctx->cache[index] = chunk->voxels;
        return;
    }
}

static void process(WorkerContext *ctx)
{
    ctx->quads_nb.resize(voxel_atlas::plane_count());
    ctx->quads_b.resize(voxel_atlas::plane_count());

    const VoxelStorage &voxels = ctx->cache.at(CPOS_ITSELF);

    for(std::size_t i = 0; i < CHUNK_VOLUME; ++i) {
        if(ctx->is_cancelled) {
            ctx->quads_nb.clear();
            ctx->quads_b.clear();
            return;
        }

        const auto voxel = voxels[i];
        const auto lpos = LocalCoord::from_index(i);

        const VoxelInfo *info = voxel_def::find(voxel);

        if(info == nullptr) {
            // Either a NULL_VOXEL or something went
            // horribly wrong and we don't what this is
            continue;
        }

        VoxelVis vis = 0;
        if(vis_test(ctx, voxel, info, lpos + LocalCoord::dir_north()))
            vis |= VIS_NORTH;
        if(vis_test(ctx, voxel, info, lpos + LocalCoord::dir_south()))
            vis |= VIS_SOUTH;
        if(vis_test(ctx, voxel, info, lpos + LocalCoord::dir_east()))
            vis |= VIS_EAST;
        if(vis_test(ctx, voxel, info, lpos + LocalCoord::dir_west()))
            vis |= VIS_WEST;
        if(vis_test(ctx, voxel, info, lpos + LocalCoord::dir_up()))
            vis |= VIS_UP;
        if(vis_test(ctx, voxel, info, lpos + LocalCoord::dir_down()))
            vis |= VIS_DOWN;

        const VoxelCoord vpos = ChunkCoord::to_voxel(ctx->coord, lpos);
        const VoxelCoord::value_type entropy_src = vpos[0] * vpos[1] * vpos[2];
        const auto entropy = crc64::get(&entropy_src, sizeof(entropy_src));

        // FIXME: handle different voxel types
        make_cube(ctx, voxel, info, lpos, vis, entropy);
    }
}

static void finalize(WorkerContext *ctx, entt::entity entity)
{
    auto &component = globals::registry.emplace_or_replace<ChunkMeshComponent>(entity);

    const std::size_t plane_count_nb = ctx->quads_nb.size();
    const std::size_t plane_count_b = ctx->quads_b.size();

    bool has_no_submeshes_b = true;
    bool has_no_submeshes_nb = true;

    component.quad_nb.resize(plane_count_nb);
    component.quad_b.resize(plane_count_b);

    for(std::size_t plane = 0; plane < plane_count_nb; ++plane) {
        QuadBuilder &builder = ctx->quads_nb[plane];
        ChunkVBO &buffer = component.quad_nb[plane];

        if(builder.empty()) {
            if(buffer.handle) {
                glDeleteBuffers(1, &buffer.handle);
                buffer.handle = 0;
                buffer.size = 0;
            }
        }
        else {
            if(!buffer.handle)
                glGenBuffers(1, &buffer.handle);
            glBindBuffer(GL_ARRAY_BUFFER, buffer.handle);
            glBufferData(GL_ARRAY_BUFFER, sizeof(ChunkQuad) * builder.size(), builder.data(), GL_STATIC_DRAW);
            buffer.size = builder.size();
            has_no_submeshes_nb = false;
        }        
    }

    for(std::size_t plane = 0; plane < plane_count_b; ++plane) {
        QuadBuilder &builder = ctx->quads_b[plane];
        ChunkVBO &buffer = component.quad_b[plane];

        if(builder.empty()) {
            if(buffer.handle) {
                glDeleteBuffers(1, &buffer.handle);
                buffer.handle = 0;
                buffer.size = 0;
            }
        }
        else {
            if(!buffer.handle)
                glGenBuffers(1, &buffer.handle);
            glBindBuffer(GL_ARRAY_BUFFER, buffer.handle);
            glBufferData(GL_ARRAY_BUFFER, sizeof(ChunkQuad) * builder.size(), builder.data(), GL_STATIC_DRAW);
            buffer.size = builder.size();
            has_no_submeshes_b = false;
        }        
    }

    if(has_no_submeshes_b && has_no_submeshes_nb)
        globals::registry.remove<ChunkMeshComponent>(entity);
    else chunk_visibility::update_chunk(entity);
}

#if defined(_WIN32) && !defined(NDEBUG)
// Debug builds for Win32 have a distinctive feature
// of utterly shitting themselves during threaded meshing.
// The code generated by MSVC on Debug configuration is
// just so slow and full of whatever debug shit the compiler
// decides to put there, it is slower than generating terrain.
constexpr static const size_t MESHER_THREADS_COUNT = 1;
constexpr static const size_t MESHER_TASKS_PER_FRAME = 16;
#else
constexpr static const size_t MESHER_THREADS_COUNT = 2;
constexpr static const size_t MESHER_TASKS_PER_FRAME = 64;
#endif

static BS::thread_pool workers_pool = BS::thread_pool(MESHER_THREADS_COUNT);
static std::unordered_map<ChunkCoord, std::unique_ptr<WorkerContext>> workers = {};

// Bogus internal flag component
struct NeedsMeshingComponent final {};

static void on_chunk_create(const ChunkCreateEvent &event)
{
    const std::array<ChunkCoord, 6> neighbours = {
        event.coord + ChunkCoord::dir_north(),
        event.coord + ChunkCoord::dir_south(),
        event.coord + ChunkCoord::dir_east(),
        event.coord + ChunkCoord::dir_west(),
        event.coord + ChunkCoord::dir_up(),
        event.coord + ChunkCoord::dir_down(),
    };

    globals::registry.emplace_or_replace<NeedsMeshingComponent>(event.chunk->entity);

    for(const ChunkCoord &cpos : neighbours) {
        if(const Chunk *chunk = world::find(cpos)) {
            globals::registry.emplace_or_replace<NeedsMeshingComponent>(chunk->entity);
            continue;
        }
    }
}

static void on_chunk_update(const ChunkUpdateEvent &event)
{
    const std::array<ChunkCoord, 6> neighbours = {
        event.coord + ChunkCoord::dir_north(),
        event.coord + ChunkCoord::dir_south(),
        event.coord + ChunkCoord::dir_east(),
        event.coord + ChunkCoord::dir_west(),
        event.coord + ChunkCoord::dir_up(),
        event.coord + ChunkCoord::dir_down(),
    };

    globals::registry.emplace_or_replace<NeedsMeshingComponent>(event.chunk->entity);

    for(const ChunkCoord &cpos : neighbours) {
        if(const Chunk *chunk = world::find(cpos)) {
            globals::registry.emplace_or_replace<NeedsMeshingComponent>(chunk->entity);
            continue;
        }
    }
}

static void on_voxel_set(const VoxelSetEvent &event)
{
    globals::registry.emplace_or_replace<NeedsMeshingComponent>(event.chunk->entity);

    std::vector<ChunkCoord> neighbours = {};

    for(int dim = 0; dim < 3; ++dim) {
        ChunkCoord offset = ChunkCoord(0, 0, 0);
        offset[dim] = 1;

        if(event.lpos[dim] == 0) {
            neighbours.push_back(event.cpos - offset);
            continue;
        }

        if(event.lpos[dim] == (CHUNK_SIZE - 1)) {
            neighbours.push_back(event.cpos + offset);
            continue;
        }
    }

    for(const ChunkCoord &cpos : neighbours) {
        if(const Chunk *chunk = world::find(cpos)) {
            globals::registry.emplace_or_replace<NeedsMeshingComponent>(chunk->entity);
            continue;
        }
    }
}

void chunk_mesher::init(void)
{
    globals::dispatcher.sink<ChunkCreateEvent>().connect<&on_chunk_create>();
    globals::dispatcher.sink<ChunkUpdateEvent>().connect<&on_chunk_update>();
    globals::dispatcher.sink<VoxelSetEvent>().connect<&on_voxel_set>();
}

void chunk_mesher::deinit(void)
{
    for(auto &worker : workers)
        worker.second->is_cancelled = true;
    workers_pool.wait();
    workers.clear();
}

void chunk_mesher::update(void)
{
    std::size_t finalized = 0;
    std::size_t enqueued = 0;

    auto worker = workers.cbegin();
    while(worker != workers.cend()) {
        if(!worker->second->future.valid()) {
            ++worker;
            continue;
        }

        if(worker->second->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++worker;
            continue;
        }

        if(worker->second->is_cancelled) {
            worker = workers.erase(worker);
            continue;
        }

        if(const Chunk *chunk = world::find(worker->second->coord)) {
            if(globals::registry.any_of<NeedsMeshingComponent>(chunk->entity)) {
                worker = workers.erase(worker);
                continue;
            }

            finalize(worker->second.get(), chunk->entity);
            finalized += 1U;
        }

        worker = workers.erase(worker);

        if(finalized >= MESHER_TASKS_PER_FRAME)
            break;
        continue;
    }

    const auto group = globals::registry.group<NeedsMeshingComponent>(entt::get<ChunkComponent>);
    for(const auto [entity, chunk] : group.each()) {
        const auto it = workers.find(chunk.coord);

        if(it == workers.cend()) {
            globals::registry.remove<NeedsMeshingComponent>(entity);

            auto &worker = workers.emplace(chunk.coord, std::make_unique<WorkerContext>()).first->second;
            worker->coord = chunk.coord;

            cache_chunk(worker.get(), chunk.coord);
            cache_chunk(worker.get(), chunk.coord + ChunkCoord::dir_north());
            cache_chunk(worker.get(), chunk.coord + ChunkCoord::dir_south());
            cache_chunk(worker.get(), chunk.coord + ChunkCoord::dir_east());
            cache_chunk(worker.get(), chunk.coord + ChunkCoord::dir_west());
            cache_chunk(worker.get(), chunk.coord + ChunkCoord::dir_up());
            cache_chunk(worker.get(), chunk.coord + ChunkCoord::dir_down());

            worker->future = workers_pool.submit_task(std::bind(&process, worker.get()));

            enqueued += 1U;
        }
        else {
            it->second->is_cancelled = true;
            continue;
        }

        if(enqueued >= MESHER_TASKS_PER_FRAME)
            break;
        continue;
    }

    if(finalized || enqueued) {
        spdlog::info("mesher: {} {}", finalized, enqueued);
    }
}
