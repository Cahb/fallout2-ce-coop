cmake_minimum_required(VERSION 3.13)

set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

set(EXECUTABLE_NAME fallout2-ce)

if(APPLE)
    if(IOS)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "12" CACHE STRING "")
        set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "")
    else()
        set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "")
        set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "")
    endif()
endif()

project(${EXECUTABLE_NAME})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_EXTENSIONS NO)

option(FALLOUT_VENDORED "Use vendored third-party libraries" ON)

if(ANDROID)
    add_library(${EXECUTABLE_NAME} SHARED)
else()
    add_executable(${EXECUTABLE_NAME} WIN32 MACOSX_BUNDLE)
endif()

add_library(f2_core OBJECT
    "src/actions.cc"
    "src/actions.h"
    "src/art.cc"
    "src/art.h"
    "src/autorun.cc"
    "src/autorun.h"
    "src/cache.cc"
    "src/cache.h"
    "src/character_transaction.cc"
    "src/character_transaction.h"
    "src/combat.cc"
    "src/combat.h"
    "src/combat_ai.cc"
    "src/combat_ai.h"
    "src/combat_ai_defs.h"
    "src/combat_ap.h"
    "src/combat_defs.h"
    "src/combat_drain.cc"
    "src/barter_intent.cc"
    "src/barter_intent.h"
    "src/combat_intent.cc"
    "src/combat_intent.h"
    "src/command.cc"
    "src/command.h"
    "src/config.cc"
    "src/config.h"
    "src/dialog_intent.cc"
    "src/dialog_intent.h"
    "src/worldmap_intent.cc"
    "src/worldmap_intent.h"
    "src/critter.cc"
    "src/critter.h"
    "src/datafile.cc"
    "src/datafile.h"
    "src/db.cc"
    "src/db.h"
    "src/debug.cc"
    "src/debug.h"
    "src/dfile.cc"
    "src/dfile.h"
    "src/dictionary.cc"
    "src/dictionary.h"
    "src/export.cc"
    "src/export.h"
    "src/file_find.cc"
    "src/file_find.h"
    "src/file_utils.cc"
    "src/file_utils.h"
    "src/game.cc"
    "src/game.h"
    "src/game_config.cc"
    "src/game_config.h"
    "src/game_memory.cc"
    "src/game_memory.h"
    "src/game_movie_state.cc"
    "src/game_vars.h"
    "src/geometry.cc"
    "src/geometry.h"
    "src/heap.cc"
    "src/heap.h"
    "src/input_replay.cc"
    "src/input_replay.h"
    "src/interpreter.cc"
    "src/interpreter.h"
    "src/interpreter_extra.cc"
    "src/interpreter_extra.h"
    "src/interpreter_lib.cc"
    "src/interpreter_lib.h"
    "src/invariants.cc"
    "src/invariants.h"
    "src/inventory.cc"
    "src/inventory.h"
    "src/item.cc"
    "src/item.h"
    "src/light.cc"
    "src/light.h"
    "src/map.cc"
    "src/map.h"
    "src/map_defs.h"
    "src/memory.cc"
    "src/memory.h"
    "src/memory_defs.h"
    "src/memory_manager.cc"
    "src/memory_manager.h"
    "src/message.cc"
    "src/message.h"
    "src/msg_channel.h"
    "src/movement.cc"
    "src/movement.h"
    "src/nevs.cc"
    "src/nevs.h"
    "src/obj_types.h"
    "src/object.cc"
    "src/opcode_trace.cc"
    "src/opcode_trace.h"
    "src/object.h"
    "src/object_delta.cc"
    "src/object_delta.h"
    "src/party_member.cc"
    "src/party_member.h"
    "src/path.cc"
    "src/path.h"
    "src/perk.cc"
    "src/perk.h"
    "src/perk_defs.h"
    "src/player_create.cc"
    "src/player_create.h"
    "src/player_sheet.cc"
    "src/player_sheet.h"
    "src/platform_compat.cc"
    "src/platform_compat.h"
    "src/pointer_registry.cc"
    "src/pointer_registry.h"
    "src/animation_scheduler.cc"
    "src/animation_scheduler.h"
    "src/presenter.cc"
    "src/presenter.h"
    "src/pres_record.cc"
    "src/pres_record.h"
    "src/presenter_narrate.cc"
    "src/presenter_narrate.h"
    "src/presenter_network.cc"
    "src/presenter_network.h"
    "src/proto.cc"
    "src/proto.h"
    "src/proto_instance.cc"
    "src/proto_instance.h"
    "src/proto_types.h"
    "src/queue.cc"
    "src/queue.h"
    "src/random.cc"
    "src/random.h"
    "src/reaction.cc"
    "src/reaction.h"
    "src/region.cc"
    "src/region.h"
    "src/script_request_handler.cc"
    "src/script_request_handler.h"
    "src/automap_state.cc"
    "src/character_editor_state.cc"
    "src/preferences_state.cc"
    "src/preferences_state.h"
    "src/savegame.cc"
    "src/savegame.h"
    "src/scripts.cc"
    "src/scripts.h"
    "src/select_file_list.cc"
    "src/select_file_list.h"
    "src/server_loop.cc"
    "src/server_loop.h"
    "src/server_accounts.cc"
    "src/server_accounts.h"
    "src/server_players.cc"
    "src/server_script_rules.cc"
    "src/server_script_rules.h"
    "src/server_players.h"
    "src/settings.cc"
    "src/settings.h"
    "src/sfall_arrays.cc"
    "src/sfall_arrays.h"
    "src/sfall_config.cc"
    "src/sfall_config.h"
    "src/sfall_global_scripts.cc"
    "src/sfall_global_scripts.h"
    "src/sfall_global_vars.cc"
    "src/sfall_global_vars.h"
    "src/sfall_ini.cc"
    "src/sfall_ini.h"
    "src/sfall_lists.cc"
    "src/sfall_lists.h"
    "src/sfall_metarules.cc"
    "src/sfall_metarules.h"
    "src/sfall_opcodes.cc"
    "src/sfall_opcodes.h"
    "src/sfx_name.cc"
    "src/skill.cc"
    "src/skill.h"
    "src/skill_defs.h"
    "src/sim_clock.cc"
    "src/sim_clock.h"
    "src/stat.cc"
    "src/stat.h"
    "src/timing.cc"
    "src/stat_defs.h"
    "src/state_dump.cc"
    "src/state_dump.h"
    "src/string_parsers.cc"
    "src/string_parsers.h"
    "src/tile.cc"
    "src/tile.h"
    "src/trait.cc"
    "src/trait.h"
    "src/trait_defs.h"
    "src/version.cc"
    "src/version.h"
    "src/worldmap.cc"
    "src/worldmap.h"
    "src/worldmap_defs.h"
    "src/xfile.cc"
    "src/xfile.h"
)

