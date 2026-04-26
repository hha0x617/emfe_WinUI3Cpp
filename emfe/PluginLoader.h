// PluginLoader.h - Dynamic loader for emfe plugin DLLs
#pragma once

#include <windows.h>
#include <string>
#include <stdexcept>
#include "emfe_plugin.h"

class PluginLoader {
public:
    PluginLoader() = default;
    ~PluginLoader() { Unload(); }

    PluginLoader(const PluginLoader&) = delete;
    PluginLoader& operator=(const PluginLoader&) = delete;

    bool Load(const std::wstring& dllPath) {
        Unload();
        m_hModule = LoadLibraryW(dllPath.c_str());
        if (!m_hModule) return false;

        // Resolve all Phase 1 functions
        #define RESOLVE(name) \
            name = reinterpret_cast<decltype(::name)*>(GetProcAddress(m_hModule, #name)); \
            if (!name) { Unload(); return false; }

        RESOLVE(emfe_negotiate)
        RESOLVE(emfe_get_board_info)
        RESOLVE(emfe_create)
        RESOLVE(emfe_destroy)
        RESOLVE(emfe_set_console_char_callback)
        RESOLVE(emfe_set_state_change_callback)
        RESOLVE(emfe_set_diagnostic_callback)
        RESOLVE(emfe_get_register_defs)
        RESOLVE(emfe_get_registers)
        RESOLVE(emfe_set_registers)
        RESOLVE(emfe_peek_byte)
        RESOLVE(emfe_peek_word)
        RESOLVE(emfe_peek_long)
        RESOLVE(emfe_poke_byte)
        RESOLVE(emfe_poke_word)
        RESOLVE(emfe_poke_long)
        RESOLVE(emfe_peek_range)
        RESOLVE(emfe_get_memory_size)
        RESOLVE(emfe_disassemble_one)
        RESOLVE(emfe_disassemble_range)
        RESOLVE(emfe_step)
        RESOLVE(emfe_step_over)
        RESOLVE(emfe_step_out)
        RESOLVE(emfe_run)
        RESOLVE(emfe_stop)
        RESOLVE(emfe_reset)
        RESOLVE(emfe_get_state)
        RESOLVE(emfe_get_instruction_count)
        RESOLVE(emfe_get_cycle_count)
        RESOLVE(emfe_add_breakpoint)
        RESOLVE(emfe_remove_breakpoint)
        RESOLVE(emfe_enable_breakpoint)
        RESOLVE(emfe_set_breakpoint_condition)
        RESOLVE(emfe_clear_breakpoints)
        RESOLVE(emfe_get_breakpoints)
        RESOLVE(emfe_add_watchpoint)
        RESOLVE(emfe_remove_watchpoint)
        RESOLVE(emfe_enable_watchpoint)
        RESOLVE(emfe_set_watchpoint_condition)
        RESOLVE(emfe_clear_watchpoints)
        RESOLVE(emfe_get_watchpoints)
        RESOLVE(emfe_get_call_stack)
        RESOLVE(emfe_get_program_range)
        RESOLVE(emfe_get_framebuffer_info)
        RESOLVE(emfe_get_palette)
        RESOLVE(emfe_push_key)
        RESOLVE(emfe_push_mouse_absolute)
        RESOLVE(emfe_push_mouse_button)
        RESOLVE(emfe_load_elf)
        RESOLVE(emfe_load_binary)
        RESOLVE(emfe_load_srec)
        RESOLVE(emfe_get_last_error)
        RESOLVE(emfe_get_setting_defs)
        RESOLVE(emfe_get_setting)
        RESOLVE(emfe_set_setting)
        RESOLVE(emfe_apply_settings)
        RESOLVE(emfe_get_applied_setting)
        RESOLVE(emfe_get_list_item_defs)
        RESOLVE(emfe_get_list_item_count)
        RESOLVE(emfe_get_list_item_field)
        RESOLVE(emfe_set_list_item_field)
        RESOLVE(emfe_add_list_item)
        RESOLVE(emfe_remove_list_item)
        RESOLVE(emfe_save_settings)
        RESOLVE(emfe_load_settings)
        RESOLVE(emfe_set_data_dir)
        RESOLVE(emfe_send_char)
        RESOLVE(emfe_send_string)
        RESOLVE(emfe_release_string)
        // Optional export: older plugin DLLs (pre-CAP_CONSOLE_TX_SPACE) don't
        // ship it, so missing it mustn't fail the load.  Host code checks
        // the pointer before calling.
        emfe_console_tx_space = reinterpret_cast<decltype(::emfe_console_tx_space)*>(
            ::GetProcAddress(m_hModule, "emfe_console_tx_space"));
        // Optional export: older plugin DLLs without LIST pending support
        // simply won't have the pending marker; host checks the pointer
        // before calling and falls back to "not pending".
        emfe_is_list_pending = reinterpret_cast<decltype(::emfe_is_list_pending)*>(
            ::GetProcAddress(m_hModule, "emfe_is_list_pending"));
        // Optional export: per-register flag-bit decomposition for checkbox
        // UI on flags registers. Fall back to plain hex display when absent.
        emfe_get_register_flag_defs = reinterpret_cast<decltype(::emfe_get_register_flag_defs)*>(
            ::GetProcAddress(m_hModule, "emfe_get_register_flag_defs"));

        #undef RESOLVE
        return true;
    }

    void Unload() {
        if (m_hModule) {
            FreeLibrary(m_hModule);
            m_hModule = nullptr;
        }
        // Zero all function pointers
        emfe_negotiate = nullptr;
        emfe_get_board_info = nullptr;
        emfe_create = nullptr;
        emfe_destroy = nullptr;
        emfe_set_console_char_callback = nullptr;
        emfe_set_state_change_callback = nullptr;
        emfe_set_diagnostic_callback = nullptr;
        emfe_get_register_defs = nullptr;
        emfe_get_registers = nullptr;
        emfe_set_registers = nullptr;
        emfe_peek_byte = nullptr;
        emfe_peek_word = nullptr;
        emfe_peek_long = nullptr;
        emfe_poke_byte = nullptr;
        emfe_poke_word = nullptr;
        emfe_poke_long = nullptr;
        emfe_peek_range = nullptr;
        emfe_get_memory_size = nullptr;
        emfe_disassemble_one = nullptr;
        emfe_disassemble_range = nullptr;
        emfe_step = nullptr;
        emfe_step_over = nullptr;
        emfe_step_out = nullptr;
        emfe_run = nullptr;
        emfe_stop = nullptr;
        emfe_reset = nullptr;
        emfe_get_state = nullptr;
        emfe_get_instruction_count = nullptr;
        emfe_get_cycle_count = nullptr;
        emfe_add_breakpoint = nullptr;
        emfe_remove_breakpoint = nullptr;
        emfe_enable_breakpoint = nullptr;
        emfe_set_breakpoint_condition = nullptr;
        emfe_clear_breakpoints = nullptr;
        emfe_get_breakpoints = nullptr;
        emfe_add_watchpoint = nullptr;
        emfe_remove_watchpoint = nullptr;
        emfe_enable_watchpoint = nullptr;
        emfe_set_watchpoint_condition = nullptr;
        emfe_clear_watchpoints = nullptr;
        emfe_get_watchpoints = nullptr;
        emfe_get_call_stack = nullptr;
        emfe_get_program_range = nullptr;
        emfe_get_framebuffer_info = nullptr;
        emfe_get_palette = nullptr;
        emfe_push_key = nullptr;
        emfe_push_mouse_absolute = nullptr;
        emfe_push_mouse_button = nullptr;
        emfe_load_elf = nullptr;
        emfe_load_binary = nullptr;
        emfe_load_srec = nullptr;
        emfe_get_last_error = nullptr;
        emfe_get_setting_defs = nullptr;
        emfe_get_setting = nullptr;
        emfe_set_setting = nullptr;
        emfe_apply_settings = nullptr;
        emfe_get_applied_setting = nullptr;
        emfe_get_list_item_defs = nullptr;
        emfe_get_list_item_count = nullptr;
        emfe_get_list_item_field = nullptr;
        emfe_set_list_item_field = nullptr;
        emfe_add_list_item = nullptr;
        emfe_remove_list_item = nullptr;
        emfe_is_list_pending = nullptr;
        emfe_get_register_flag_defs = nullptr;
        emfe_save_settings = nullptr;
        emfe_load_settings = nullptr;
        emfe_set_data_dir = nullptr;
        emfe_send_char = nullptr;
        emfe_send_string = nullptr;
        emfe_console_tx_space = nullptr;
        emfe_release_string = nullptr;
    }

    bool IsLoaded() const { return m_hModule != nullptr; }

    // Function pointers (public for direct access)
    decltype(::emfe_negotiate)*                 emfe_negotiate = nullptr;
    decltype(::emfe_get_board_info)*            emfe_get_board_info = nullptr;
    decltype(::emfe_create)*                    emfe_create = nullptr;
    decltype(::emfe_destroy)*                   emfe_destroy = nullptr;
    decltype(::emfe_set_console_char_callback)* emfe_set_console_char_callback = nullptr;
    decltype(::emfe_set_state_change_callback)* emfe_set_state_change_callback = nullptr;
    decltype(::emfe_set_diagnostic_callback)*   emfe_set_diagnostic_callback = nullptr;
    decltype(::emfe_get_register_defs)*         emfe_get_register_defs = nullptr;
    decltype(::emfe_get_registers)*             emfe_get_registers = nullptr;
    decltype(::emfe_set_registers)*             emfe_set_registers = nullptr;
    decltype(::emfe_peek_byte)*                 emfe_peek_byte = nullptr;
    decltype(::emfe_peek_word)*                 emfe_peek_word = nullptr;
    decltype(::emfe_peek_long)*                 emfe_peek_long = nullptr;
    decltype(::emfe_poke_byte)*                 emfe_poke_byte = nullptr;
    decltype(::emfe_poke_word)*                 emfe_poke_word = nullptr;
    decltype(::emfe_poke_long)*                 emfe_poke_long = nullptr;
    decltype(::emfe_peek_range)*                emfe_peek_range = nullptr;
    decltype(::emfe_get_memory_size)*           emfe_get_memory_size = nullptr;
    decltype(::emfe_disassemble_one)*           emfe_disassemble_one = nullptr;
    decltype(::emfe_disassemble_range)*         emfe_disassemble_range = nullptr;
    decltype(::emfe_step)*                      emfe_step = nullptr;
    decltype(::emfe_step_over)*                 emfe_step_over = nullptr;
    decltype(::emfe_step_out)*                  emfe_step_out = nullptr;
    decltype(::emfe_run)*                       emfe_run = nullptr;
    decltype(::emfe_stop)*                      emfe_stop = nullptr;
    decltype(::emfe_reset)*                     emfe_reset = nullptr;
    decltype(::emfe_get_state)*                 emfe_get_state = nullptr;
    decltype(::emfe_get_instruction_count)*     emfe_get_instruction_count = nullptr;
    decltype(::emfe_get_cycle_count)*           emfe_get_cycle_count = nullptr;
    decltype(::emfe_add_breakpoint)*            emfe_add_breakpoint = nullptr;
    decltype(::emfe_remove_breakpoint)*         emfe_remove_breakpoint = nullptr;
    decltype(::emfe_enable_breakpoint)*         emfe_enable_breakpoint = nullptr;
    decltype(::emfe_set_breakpoint_condition)*  emfe_set_breakpoint_condition = nullptr;
    decltype(::emfe_clear_breakpoints)*         emfe_clear_breakpoints = nullptr;
    decltype(::emfe_get_breakpoints)*           emfe_get_breakpoints = nullptr;
    decltype(::emfe_add_watchpoint)*            emfe_add_watchpoint = nullptr;
    decltype(::emfe_remove_watchpoint)*         emfe_remove_watchpoint = nullptr;
    decltype(::emfe_enable_watchpoint)*         emfe_enable_watchpoint = nullptr;
    decltype(::emfe_set_watchpoint_condition)*   emfe_set_watchpoint_condition = nullptr;
    decltype(::emfe_clear_watchpoints)*         emfe_clear_watchpoints = nullptr;
    decltype(::emfe_get_watchpoints)*           emfe_get_watchpoints = nullptr;
    decltype(::emfe_get_call_stack)*            emfe_get_call_stack = nullptr;
    decltype(::emfe_get_program_range)*         emfe_get_program_range = nullptr;
    decltype(::emfe_get_framebuffer_info)*      emfe_get_framebuffer_info = nullptr;
    decltype(::emfe_get_palette)*               emfe_get_palette = nullptr;
    decltype(::emfe_push_key)*                  emfe_push_key = nullptr;
    decltype(::emfe_push_mouse_absolute)*       emfe_push_mouse_absolute = nullptr;
    decltype(::emfe_push_mouse_button)*         emfe_push_mouse_button = nullptr;
    decltype(::emfe_load_elf)*                  emfe_load_elf = nullptr;
    decltype(::emfe_load_binary)*              emfe_load_binary = nullptr;
    decltype(::emfe_load_srec)*                emfe_load_srec = nullptr;
    decltype(::emfe_get_last_error)*            emfe_get_last_error = nullptr;
    decltype(::emfe_get_setting_defs)*         emfe_get_setting_defs = nullptr;
    decltype(::emfe_get_setting)*              emfe_get_setting = nullptr;
    decltype(::emfe_set_setting)*              emfe_set_setting = nullptr;
    decltype(::emfe_apply_settings)*           emfe_apply_settings = nullptr;
    decltype(::emfe_get_applied_setting)*      emfe_get_applied_setting = nullptr;
    decltype(::emfe_get_list_item_defs)*       emfe_get_list_item_defs = nullptr;
    decltype(::emfe_get_list_item_count)*      emfe_get_list_item_count = nullptr;
    decltype(::emfe_get_list_item_field)*      emfe_get_list_item_field = nullptr;
    decltype(::emfe_set_list_item_field)*      emfe_set_list_item_field = nullptr;
    decltype(::emfe_add_list_item)*            emfe_add_list_item = nullptr;
    decltype(::emfe_remove_list_item)*         emfe_remove_list_item = nullptr;
    decltype(::emfe_is_list_pending)*          emfe_is_list_pending = nullptr;
    decltype(::emfe_get_register_flag_defs)*   emfe_get_register_flag_defs = nullptr;
    decltype(::emfe_save_settings)*            emfe_save_settings = nullptr;
    decltype(::emfe_load_settings)*            emfe_load_settings = nullptr;
    decltype(::emfe_set_data_dir)*             emfe_set_data_dir = nullptr;
    decltype(::emfe_send_char)*                 emfe_send_char = nullptr;
    decltype(::emfe_send_string)*               emfe_send_string = nullptr;
    decltype(::emfe_console_tx_space)*          emfe_console_tx_space = nullptr;
    decltype(::emfe_release_string)*            emfe_release_string = nullptr;

private:
    HMODULE m_hModule = nullptr;
};
