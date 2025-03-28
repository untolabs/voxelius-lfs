// SPDX-License-Identifier: BSD-2-Clause
#include "client/precompiled.hh"
#include "client/game.hh"

#include "cmake/config.hh"

#include "common/resource/binary_file.hh"

#include "common/cmdline.hh"
#include "common/config.hh"
#include "common/crc64.hh"
#include "common/epoch.hh"
#include "common/fstools.hh"

#include "shared/entity/collision.hh"
#include "shared/entity/gravity.hh"
#include "shared/entity/stasis.hh"
#include "shared/entity/transform.hh"
#include "shared/entity/velocity.hh"

#include "shared/world/game_items.hh"
#include "shared/world/game_voxels.hh"
#include "shared/world/item_def.hh"
#include "shared/world/ray_dda.hh"
#include "shared/world/unloader.hh"
#include "shared/world/voxel_def.hh"
#include "shared/world/world.hh"

#include "shared/protocol.hh"

#include "client/entity/interpolation.hh"
#include "client/entity/player_look.hh"
#include "client/entity/player_move.hh"
#include "client/entity/player_target.hh"
#include "client/entity/sound_emitter.hh"

#include "client/event/glfw_framebuffer_size.hh"

#include "client/gui/background.hh"
#include "client/gui/chat.hh"
#include "client/gui/gui_screen.hh"
#include "client/gui/language.hh"
#include "client/gui/main_menu.hh"
#include "client/gui/message_box.hh"
#include "client/gui/play_menu.hh"
#include "client/gui/player_list.hh"
#include "client/gui/progress.hh"
#include "client/gui/settings.hh"
#include "client/gui/splash.hh"

#include "client/hud/crosshair.hh"
#include "client/hud/hotbar.hh"
#include "client/hud/metrics.hh"
#include "client/hud/status_lines.hh"

#include "client/resource/texture2D.hh"

#include "client/sound/listener.hh"
#include "client/sound/sound.hh"

#include "client/world/chunk_mesher.hh"
#include "client/world/chunk_renderer.hh"
#include "client/world/chunk_visibility.hh"
#include "client/world/outline.hh"
#include "client/world/skybox.hh"
#include "client/world/voxel_anims.hh"
#include "client/world/voxel_atlas.hh"

#include "client/const.hh"
#include "client/globals.hh"
#include "client/keyboard.hh"
#include "client/keynames.hh"
#include "client/mouse.hh"
#include "client/receive.hh"
#include "client/screenshot.hh"
#include "client/session.hh"
#include "client/toggles.hh"
#include "client/view.hh"

#if ENABLE_EXPERIMENTS
#include "shared/entity/head.hh"
#include "shared/entity/player.hh"
#include "client/experiments.hh"
#endif /* ENABLE_EXPERIMENTS */

static std::shared_ptr<const BinaryFile> bin_unscii16 = nullptr;
static std::shared_ptr<const BinaryFile> bin_unscii8 = nullptr;

bool client_game::streamer_mode = false;
bool client_game::vertical_sync = true;
bool client_game::world_curvature = true;
unsigned int client_game::pixel_size = 4U;
unsigned int client_game::fog_mode = 1U;
std::string client_game::username = "player";