add_library(f2_client OBJECT
    "src/animation.cc"
    "src/animation.h"
    "src/art_render.cc"
    "src/audio.cc"
    "src/audio.h"
    "src/audio_engine.cc"
    "src/audio_engine.h"
    "src/audio_file.cc"
    "src/audio_file.h"
    "src/automap.cc"
    "src/automap.h"
    "src/character_editor.cc"
    "src/character_editor.h"
    "src/character_selector.cc"
    "src/character_selector.h"
    "src/client_present.cc"
    "src/client_present.h"
    "src/client_barter.cc"
    "src/client_barter.h"
    "src/client_dialog.cc"
    "src/client_dialog.h"
    "src/client_net.cc"
    "src/client_net.h"
    "src/color.cc"
    "src/color.h"
    "src/combat_ui.cc"
    "src/combat_ui.h"
    "src/credits.cc"
    "src/credits.h"
    "src/cycle.cc"
    "src/cycle.h"
    "src/dbox.cc"
    "src/dbox.h"
    "src/delay.cc"
    "src/delay.h"
    "src/dialog.cc"
    "src/dialog.h"
    "src/dinput.cc"
    "src/dinput.h"
    "src/display_monitor.cc"
    "src/display_monitor.h"
    "src/draw.cc"
    "src/draw.h"
    "src/elevator.cc"
    "src/elevator.h"
    "src/endgame.cc"
    "src/endgame.h"
    "src/font_manager.cc"
    "src/font_manager.h"
    "src/fps_limiter.cc"
    "src/fps_limiter.h"
    "src/game_dialog.cc"
    "src/game_dialog.h"
    "src/game_lifecycle.cc"
    "src/game_mouse.cc"
    "src/game_mouse.h"
    "src/game_movie.cc"
    "src/game_movie.h"
    "src/game_sound.cc"
    "src/game_sound.h"
    "src/game_ui.cc"
    "src/game_ui.h"
    "src/graph_lib.cc"
    "src/graph_lib.h"
    "src/input.cc"
    "src/input.h"
    "src/interface.cc"
    "src/interface.h"
    "src/inventory_ui.cc"
    "src/inventory_ui.h"
    "src/kb.cc"
    "src/kb.h"
    "src/lips.cc"
    "src/lips.h"
    "src/loadsave.cc"
    "src/loadsave.h"
    "src/mainmenu.cc"
    "src/mainmenu.h"
    "src/map_render.cc"
    "src/map_render.h"
    "src/mouse.cc"
    "src/mouse.h"
    "src/mouse_manager.cc"
    "src/mouse_manager.h"
    "src/movie.cc"
    "src/movie.h"
    "src/movie_effect.cc"
    "src/movie_effect.h"
    "src/movie_lib.cc"
    "src/movie_lib.h"
    "src/object_render.cc"
    "src/object_render.h"
    "src/options.cc"
    "src/options.h"
    "src/palette.cc"
    "src/palette.h"
    "src/pcx.cc"
    "src/pcx.h"
    "src/pipboy.cc"
    "src/pipboy.h"
    "src/preferences.cc"
    "src/preferences.h"
    "src/presenter_client.cc"
    "src/presenter_client.h"
    "src/script_request_handler_client.cc"
    "src/script_request_handler_client.h"
    "src/sfall_kb_helpers.cc"
    "src/sfall_kb_helpers.h"
    "src/skilldex.cc"
    "src/skilldex.h"
    "src/sound.cc"
    "src/sound.h"
    "src/sound_decoder.cc"
    "src/sound_decoder.h"
    "src/sound_effects_cache.cc"
    "src/sound_effects_cache.h"
    "src/sound_effects_list.cc"
    "src/sound_effects_list.h"
    "src/svga.cc"
    "src/svga.h"
    "src/text_font.cc"
    "src/text_font.h"
    "src/text_object.cc"
    "src/text_object.h"
    "src/tile_render.cc"
    "src/tile_render.h"
    "src/touch.cc"
    "src/touch.h"
    "src/widget.cc"
    "src/widget.h"
    "src/win32.cc"
    "src/win32.h"
    "src/window.cc"
    "src/window.h"
    "src/window_manager.cc"
    "src/window_manager.h"
    "src/window_manager_private.cc"
    "src/window_manager_private.h"
    "src/word_wrap.cc"
    "src/word_wrap.h"
    "src/worldmap_ui.cc"
    "src/worldmap_ui.h"
)

