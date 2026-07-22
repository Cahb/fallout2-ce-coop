#include "opcode_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unordered_map>

#include "interpreter.h"
#include "object.h"
#include "scripts.h"

namespace fallout {

// Generated from the interpreterRegisterOpcode call sites, whose trailing comment
// is the script-language name (where a site has none, the handler name stands in).
// Safe to hardcode: opcode numbers are the frozen bytecode ABI every shipped .int
// is compiled against.
static const struct {
    int opcode;
    const char* name;
} gOpcodeNameTable[] = {
    { OPCODE_NOOP, "noop" },
    { OPCODE_PUSH, "push" },
    { OPCODE_ENTER_CRITICAL_SECTION, "enter_critical_section" },
    { OPCODE_LEAVE_CRITICAL_SECTION, "leave_critical_section" },
    { OPCODE_JUMP, "jump" },
    { OPCODE_CALL, "call" },
    { OPCODE_CALL_AT, "call_at" },
    { OPCODE_CALL_WHEN, "call_when" },
    { OPCODE_CALLSTART, "callstart" },
    { OPCODE_EXEC, "exec" },
    { OPCODE_SPAWN, "spawn" },
    { OPCODE_FORK, "fork" },
    { OPCODE_A_TO_D, "a_to_d" },
    { OPCODE_D_TO_A, "d_to_a" },
    { OPCODE_EXIT, "exit" },
    { OPCODE_DETACH, "detach" },
    { OPCODE_EXIT_PROGRAM, "exit_program" },
    { OPCODE_STOP_PROGRAM, "stop_program" },
    { OPCODE_FETCH_GLOBAL, "fetch_global" },
    { OPCODE_STORE_GLOBAL, "store_global" },
    { OPCODE_FETCH_EXTERNAL, "fetch_external" },
    { OPCODE_STORE_EXTERNAL, "store_external" },
    { OPCODE_EXPORT_VARIABLE, "export_variable" },
    { OPCODE_EXPORT_PROCEDURE, "export_procedure" },
    { OPCODE_SWAP, "swap" },
    { OPCODE_SWAPA, "swapa" },
    { OPCODE_POP, "pop" },
    { OPCODE_DUP, "dup" },
    { OPCODE_POP_RETURN, "pop_return" },
    { OPCODE_POP_EXIT, "pop_exit" },
    { OPCODE_POP_ADDRESS, "pop_address" },
    { OPCODE_POP_FLAGS, "pop_flags" },
    { OPCODE_POP_FLAGS_RETURN, "pop_flags_return" },
    { OPCODE_POP_FLAGS_EXIT, "pop_flags_exit" },
    { OPCODE_POP_FLAGS_RETURN_EXTERN, "pop_flags_return_extern" },
    { OPCODE_POP_FLAGS_EXIT_EXTERN, "pop_flags_exit_extern" },
    { OPCODE_POP_FLAGS_RETURN_VAL_EXTERN, "pop_flags_return_val_extern" },
    { OPCODE_POP_FLAGS_RETURN_VAL_EXIT, "pop_flags_return_val_exit" },
    { OPCODE_POP_FLAGS_RETURN_VAL_EXIT_EXTERN, "pop_flags_return_val_exit_extern" },
    { OPCODE_CHECK_PROCEDURE_ARGUMENT_COUNT, "check_procedure_argument_count" },
    { OPCODE_LOOKUP_PROCEDURE_BY_NAME, "lookup_procedure_by_name" },
    { OPCODE_POP_BASE, "pop_base" },
    { OPCODE_POP_TO_BASE, "pop_to_base" },
    { OPCODE_PUSH_BASE, "push_base" },
    { OPCODE_SET_GLOBAL, "set_global" },
    { OPCODE_FETCH_PROCEDURE_ADDRESS, "fetch_procedure_address" },
    { OPCODE_DUMP, "dump" },
    { OPCODE_IF, "if" },
    { OPCODE_WHILE, "while" },
    { OPCODE_STORE, "store" },
    { OPCODE_FETCH, "fetch" },
    { OPCODE_EQUAL, "equal" },
    { OPCODE_NOT_EQUAL, "not_equal" },
    { OPCODE_LESS_THAN_EQUAL, "less_than_equal" },
    { OPCODE_GREATER_THAN_EQUAL, "greater_than_equal" },
    { OPCODE_LESS_THAN, "less_than" },
    { OPCODE_GREATER_THAN, "greater_than" },
    { OPCODE_ADD, "add" },
    { OPCODE_SUB, "sub" },
    { OPCODE_MUL, "mul" },
    { OPCODE_DIV, "div" },
    { OPCODE_MOD, "mod" },
    { OPCODE_AND, "and" },
    { OPCODE_OR, "or" },
    { OPCODE_BITWISE_AND, "bitwise_and" },
    { OPCODE_BITWISE_OR, "bitwise_or" },
    { OPCODE_BITWISE_XOR, "bitwise_xor" },
    { OPCODE_BITWISE_NOT, "bitwise_not" },
    { OPCODE_FLOOR, "floor" },
    { OPCODE_NOT, "not" },
    { OPCODE_NEGATE, "negate" },
    { OPCODE_WAIT, "wait" },
    { OPCODE_CANCEL, "cancel" },
    { OPCODE_CANCEL_ALL, "cancel_all" },
    { OPCODE_START_CRITICAL, "start_critical" },
    { OPCODE_END_CRITICAL, "end_critical" },
    { 0x80A1, "op_give_exp_points" },
    { 0x80A2, "op_scr_return" },
    { 0x80A3, "op_play_sfx" },
    { 0x80A4, "op_obj_name" },
    { 0x80A5, "op_sfx_build_open_name" },
    { 0x80A6, "op_get_pc_stat" },
    { 0x80A7, "op_tile_contains_pid_obj" },
    { 0x80A8, "op_set_map_start" },
    { 0x80A9, "op_override_map_start" },
    { 0x80AA, "op_has_skill" },
    { 0x80AB, "op_using_skill" },
    { 0x80AC, "op_roll_vs_skill" },
    { 0x80AD, "op_skill_contest" },
    { 0x80AE, "op_do_check" },
    { 0x80AF, "op_success" },
    { 0x80B0, "op_critical" },
    { 0x80B1, "op_how_much" },
    { 0x80B2, "op_mark_area_known" },
    { 0x80B3, "op_reaction_influence" },
    { 0x80B4, "op_random" },
    { 0x80B5, "op_roll_dice" },
    { 0x80B6, "op_move_to" },
    { 0x80B7, "op_create_object" },
    { 0x80B8, "op_display_msg" },
    { 0x80B9, "op_script_overrides" },
    { 0x80BA, "op_obj_is_carrying_obj" },
    { 0x80BB, "op_tile_contains_obj_pid" },
    { 0x80BC, "op_self_obj" },
    { 0x80BD, "op_source_obj" },
    { 0x80BE, "op_target_obj" },
    { 0x80BF, "opGetDude" },
    { 0x80C0, "op_obj_being_used_with" },
    { 0x80C1, "op_get_local_var" },
    { 0x80C2, "op_set_local_var" },
    { 0x80C3, "op_get_map_var" },
    { 0x80C4, "op_set_map_var" },
    { 0x80C5, "op_get_global_var" },
    { 0x80C6, "op_set_global_var" },
    { 0x80C7, "op_script_action" },
    { 0x80C8, "op_obj_type" },
    { 0x80C9, "op_item_subtype" },
    { 0x80CA, "op_get_critter_stat" },
    { 0x80CB, "op_set_critter_stat" },
    { 0x80CC, "op_animate_stand_obj" },
    { 0x80CD, "animate_stand_reverse_obj" },
    { 0x80CE, "animate_move_obj_to_tile" },
    { 0x80CF, "tile_in_tile_rect" },
    { 0x80D0, "op_attack" },
    { 0x80D1, "op_make_daytime" },
    { 0x80D2, "op_tile_distance" },
    { 0x80D3, "op_tile_distance_objs" },
    { 0x80D4, "op_tile_num" },
    { 0x80D5, "op_tile_num_in_direction" },
    { 0x80D6, "op_pickup_obj" },
    { 0x80D7, "op_drop_obj" },
    { 0x80D8, "op_add_obj_to_inven" },
    { 0x80D9, "op_rm_obj_from_inven" },
    { 0x80DA, "op_wield_obj_critter" },
    { 0x80DB, "op_use_obj" },
    { 0x80DC, "op_obj_can_see_obj" },
    { 0x80DD, "op_attack" },
    { 0x80DE, "op_start_gdialog" },
    { 0x80DF, "op_end_gdialog" },
    { 0x80E0, "op_dialogue_reaction" },
    { 0x80E1, "op_metarule3" },
    { 0x80E2, "op_set_map_music" },
    { 0x80E3, "op_set_obj_visibility" },
    { 0x80E4, "op_load_map" },
    { 0x80E5, "op_wm_area_set_pos" },
    { 0x80E6, "op_set_exit_grids" },
    { 0x80E7, "op_anim_busy" },
    { 0x80E8, "op_critter_heal" },
    { 0x80E9, "op_set_light_level" },
    { 0x80EA, "op_game_time" },
    { 0x80EB, "op_game_time" },
    { 0x80EC, "op_elevation" },
    { 0x80ED, "op_kill_critter" },
    { 0x80EE, "op_kill_critter_type" },
    { 0x80EF, "op_critter_damage" },
    { 0x80F0, "op_add_timer_event" },
    { 0x80F1, "op_rm_timer_event" },
    { 0x80F2, "op_game_ticks" },
    { 0x80F3, "op_has_trait" },
    { 0x80F4, "op_destroy_object" },
    { 0x80F5, "op_obj_can_hear_obj" },
    { 0x80F6, "op_game_time_hour" },
    { 0x80F7, "op_fixed_param" },
    { 0x80F8, "op_tile_is_visible" },
    { 0x80F9, "op_dialogue_system_enter" },
    { 0x80FA, "op_action_being_used" },
    { 0x80FB, "critter_state" },
    { 0x80FC, "op_game_time_advance" },
    { 0x80FD, "op_radiation_inc" },
    { 0x80FE, "op_radiation_dec" },
    { 0x80FF, "critter_attempt_placement" },
    { 0x8100, "op_obj_pid" },
    { 0x8101, "op_cur_map_index" },
    { 0x8102, "op_critter_add_trait" },
    { 0x8103, "critter_rm_trait" },
    { 0x8104, "op_proto_data" },
    { 0x8105, "op_message_str" },
    { 0x8106, "op_critter_inven_obj" },
    { 0x8107, "op_obj_set_light_level" },
    { 0x8108, "op_scripts_request_world_map" },
    { 0x8109, "op_inven_cmds" },
    { 0x810A, "op_float_msg" },
    { 0x810B, "op_metarule" },
    { 0x810C, "op_anim" },
    { 0x810D, "op_obj_carrying_pid_obj" },
    { 0x810E, "op_reg_anim_func" },
    { 0x810F, "op_reg_anim_animate" },
    { 0x8110, "op_reg_anim_animate_reverse" },
    { 0x8111, "op_reg_anim_obj_move_to_obj" },
    { 0x8112, "op_reg_anim_obj_run_to_obj" },
    { 0x8113, "op_reg_anim_obj_move_to_tile" },
    { 0x8114, "op_reg_anim_obj_run_to_tile" },
    { 0x8115, "op_play_gmovie" },
    { 0x8116, "op_add_mult_objs_to_inven" },
    { 0x8117, "rm_mult_objs_from_inven" },
    { 0x8118, "op_month" },
    { 0x8119, "op_day" },
    { 0x811A, "op_explosion" },
    { 0x811B, "op_days_since_visited" },
    { 0x811C, "_op_gsay_start" },
    { 0x811D, "_op_gsay_end" },
    { 0x811E, "op_gsay_reply" },
    { 0x811F, "op_gsay_option" },
    { 0x8120, "op_gsay_message" },
    { 0x8121, "op_giq_option" },
    { 0x8122, "op_poison" },
    { 0x8123, "op_get_poison" },
    { 0x8124, "op_party_add" },
    { 0x8125, "op_party_remove" },
    { 0x8126, "op_reg_anim_animate_forever" },
    { 0x8127, "op_critter_injure" },
    { 0x8128, "op_is_in_combat" },
    { 0x8129, "op_gdialog_barter" },
    { 0x812A, "op_game_difficulty" },
    { 0x812B, "op_running_burning_guy" },
    { 0x812C, "op_inven_unwield" },
    { 0x812D, "op_obj_is_locked" },
    { 0x812E, "op_obj_lock" },
    { 0x812F, "op_obj_unlock" },
    { 0x8131, "op_obj_open" },
    { 0x8130, "op_obj_is_open" },
    { 0x8132, "op_obj_close" },
    { 0x8133, "op_game_ui_disable" },
    { 0x8134, "op_game_ui_enable" },
    { 0x8135, "op_game_ui_is_disabled" },
    { 0x8136, "op_gfade_out" },
    { 0x8137, "op_gfade_in" },
    { 0x8138, "op_item_caps_total" },
    { 0x8139, "op_item_caps_adjust" },
    { 0x813A, "op_anim_action_frame" },
    { 0x813B, "op_reg_anim_play_sfx" },
    { 0x813C, "op_critter_mod_skill" },
    { 0x813D, "op_sfx_build_char_name" },
    { 0x813E, "op_sfx_build_ambient_name" },
    { 0x813F, "op_sfx_build_interface_name" },
    { 0x8140, "op_sfx_build_item_name" },
    { 0x8141, "op_sfx_build_weapon_name" },
    { 0x8142, "op_sfx_build_scenery_name" },
    { 0x8143, "op_attack_setup" },
    { 0x8144, "op_destroy_mult_objs" },
    { 0x8145, "op_use_obj_on_obj" },
    { 0x8146, "op_endgame_slideshow" },
    { 0x8147, "op_move_obj_inven_to_obj" },
    { 0x8148, "op_endgame_movie" },
    { 0x8149, "op_obj_art_fid" },
    { 0x814A, "op_art_anim" },
    { 0x814B, "op_party_member_obj" },
    { 0x814C, "op_rotation_to_tile" },
    { 0x814D, "op_jam_lock" },
    { 0x814E, "op_gdialog_set_barter_mod" },
    { 0x814F, "op_combat_difficulty" },
    { 0x8150, "op_obj_on_screen" },
    { 0x8151, "op_critter_is_fleeing" },
    { 0x8152, "op_critter_set_flee_state" },
    { 0x8153, "op_terminate_combat" },
    { 0x8154, "op_debug_msg" },
    { 0x8155, "op_critter_stop_attacking" },
    { 0x806A, "opFillWin3x3" },
    { 0x808C, "opDeleteButton" },
    { 0x8086, "opAddButton" },
    { 0x8088, "opAddButtonFlag" },
    { 0x8087, "opAddButtonText" },
    { 0x8089, "opAddButtonGfx" },
    { 0x808A, "opAddButtonProc" },
    { 0x808B, "opAddButtonRightProc" },
    { 0x8067, "opShowWin" },
    { 0x8068, "opFillWin" },
    { 0x8069, "opFillRect" },
    { 0x8072, "opPrint" },
    { 0x8073, "opFormat" },
    { 0x8074, "opPrintRect" },
    { 0x8075, "opSetFont" },
    { 0x8076, "opSetTextFlags" },
    { 0x8077, "opSetTextColor" },
    { 0x8078, "opSetHighlightColor" },
    { 0x8064, "opSelect" },
    { 0x806B, "opDisplay" },
    { 0x806D, "opDisplayRaw" },
    { 0x806C, "opDisplayGfx" },
    { 0x806F, "opFadeIn" },
    { 0x8070, "opFadeOut" },
    { 0x807A, "opPlayMovie" },
    { 0x807B, "opSetMovieFlags" },
    { 0x807C, "opPlayMovieRect" },
    { 0x8079, "opStopMovie" },
    { 0x807F, "opAddRegion" },
    { 0x8080, "opAddRegionFlag" },
    { 0x8081, "opAddRegionProc" },
    { 0x8082, "opAddRegionRightProc" },
    { 0x8083, "opDeleteRegion" },
    { 0x8084, "opActivateRegion" },
    { 0x8085, "opCheckRegion" },
    { 0x8062, "opCreateWin" },
    { 0x8063, "opDeleteWin" },
    { 0x8065, "opResizeWin" },
    { 0x8066, "opScaleWin" },
    { 0x804E, "opSayStart" },
    { 0x804F, "opSayStartPos" },
    { 0x8050, "opSayReplyTitle" },
    { 0x8051, "opSayGoToReply" },
    { 0x8053, "opSayReply" },
    { 0x8052, "opSayOption" },
    { 0x804D, "opSayEnd" },
    { 0x804C, "opSayQuit" },
    { 0x8054, "opSayMessage" },
    { 0x8055, "opSayReplyWindow" },
    { 0x8056, "opSayOptionWindow" },
    { 0x805F, "opSayReplyFlags" },
    { 0x8060, "opSayOptionFlags" },
    { 0x8057, "opSayBorder" },
    { 0x8058, "opSayScrollUp" },
    { 0x8059, "opSayScrollDown" },
    { 0x805A, "opSaySetSpacing" },
    { 0x805B, "opSayOptionColor" },
    { 0x805C, "opSayReplyColor" },
    { 0x805D, "opSayRestart" },
    { 0x805E, "opSayGetLastPos" },
    { 0x8061, "opSayMessageTimeout" },
    { 0x8071, "opGotoXY" },
    { 0x808D, "opHideMouse" },
    { 0x808E, "opShowMouse" },
    { 0x8090, "opRefreshMouse" },
    { 0x808F, "opMouseShape" },
    { 0x8091, "opSetGlobalMouseFunc" },
    { 0x806E, "opLoadPaletteTable" },
    { 0x8092, "opAddNamedEvent" },
    { 0x8093, "opAddNamedHandler" },
    { 0x8094, "opClearNamed" },
    { 0x8095, "opSignalNamed" },
    { 0x8096, "opAddKey" },
    { 0x8097, "opDeleteKey" },
    { 0x8098, "opSoundPlay" },
    { 0x8099, "opSoundPause" },
    { 0x809A, "opSoundResume" },
    { 0x809B, "opSoundStop" },
    { 0x809C, "opSoundRewind" },
    { 0x809D, "opSoundDelete" },
    { 0x809E, "opSetOneOptPause" },
    { 0x809F, "opSelectFileList" },
    { 0x80A0, "opTokenize" },
};

static std::unordered_map<int, const char*>* gOpcodeNames = nullptr;

const char* opcodeGetName(int opcode)
{
    if (gOpcodeNames == nullptr) {
        gOpcodeNames = new std::unordered_map<int, const char*>();
        for (const auto& entry : gOpcodeNameTable) {
            (*gOpcodeNames)[entry.opcode & 0x3FF] = entry.name;
        }
    }

    auto it = gOpcodeNames->find(opcode & 0x3FF);
    return it != gOpcodeNames->end() ? it->second : nullptr;
}

static char gTraceFilter[256];
static bool gTraceAll = false;

static bool opcodeTraceMatches(const char* name)
{
    if (gTraceAll) {
        return true;
    }
    if (name == nullptr) {
        return false; // an unnamed opcode is only reachable via "all"
    }

    const char* cursor = gTraceFilter;
    while (*cursor != 0) {
        const char* comma = strchr(cursor, ',');
        size_t len = comma != nullptr ? (size_t)(comma - cursor) : strlen(cursor);
        if (len != 0) {
            char needle[64];
            if (len >= sizeof(needle)) {
                len = sizeof(needle) - 1;
            }
            memcpy(needle, cursor, len);
            needle[len] = 0;
            if (strstr(name, needle) != nullptr) {
                return true;
            }
        }
        if (comma == nullptr) {
            break;
        }
        cursor = comma + 1;
    }

    return false;
}

// Pre-hook: the operand stack is UNTOUCHED here, so the opcode's arguments can be
// read without disturbing them. Stack order is the reverse of the source call —
// depth 0 is the LAST argument.
static void opcodeTracePre(int opcode, Program* program)
{
    const char* name = opcodeGetName(opcode);
    if (!opcodeTraceMatches(name)) {
        return;
    }

    char args[192];
    args[0] = 0;
    int offset = 0;
    for (int depth = 0; depth < 4; depth++) {
        ProgramValue value;
        if (!programStackPeekValue(program, depth, &value)) {
            break;
        }

        const char* sep = depth != 0 ? ", " : "";
        int written;
        if (value.opcode == VALUE_TYPE_INT) {
            written = snprintf(args + offset, sizeof(args) - offset, "%s%d", sep, value.integerValue);
        } else if (value.opcode == VALUE_TYPE_PTR) {
            // Objects are the common pointer operand, and naming the pid is what
            // makes a line readable ("which item?", "which critter?").
            Object* obj = static_cast<Object*>(value.pointerValue);
            written = snprintf(args + offset, sizeof(args) - offset, "%sobj(pid=%d net=%d)",
                sep, obj != nullptr ? obj->pid : -1, obj != nullptr ? obj->netId : -1);
        } else {
            written = snprintf(args + offset, sizeof(args) - offset, "%s<type %x>", sep, value.opcode);
        }

        if (written <= 0 || offset + written >= (int)sizeof(args)) {
            break;
        }
        offset += written;
    }

    Object* self = scriptGetSelf(program);
    fprintf(stderr, "[op] %-16s %-28s args[%s] self=pid%d\n",
        program != nullptr && program->name != nullptr ? program->name : "?",
        name != nullptr ? name : "<unnamed>",
        args,
        self != nullptr ? self->pid : -1);
}

void opcodeTraceInstall()
{
    const char* filter = getenv("F2_TRACE_OPCODE");
    if (filter == nullptr) {
        return; // nothing registered -> original dispatch path -> goldens untouched
    }

    gTraceAll = filter[0] == 0 || strcmp(filter, "all") == 0;
    snprintf(gTraceFilter, sizeof(gTraceFilter), "%s", filter);

    interpreterAddOpcodePreHook(-1, opcodeTracePre); // -1 = every opcode
    fprintf(stderr, "[op] opcode trace ON (filter=%s)\n", gTraceAll ? "all" : gTraceFilter);
}

} // namespace fallout