static void on_glfw_framebuffer_size(const GlfwFramebufferSizeEvent &event)
{
    if(globals::world_fbo) {
        glDeleteRenderbuffers(1, &globals::world_fbo_depth);
        glDeleteTextures(1, &globals::world_fbo_color);
        glDeleteFramebuffers(1, &globals::world_fbo);
    }

    glGenFramebuffers(1, &globals::world_fbo);
    glGenTextures(1, &globals::world_fbo_color);
    glGenRenderbuffers(1, &globals::world_fbo_depth);

    glBindTexture(GL_TEXTURE_2D, globals::world_fbo_color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, event.width, event.height, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);

    glBindRenderbuffer(GL_RENDERBUFFER, globals::world_fbo_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, event.width, event.height);

    glBindFramebuffer(GL_FRAMEBUFFER, globals::world_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, globals::world_fbo_color, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, globals::world_fbo_depth);

    if(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        spdlog::critical("opengl: world framebuffer is incomplete");
        glDeleteRenderbuffers(1, &globals::world_fbo_depth);
        glDeleteTextures(1, &globals::world_fbo_color);
        glDeleteFramebuffers(1, &globals::world_fbo);
        std::terminate();
    }

    const float width_float = event.width;
    const float height_float = event.height;
    const unsigned int wscale = cxpr::max(1U, cxpr::floor<unsigned int>(width_float / static_cast<float>(BASE_WIDTH)));
    const unsigned int hscale = cxpr::max(1U, cxpr::floor<unsigned int>(height_float / static_cast<float>(BASE_HEIGHT)));
    const unsigned int scale = cxpr::min(wscale, hscale);

    if(globals::gui_scale != scale) {
        ImGuiIO &io = ImGui::GetIO();
        ImGuiStyle &style = ImGui::GetStyle();

        ImFontConfig font_config = {};
        font_config.FontDataOwnedByAtlas = false;

        io.Fonts->Clear();

        ImFontGlyphRangesBuilder builder = {};
        std::vector<uint8_t> fontbin = {};

        // This should cover a hefty range of glyph ranges.
        // UNDONE: just slap the whole UNICODE Plane-0 here?
        builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
        builder.AddRanges(io.Fonts->GetGlyphRangesCyrillic());
        builder.AddRanges(io.Fonts->GetGlyphRangesGreek());
        builder.AddRanges(io.Fonts->GetGlyphRangesJapanese());

        ImVector<ImWchar> ranges = {};
        builder.BuildRanges(&ranges);

        globals::font_default = io.Fonts->AddFontFromMemoryTTF(bin_unscii16->buffer, bin_unscii16->length, 16.0f * scale, &font_config, ranges.Data);
        globals::font_chat = io.Fonts->AddFontFromMemoryTTF(bin_unscii16->buffer, bin_unscii16->length, 8.0f * scale, &font_config, ranges.Data);
        globals::font_debug = io.Fonts->AddFontFromMemoryTTF(bin_unscii8->buffer, bin_unscii8->length, 4.0f * scale, &font_config);

        // Re-assign the default font
        io.FontDefault = globals::font_default;

        // This should be here!!! Just calling Build()
        // on the font atlas does not invalidate internal
        // device objects defined by the implementation!!!
        ImGui_ImplOpenGL3_CreateDeviceObjects();

        if(globals::gui_scale) {
            // Well, ImGuiStyle::ScaleAllSizes indeed takes
            // the scale values as a RELATIVE scaling, not as
            // absolute. So I have to make a special crutch
            style.ScaleAllSizes(static_cast<float>(scale) / static_cast<float>(globals::gui_scale));
        }

        globals::gui_scale = scale;
    }
}