target_sources(${EXECUTABLE_NAME} PUBLIC
    "src/main.cc"
    "src/main.h"
)

if(IOS)
    target_sources(${EXECUTABLE_NAME} PUBLIC
        "src/platform/ios/paths.h"
        "src/platform/ios/paths.mm"
    )
endif()

if(WIN32)
    foreach(_f2tgt ${EXECUTABLE_NAME} f2_core f2_client)
        target_compile_definitions(${_f2tgt} PUBLIC
            _CRT_SECURE_NO_WARNINGS
            _CRT_NONSTDC_NO_WARNINGS
            NOMINMAX
            WIN32_LEAN_AND_MEAN
        )
    endforeach()
endif()

if(WIN32)
    target_link_libraries(${EXECUTABLE_NAME}
        winmm
    )
endif()

if(WIN32)
    target_sources(${EXECUTABLE_NAME} PUBLIC
        "os/windows/fallout2-ce.ico"
        "os/windows/fallout2-ce.rc"
    )
endif()

if(APPLE)
    if(IOS)
        set(RESOURCES
            "os/ios/AppIcon.xcassets"
            "os/ios/LaunchScreen.storyboard"
        )

        target_sources(${EXECUTABLE_NAME} PUBLIC ${RESOURCES})
        set_source_files_properties(${RESOURCES} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")

        set_target_properties(${EXECUTABLE_NAME} PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/os/ios/Info.plist"
            XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon"
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.alexbatalov.fallout2-ce"
            XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
        )

        set(MACOSX_BUNDLE_BUNDLE_NAME "${EXECUTABLE_NAME}")
        set(MACOSX_BUNDLE_DISPLAY_NAME "Fallout 2")
    else()
        set(RESOURCES
            "os/macos/fallout2-ce.icns"
        )

        target_sources(${EXECUTABLE_NAME} PUBLIC ${RESOURCES})
        set_source_files_properties(${RESOURCES} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")

        set_target_properties(${EXECUTABLE_NAME} PROPERTIES
            OUTPUT_NAME "Fallout II Community Edition"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/os/macos/Info.plist"
            XCODE_ATTRIBUTE_EXECUTABLE_NAME "${EXECUTABLE_NAME}"
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.alexbatalov.fallout2-ce"
        )

        set(MACOSX_BUNDLE_ICON_FILE "fallout2-ce.icns")
        set(MACOSX_BUNDLE_BUNDLE_NAME "Fallout II: Community Edition")
        set(MACOSX_BUNDLE_DISPLAY_NAME "Fallout II")
    endif()

    set(MACOSX_BUNDLE_GUI_IDENTIFIER "com.alexbatalov.fallout2-ce")
    set(MACOSX_BUNDLE_SHORT_VERSION_STRING "1.3.0")
    set(MACOSX_BUNDLE_BUNDLE_VERSION "1.3.0")
endif()

add_subdirectory("third_party/fpattern")
target_link_libraries(${EXECUTABLE_NAME} fpattern::fpattern)

if(FALLOUT_VENDORED)
    add_subdirectory("third_party/zlib")
    add_subdirectory("third_party/sdl2")
else()
    find_package(ZLIB)
    find_package(SDL2)
endif()

target_link_libraries(${EXECUTABLE_NAME} ${ZLIB_LIBRARIES})
target_include_directories(${EXECUTABLE_NAME} PRIVATE ${ZLIB_INCLUDE_DIRS})

target_link_libraries(${EXECUTABLE_NAME} ${SDL2_LIBRARIES})
target_include_directories(${EXECUTABLE_NAME} PRIVATE ${SDL2_INCLUDE_DIRS})

# Multiplayer transport on Windows: the sockets the wire viewer uses live in
# Winsock, which (unlike POSIX) is a separate link library. src/platform_net.h
# hides the API differences; this supplies the symbols. MinGW and MSVC both
# spell it ws2_32.
if(WIN32)
    target_link_libraries(${EXECUTABLE_NAME} ws2_32)
endif()

# --- Phase 1 core/client library split (REWRITE_PLAN 1.4) ---
# f2_core and f2_client are OBJECT libraries: every object still links into the
# executable, so the binary is byte-identical to the pre-split single target.
# Both get the same include dirs / usage requirements as the executable so
# compilation is unchanged. f2_core is the SDL-free simulation + shared infra
# (reused by the eventual headless/server binary); f2_client is presentation.
foreach(_f2lib f2_core f2_client)
    target_link_libraries(${_f2lib} PUBLIC fpattern::fpattern ${SDL2_LIBRARIES} ${ZLIB_LIBRARIES})
    target_include_directories(${_f2lib} PRIVATE ${ZLIB_INCLUDE_DIRS} ${SDL2_INCLUDE_DIRS})
endforeach()
target_link_libraries(${EXECUTABLE_NAME} f2_core f2_client)

# --- P5-C: core-only dedicated-server link target (p5-server-plan) ---
# f2_server links f2_core WITHOUT f2_client, proving the simulation core can be
# the basis of a headless dedicated server independent of the SDL presentation
# layer. f2_core is consumed as a STATIC ARCHIVE (built from the already-compiled
# f2_core objects — no recompile) rather than the OBJECT library, so the linker
# pulls in only the object files actually reachable from the server entry points.
# The resulting undefined-symbol set is then the TRUE client-severance surface,
# satisfied for now by abort/no-op stubs in src/server_stubs.cc. No SDL is linked:
# every SDL reference lives in f2_client .cc files, which the stubs replace.
# Desktop only (Android/iOS build a client app bundle, not a server). Windows
# included: server_net.cc goes through src/platform_net.h, the same shim the
# client's transport crosses on, so the dedicated server builds there too
# (linking ws2_32 below, like the client).
if(NOT ANDROID AND NOT IOS)
    add_library(f2_core_archive STATIC $<TARGET_OBJECTS:f2_core>)
    target_link_libraries(f2_core_archive PUBLIC fpattern::fpattern ${ZLIB_LIBRARIES})

    add_executable(f2_server
        "src/server_main.cc"
        "src/server_boot.cc"
        # Admin verbs on the control channel (save/load/saves/new). Server-only:
        # they answer the operator over the socket and take string arguments,
        # neither of which the core's int-only Command dispatch does.
        "src/server_admin.cc"
        "src/server_stubs.cc"
        "src/server_shim.cc"
        "src/server_anim.cc"
        "src/server_net.cc"
        "src/server_control.cc"
        # A1: the dedicated server's dialog ScriptRequestHandler (dialogEnter ->
        # gameDialogEnter); makes SCRIPT_REQUEST_DIALOG reach the engine below.
        "src/script_request_handler_server.cc"
        # Server worldmap driver: block-and-pump travel driver (serverLoopActive branch
        # of wmWorldMapFunc). Calls pure-sim helpers (worldmap.cc) via the intent queue.
        "src/server_worldmap.cc"
        # Shared dialog engine — client-homed but semantically core (runs
        # authoritative script procs). Server links it pending the Stage-B
        # core/client split; its renderer deps land on the labeled
        # "dialog-UI" stub block in server_stubs.cc. See DIALOG_STREAMING_PLAN.md.
        "src/game_dialog.cc"
        # Barter, for exactly the same reason and by the same precedent. A dialog
        # option can open a trade (game_dialog.cc calls inventoryOpenTrade), and
        # game_dialog.cc is linked here — so while this file was NOT, that call
        # reached the server_stubs.cc stub, which aborts(). One merchant
        # conversation killed the dedicated server and every session on it.
        # inventoryOpenTrade's serverLoopActive branch is authoritative SIM
        # (itemMoveForce / barterAttemptTransaction), so it belongs on the server;
        # its renderer deps land on the labeled "inventory-UI" stub block.
        "src/inventory_ui.cc"
    )
    target_include_directories(f2_server PRIVATE ${ZLIB_INCLUDE_DIRS} ${SDL2_INCLUDE_DIRS})
    # server_stubs.cc includes the f2_client headers it stubs (for signature
    # validation); some of those pull <SDL.h>. Add SDL's include dirs at COMPILE
    # time only — SDL is NOT linked, so no SDL code enters the server binary.
    foreach(_sdllib ${SDL2_LIBRARIES} SDL2::SDL2)
        if(TARGET ${_sdllib})
            target_include_directories(f2_server PRIVATE
                $<TARGET_PROPERTY:${_sdllib},INTERFACE_INCLUDE_DIRECTORIES>)
        endif()
    endforeach()
    target_link_libraries(f2_server f2_core_archive fpattern::fpattern ${ZLIB_LIBRARIES})
    if(WIN32)
        # ws2_32: Winsock (src/platform_net.h). winmm: timeGetTime in
        # platform_compat.cc — the client gets it transitively from SDL, which
        # the server deliberately does not link.
        target_link_libraries(f2_server ws2_32 winmm)
    endif()
endif()

if(APPLE)
    if(IOS)
        install(TARGETS ${EXECUTABLE_NAME} DESTINATION "Payload")

        set(CPACK_GENERATOR "ZIP")
        set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
        set(CPACK_PACKAGE_FILE_NAME "fallout2-ce")
        set(CPACK_ARCHIVE_FILE_EXTENSION "ipa")
    else()
        install(TARGETS ${EXECUTABLE_NAME} DESTINATION .)

        set(CPACK_GENERATOR "DragNDrop")
        set(CPACK_DMG_DISABLE_APPLICATIONS_SYMLINK ON)
        set(CPACK_PACKAGE_FILE_NAME "Fallout II Community Edition")
    endif()

    include(CPack)
endif()
