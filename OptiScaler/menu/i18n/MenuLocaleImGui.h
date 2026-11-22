#pragma once

// Shadows ImGui:: in the menu's sources: the define at the end sends the calls here, which translate the text and pass
// on. The menu's code stays as it is, which keeps merges with upstream simple. Functions with no text come through the
// using-directive. A wrapped name hides all of ImGui's overloads of it.

#include "MenuLocale.h"

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

#include <cstdarg>
#include <cstring>
#include <vector>

namespace MenuLocaleImGui
{
using namespace ::ImGui;

// Text("%s", name) is how lists show their names, so the argument gets translated too. Text with no percent sign
// is no format, and its translation is drawn as it is.
#define MENU_LOCALE_FORMATTED(call_v, call_plain, ...)                                                                 \
    va_list args;                                                                                                      \
    va_start(args, fmt);                                                                                               \
    if (fmt[0] == '%' && fmt[1] == 's' && fmt[2] == 0)                                                                 \
        call_plain(__VA_ARGS__ "%s", MenuLocale::Text(va_arg(args, const char*)));                                     \
    else if (strchr(fmt, '%') == nullptr)                                                                              \
        call_plain(__VA_ARGS__ "%s", MenuLocale::Text(fmt));                                                           \
    else                                                                                                               \
        call_v(__VA_ARGS__ MenuLocale::Text(fmt), args);                                                               \
    va_end(args)

inline void Text(const char* fmt, ...) { MENU_LOCALE_FORMATTED(::ImGui::TextV, ::ImGui::Text); }
inline void TextDisabled(const char* fmt, ...) { MENU_LOCALE_FORMATTED(::ImGui::TextDisabledV, ::ImGui::TextDisabled); }
inline void TextWrapped(const char* fmt, ...) { MENU_LOCALE_FORMATTED(::ImGui::TextWrappedV, ::ImGui::TextWrapped); }
inline void SetTooltip(const char* fmt, ...) { MENU_LOCALE_FORMATTED(::ImGui::SetTooltipV, ::ImGui::SetTooltip); }
inline void SetItemTooltip(const char* fmt, ...)
{
    MENU_LOCALE_FORMATTED(::ImGui::SetItemTooltipV, ::ImGui::SetItemTooltip);
}
inline void TextColored(const ImVec4& col, const char* fmt, ...)
{
    MENU_LOCALE_FORMATTED(::ImGui::TextColoredV, ::ImGui::TextColored, col, );
}
#undef MENU_LOCALE_FORMATTED

inline void TextUnformatted(const char* text, const char* text_end = nullptr)
{
    if (text_end == nullptr)
    {
        ::ImGui::TextUnformatted(MenuLocale::Text(text));
        return;
    }

    if (const char* translated = MenuLocale::TextOrNull({ text, (size_t) (text_end - text) }))
        ::ImGui::TextUnformatted(translated);
    else
        ::ImGui::TextUnformatted(text, text_end);
}

inline ImVec2 CalcTextSize(const char* text, const char* text_end = nullptr, bool hide_text_after_double_hash = false,
                           float wrap_width = -1.0f)
{
    if (text_end == nullptr)
        return ::ImGui::CalcTextSize(MenuLocale::Text(text), nullptr, hide_text_after_double_hash, wrap_width);

    const char* translated = MenuLocale::TextOrNull({ text, (size_t) (text_end - text) });
    return translated != nullptr ? ::ImGui::CalcTextSize(translated, nullptr, hide_text_after_double_hash, wrap_width)
                                 : ::ImGui::CalcTextSize(text, text_end, hide_text_after_double_hash, wrap_width);
}

inline void SeparatorText(const char* label) { ::ImGui::SeparatorText(MenuLocale::Text(label)); }

inline bool Begin(const char* name, bool* p_open = nullptr, ImGuiWindowFlags flags = 0)
{
    return ::ImGui::Begin(MenuLocale::Label(name), p_open, flags);
}
inline bool Button(const char* label, const ImVec2& size = ImVec2(0, 0))
{
    return ::ImGui::Button(MenuLocale::Label(label), size);
}
inline bool SmallButton(const char* label) { return ::ImGui::SmallButton(MenuLocale::Label(label)); }
inline bool Checkbox(const char* label, bool* v) { return ::ImGui::Checkbox(MenuLocale::Label(label), v); }
inline bool CheckboxFlags(const char* label, int* flags, int flags_value)
{
    return ::ImGui::CheckboxFlags(MenuLocale::Label(label), flags, flags_value);
}
inline bool CheckboxFlags(const char* label, unsigned int* flags, unsigned int flags_value)
{
    return ::ImGui::CheckboxFlags(MenuLocale::Label(label), flags, flags_value);
}
inline bool RadioButton(const char* label, bool active) { return ::ImGui::RadioButton(MenuLocale::Label(label), active); }
inline bool RadioButton(const char* label, int* v, int v_button)
{
    return ::ImGui::RadioButton(MenuLocale::Label(label), v, v_button);
}
inline void TextLinkOpenURL(const char* label, const char* url = nullptr)
{
    ::ImGui::TextLinkOpenURL(MenuLocale::Label(label), url);
}

inline bool Selectable(const char* label, bool selected = false, ImGuiSelectableFlags flags = 0,
                       const ImVec2& size = ImVec2(0, 0))
{
    return ::ImGui::Selectable(MenuLocale::Label(label), selected, flags, size);
}
inline bool Selectable(const char* label, bool* p_selected, ImGuiSelectableFlags flags = 0,
                       const ImVec2& size = ImVec2(0, 0))
{
    return ::ImGui::Selectable(MenuLocale::Label(label), p_selected, flags, size);
}

inline bool BeginCombo(const char* label, const char* preview_value, ImGuiComboFlags flags = 0)
{
    return ::ImGui::BeginCombo(MenuLocale::Label(label), preview_value != nullptr ? MenuLocale::Text(preview_value) : nullptr,
                               flags);
}
inline bool Combo(const char* label, int* current_item, const char* const items[], int items_count,
                  int popup_max_height_in_items = -1)
{
    return ::ImGui::Combo(
        MenuLocale::Label(label), current_item,
        [](void* data, int index) { return MenuLocale::Text(static_cast<const char* const*>(data)[index]); },
        const_cast<char**>(items), items_count, popup_max_height_in_items);
}
inline bool Combo(const char* label, int* current_item, const char* items_separated_by_zeros,
                  int popup_max_height_in_items = -1)
{
    std::vector<const char*> items;
    for (const char* at = items_separated_by_zeros; *at != 0; at += strlen(at) + 1)
        items.push_back(at);

    return Combo(label, current_item, items.data(), (int) items.size(), popup_max_height_in_items);
}
inline bool Combo(const char* label, int* current_item, const char* (*getter)(void*, int), void* user_data,
                  int items_count, int popup_max_height_in_items = -1)
{
    return ::ImGui::Combo(MenuLocale::Label(label), current_item, getter, user_data, items_count,
                          popup_max_height_in_items);
}

inline bool TreeNode(const char* label) { return ::ImGui::TreeNode(MenuLocale::Label(label)); }
inline bool TreeNode(const char* str_id, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    const bool open = ::ImGui::TreeNodeV(str_id, MenuLocale::Text(fmt), args);
    va_end(args);
    return open;
}
inline bool TreeNodeEx(const char* label, ImGuiTreeNodeFlags flags = 0)
{
    return ::ImGui::TreeNodeEx(MenuLocale::Label(label), flags);
}

inline bool SliderFloat(const char* label, float* v, float v_min, float v_max, const char* format = "%.3f",
                        ImGuiSliderFlags flags = 0)
{
    return ::ImGui::SliderFloat(MenuLocale::Label(label), v, v_min, v_max, MenuLocale::Text(format), flags);
}
inline bool SliderInt(const char* label, int* v, int v_min, int v_max, const char* format = "%d",
                      ImGuiSliderFlags flags = 0)
{
    return ::ImGui::SliderInt(MenuLocale::Label(label), v, v_min, v_max, MenuLocale::Text(format), flags);
}
inline bool SliderScalar(const char* label, ImGuiDataType data_type, void* p_data, const void* p_min, const void* p_max,
                         const char* format = nullptr, ImGuiSliderFlags flags = 0)
{
    return ::ImGui::SliderScalar(MenuLocale::Label(label), data_type, p_data, p_min, p_max,
                                 format != nullptr ? MenuLocale::Text(format) : nullptr, flags);
}
inline bool InputInt(const char* label, int* v, int step = 1, int step_fast = 100, ImGuiInputTextFlags flags = 0)
{
    return ::ImGui::InputInt(MenuLocale::Label(label), v, step, step_fast, flags);
}
inline bool InputFloat(const char* label, float* v, float step = 0.0f, float step_fast = 0.0f,
                       const char* format = "%.3f", ImGuiInputTextFlags flags = 0)
{
    return ::ImGui::InputFloat(MenuLocale::Label(label), v, step, step_fast, format, flags);
}
inline bool InputScalar(const char* label, ImGuiDataType data_type, void* p_data, const void* p_step = nullptr,
                        const void* p_step_fast = nullptr, const char* format = nullptr, ImGuiInputTextFlags flags = 0)
{
    return ::ImGui::InputScalar(MenuLocale::Label(label), data_type, p_data, p_step, p_step_fast, format, flags);
}
inline bool InputTextWithHint(const char* label, const char* hint, char* buf, size_t buf_size,
                              ImGuiInputTextFlags flags = 0, ImGuiInputTextCallback callback = nullptr,
                              void* user_data = nullptr)
{
    return ::ImGui::InputTextWithHint(MenuLocale::Label(label), MenuLocale::Text(hint), buf, buf_size, flags, callback,
                                      user_data);
}
inline bool ColorEdit3(const char* label, float col[3], ImGuiColorEditFlags flags = 0)
{
    return ::ImGui::ColorEdit3(MenuLocale::Label(label), col, flags);
}

inline void TableSetupColumn(const char* label, ImGuiTableColumnFlags flags = 0, float init_width_or_weight = 0.0f,
                             ImGuiID user_id = 0)
{
    ::ImGui::TableSetupColumn(label != nullptr ? MenuLocale::Text(label) : nullptr, flags, init_width_or_weight, user_id);
}
} // namespace MenuLocaleImGui

// In the file that includes this, below all of its includes.
#define ImGui MenuLocaleImGui