void client_game::init(void)
{
    bin_unscii16 = resource::load<BinaryFile>("fonts/unscii-16.ttf");
    bin_unscii8 = resource::load<BinaryFile>("fonts/unscii-8.ttf");

    if(!bin_unscii16 || !bin_unscii8) {
        spdlog::critical("client_game: font loading failed");
        std::terminate();
    }

    splash::init();
    splash::render(std::string());

    Config::add(globals::client_config, "game.streamer_mode", client_game::streamer_mode);
    Config::add(globals::client_config, "game.vertical_sync", client_game::vertical_sync);
    Config::add(globals::client_config, "game.world_curvature", client_game::world_curvature);
    Config::add(globals::client_config, "game.pixel_size", client_game::pixel_size);
    Config::add(globals::client_config, "game.fog_mode", client_game::fog_mode);
    Config::add(globals::client_config, "game.username", client_game::username);

    settings::add_checkbox(0, settings::VIDEO_GUI, "game.streamer_mode", client_game::streamer_mode, true);
    settings::add_checkbox(5, settings::VIDEO, "game.vertical_sync", client_game::vertical_sync, false);
    settings::add_checkbox(4, settings::VIDEO, "game.world_curvature", client_game::world_curvature, true);
    settings::add_slider(1, settings::VIDEO, "game.pixel_size", client_game::pixel_size, 1U, 4U, true);
    settings::add_stepper(3, settings::VIDEO, "game.fog_mode", client_game::fog_mode, 3U, false);
    settings::add_input(1, settings::GENERAL, "game.username", client_game::username, true, false);

    globals::client_host = enet_host_create(nullptr, 1, 1, 0, 0);

    if(!globals::client_host) {
        spdlog::critical("game: unable to setup an ENet host");
        std::terminate();
    }

    language::init();

    session::init();

    player_move::init();
    player_target::init();

    keynames::init();
    keyboard::init();
    mouse::init();

    screenshot::init();

    view::init();

    voxel_anims::init();

    chunk_mesher::init();
    chunk_renderer::init();

    skybox::init();

    outline::init();

    world::init();

    unloader::init();

    ImGuiStyle &style = ImGui::GetStyle();

    // Black buttons on a dark background
    // may be harder to read than the text on them
    style.FrameBorderSize = 1.0;
    style.TabBorderSize = 1.0;

    // Rounding on elements looks cool but I am
    // aiming for a more or less blocky and
    // visually simple HiDPI-friendly UI style
    style.TabRounding       = 0.0f;
    style.GrabRounding      = 0.0f;
    style.ChildRounding     = 0.0f;
    style.FrameRounding     = 0.0f;
    style.PopupRounding     = 0.0f;
    style.WindowRounding    = 0.0f;
    style.ScrollbarRounding = 0.0f;

    style.Colors[ImGuiCol_Text]                     = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    style.Colors[ImGuiCol_TextDisabled]             = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    style.Colors[ImGuiCol_WindowBg]                 = ImVec4(0.06f, 0.06f, 0.06f, 0.94f);
    style.Colors[ImGuiCol_ChildBg]                  = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    style.Colors[ImGuiCol_PopupBg]                  = ImVec4(0.08f, 0.08f, 0.08f, 0.94f);
    style.Colors[ImGuiCol_Border]                   = ImVec4(0.79f, 0.79f, 0.79f, 0.50f);
    style.Colors[ImGuiCol_BorderShadow]             = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    style.Colors[ImGuiCol_FrameBg]                  = ImVec4(0.00f, 0.00f, 0.00f, 0.54f);
    style.Colors[ImGuiCol_FrameBgHovered]           = ImVec4(0.36f, 0.36f, 0.36f, 0.40f);
    style.Colors[ImGuiCol_FrameBgActive]            = ImVec4(0.63f, 0.63f, 0.63f, 0.67f);
    style.Colors[ImGuiCol_TitleBg]                  = ImVec4(0.04f, 0.04f, 0.04f, 1.00f);
    style.Colors[ImGuiCol_TitleBgActive]            = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    style.Colors[ImGuiCol_TitleBgCollapsed]         = ImVec4(0.00f, 0.00f, 0.00f, 0.51f);
    style.Colors[ImGuiCol_MenuBarBg]                = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    style.Colors[ImGuiCol_ScrollbarBg]              = ImVec4(0.02f, 0.02f, 0.02f, 0.53f);
    style.Colors[ImGuiCol_ScrollbarGrab]            = ImVec4(0.00f, 0.00f, 0.00f, 0.75f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered]     = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
    style.Colors[ImGuiCol_ScrollbarGrabActive]      = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    style.Colors[ImGuiCol_CheckMark]                = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    style.Colors[ImGuiCol_SliderGrab]               = ImVec4(0.81f, 0.81f, 0.81f, 0.75f);
    style.Colors[ImGuiCol_SliderGrabActive]         = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    style.Colors[ImGuiCol_Button]                   = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    style.Colors[ImGuiCol_ButtonHovered]            = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
    style.Colors[ImGuiCol_ButtonActive]             = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    style.Colors[ImGuiCol_Header]                   = ImVec4(0.00f, 0.00f, 0.00f, 0.75f);
    style.Colors[ImGuiCol_HeaderHovered]            = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
    style.Colors[ImGuiCol_HeaderActive]             = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    style.Colors[ImGuiCol_Separator]                = ImVec4(0.49f, 0.49f, 0.49f, 0.50f);
    style.Colors[ImGuiCol_SeparatorHovered]         = ImVec4(0.56f, 0.56f, 0.56f, 0.78f);
    style.Colors[ImGuiCol_SeparatorActive]          = ImVec4(0.90f, 0.90f, 0.90f, 1.00f);
    style.Colors[ImGuiCol_ResizeGrip]               = ImVec4(0.34f, 0.34f, 0.34f, 0.20f);
    style.Colors[ImGuiCol_ResizeGripHovered]        = ImVec4(0.57f, 0.57f, 0.57f, 0.67f);
    style.Colors[ImGuiCol_ResizeGripActive]         = ImVec4(1.00f, 1.00f, 1.00f, 0.95f);
    style.Colors[ImGuiCol_Tab]                      = ImVec4(0.00f, 0.00f, 0.00f, 0.75f);
    style.Colors[ImGuiCol_TabHovered]               = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
    style.Colors[ImGuiCol_TabActive]                = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    style.Colors[ImGuiCol_TabUnfocused]             = ImVec4(0.13f, 0.13f, 0.13f, 0.97f);
    style.Colors[ImGuiCol_TabUnfocusedActive]       = ImVec4(0.44f, 0.44f, 0.44f, 1.00f);
    style.Colors[ImGuiCol_PlotLines]                = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
    style.Colors[ImGuiCol_PlotLinesHovered]         = ImVec4(0.69f, 0.00f, 0.00f, 1.00f);
    style.Colors[ImGuiCol_PlotHistogram]            = ImVec4(0.00f, 1.00f, 0.20f, 1.00f);
    style.Colors[ImGuiCol_PlotHistogramHovered]     = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
    style.Colors[ImGuiCol_TableHeaderBg]            = ImVec4(0.19f, 0.19f, 0.20f, 1.00f);
    style.Colors[ImGuiCol_TableBorderStrong]        = ImVec4(0.31f, 0.31f, 0.35f, 1.00f);
    style.Colors[ImGuiCol_TableBorderLight]         = ImVec4(0.23f, 0.23f, 0.25f, 1.00f);
    style.Colors[ImGuiCol_TableRowBg]               = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    style.Colors[ImGuiCol_TableRowBgAlt]            = ImVec4(1.00f, 1.00f, 1.00f, 0.06f);
    style.Colors[ImGuiCol_TextSelectedBg]           = ImVec4(0.61f, 0.61f, 0.61f, 0.35f);
    style.Colors[ImGuiCol_DragDropTarget]           = ImVec4(1.00f, 1.00f, 0.00f, 1.00f);
    style.Colors[ImGuiCol_NavHighlight]             = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    style.Colors[ImGuiCol_NavWindowingHighlight]    = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    style.Colors[ImGuiCol_NavWindowingDimBg]        = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    style.Colors[ImGuiCol_ModalWindowDimBg]         = ImVec4(0.80f, 0.80f, 0.80f, 0.35f);

    // Making my own Game UI for Source Engine
    // taught me one important thing: dimensions
    // of UI elements must be calculated at semi-runtime
    // so there's simply no point for an INI file.
    ImGui::GetIO().IniFilename = nullptr;

    toggles::init();

    background::init();

    player_list::init();

    client_chat::init();

    main_menu::init();
    play_menu::init();
    settings::init();
    progress::init();
    message_box::init();

#if ENABLE_EXPERIMENTS
    experiments::init();
#endif /* ENABLE_EXPERIMENTS */

    crosshair::init();
    hotbar::init();
    metrics::init();
    status_lines::init();

    globals::gui_keybind_ptr = nullptr;
    globals::gui_scale = 0U;
    globals::gui_screen = GUI_MAIN_MENU;

    sound::init();

    globals::dispatcher.sink<GlfwFramebufferSizeEvent>().connect<&on_glfw_framebuffer_size>();
}

void client_game::init_late(void)
{
    sound::init_late();

    language::init_late();

    settings::init_late();

    client_chat::init_late();

    status_lines::init_late();

    game_voxels::populate();
    game_items::populate();

#if ENABLE_EXPERIMENTS
    experiments::init_late();
#endif /* ENABLE_EXPERIMENTS */

    std::size_t max_texture_count = 0;

    // Figure out the total texture count
    // NOTE: this is very debug, early and a quite
    // conservative limit choice; there must be a better
    // way to make this limit way smaller than it currently is
    for(const std::shared_ptr<VoxelInfo> &info : voxel_def::voxels) {
        for(const VoxelTexture &vtex : info->textures) {
            max_texture_count += vtex.paths.size();
        }
    }

    // UNDONE: asset packs for non-16x16 stuff
    voxel_atlas::create(16, 16, max_texture_count);

    for(std::shared_ptr<VoxelInfo> &info : voxel_def::voxels) {
        for(VoxelTexture &vtex : info->textures) {
            if(AtlasStrip *strip = voxel_atlas::find_or_load(vtex.paths)) {
                vtex.cached_offset = strip->offset;
                vtex.cached_plane = strip->plane;
                continue;
            }
            
            spdlog::critical("game: {}: failed to load atlas strips", info->name);
            std::terminate();
        }
    }

    voxel_atlas::generate_mipmaps();

    for(std::shared_ptr<ItemInfo> &info : item_def::items) {
        info->cached_texture = resource::load<Texture2D>(info->texture, TEXTURE2D_LOAD_CLAMP_S | TEXTURE2D_LOAD_CLAMP_T);
    }

    client_receive::init();

    splash::init_late();
}

void client_game::deinit(void)
{
    player_move::deinit();

    session::deinit();

    sound::deinit();

    hotbar::deinit();

#if ENABLE_EXPERIMENTS
    experiments::deinit();
#endif /* ENABLE_EXPERIMENTS */

    main_menu::deinit();

    play_menu::deinit();

    voxel_atlas::destroy();

    glDeleteRenderbuffers(1, &globals::world_fbo_depth);
    glDeleteTextures(1, &globals::world_fbo_color);
    glDeleteFramebuffers(1, &globals::world_fbo);

    background::deinit();

    outline::deinit();

    crosshair::deinit();

    chunk_renderer::deinit();
    chunk_mesher::deinit();

    globals::registry.clear();

    item_def::purge();
    voxel_def::purge();

    enet_host_destroy(globals::client_host);

    bin_unscii8 = nullptr;
    bin_unscii16 = nullptr;
}

void client_game::fixed_update(void)
{
    player_move::fixed_update();

    // Only update world simulation gamesystems
    // if the player can actually observe all the
    // changes these gamesystems cause visually
    if(globals::registry.valid(globals::player)) {
        CollisionComponent::fixed_update();

        VelocityComponent::fixed_update();

        TransformComponent::fixed_update();

        GravityComponent::fixed_update();

        StasisComponent::fixed_update();
    }
}

void client_game::fixed_update_late(void)
{
    if(globals::registry.valid(globals::player)) {
        protocol::send_entity_head(session::peer, nullptr, globals::player);
        protocol::send_entity_transform(session::peer, nullptr, globals::player);
        protocol::send_entity_velocity(session::peer, nullptr, globals::player);
    }
}

void client_game::update(void)
{
    session::sp::update();

    sound::update();

    listener::update();

#if ENABLE_EXPERIMENTS
    experiments::update();
#endif /* ENABLE_EXPERIMENTS */

    interpolation::update();

    player_target::update();

    view::update();

    SoundEmitterComponent::update();

    voxel_anims::update();

    chunk_mesher::update();

    chunk_visibility::update();
    
    client_chat::update();
}

void client_game::update_late(void)
{
    session::sp::update_late();

#if ENABLE_EXPERIMENTS
    experiments::update_late();
#endif /* ENABLE_EXPERIMENTS */

    mouse::update_late();

    if(client_game::vertical_sync)
        glfwSwapInterval(1);
    else glfwSwapInterval(0);

    ENetEvent host_event;
    while(0 < enet_host_service(globals::client_host, &host_event, 0)) {
        switch(host_event.type) {
            case ENET_EVENT_TYPE_CONNECT:
                session::mp::send_login_request();
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
                session::invalidate();
                break;
            case ENET_EVENT_TYPE_RECEIVE:
                protocol::receive(host_event.packet, host_event.peer);
                enet_packet_destroy(host_event.packet);
                break;
        }
    }

    play_menu::update_late();
}

void client_game::render(void)
{
    const int scaled_width = globals::width / cxpr::max(1U, client_game::pixel_size);
    const int scaled_height = globals::height / cxpr::max(1U, client_game::pixel_size);

    glViewport(0, 0, scaled_width, scaled_height);
    glBindFramebuffer(GL_FRAMEBUFFER, globals::world_fbo);
    glClearColor(skybox::fog_color[0], skybox::fog_color[1], skybox::fog_color[2], 1.000f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    chunk_renderer::render();

    player_target::render();

    sound::render();

#if ENABLE_EXPERIMENTS
    const auto group = globals::registry.group(entt::get<PlayerComponent, CollisionComponent, HeadComponentIntr, TransformComponentIntr>);

    outline::prepare();

    glEnable(GL_DEPTH_TEST);

    for(const auto [entity, collision, head, transform] : group.each()) {
        if(entity == globals::player) {
            // Don't render ourselves
            continue;
        }

        Vec3f forward;
        Vec3angles::vectors(transform.angles + head.angles, forward);
        forward *= 2.0f;

        Vec3f hull_size = collision.hull.max - collision.hull.min;
        WorldCoord hull = transform.position;
        hull.local += collision.hull.min;

        WorldCoord look = transform.position;
        look.local += head.position;

        outline::cube(hull, hull_size, 2.0f, Vec4f::red());
        outline::line(look, forward, 2.0f, Vec4f::light_gray());
    }
#endif /* ENABLE_EXPERIMENTS */

    glViewport(0, 0, globals::width, globals::height);
    glClearColor(0.000f, 0.000f, 0.000f, 1.000f);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glClear(GL_COLOR_BUFFER_BIT);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, globals::world_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, scaled_width, scaled_height, 0, 0, globals::width, globals::height, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    if((globals::gui_screen == GUI_SCREEN_NONE) || (globals::gui_screen == GUI_CHAT)) {
        crosshair::layout();
    }
}

void client_game::layout(void)
{
    if(!globals::registry.valid(globals::player)) {
        background::layout();
    }

    if(!globals::gui_screen || (globals::gui_screen == GUI_CHAT) || (globals::gui_screen == GUI_DEBUG_WINDOW)) {
        if(toggles::draw_metrics) {
            // This contains Minecraft-esque debug information
            // about the hardware, world state and other
            // things that might be uesful
            metrics::layout();
        }
    }

    if(globals::registry.valid(globals::player)) {
        client_chat::layout();
        player_list::layout();

        if(!globals::gui_screen) {
            hotbar::layout();

            status_lines::layout();
        }
    }

    if(globals::gui_screen) {
        if(globals::registry.valid(globals::player) && (globals::gui_screen != GUI_CHAT) && (globals::gui_screen != GUI_DEBUG_WINDOW)) {
            const float width_f = static_cast<float>(globals::width);
            const float height_f = static_cast<float>(globals::height);
            const ImU32 splash = ImGui::GetColorU32(ImVec4(0.00f, 0.00f, 0.00f, 0.75f));
            ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(), ImVec2(width_f, height_f), splash);
        }

        switch(globals::gui_screen) {
            case GUI_MAIN_MENU:
                main_menu::layout();
                break;
            case GUI_PLAY_MENU:
                play_menu::layout();
                break;
            case GUI_SETTINGS:
                settings::layout();
                break;
            case GUI_PROGRESS:
                progress::layout();
                break;
            case GUI_MESSAGE_BOX:
                message_box::layout();
                break;
        }
    }
}
