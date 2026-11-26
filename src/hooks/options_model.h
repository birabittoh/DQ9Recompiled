// The Options pages' content, shared by the menus that show them: the title
// menu (options_row.cpp) and the camp menu's Misc. list (camp_options.cpp).
//
// The rows and their values are the runtime menu's (title_menu.h), so every
// menu shows the same settings; the labels are translated here
// (res/lang/*.toml [options]), English when a key is missing, and the rows
// are grouped into the pages' own categories (kLayout).
#pragma once

#include <cctype>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "battle_multipliers.h"
#include "lang.h"
#include "title_hooks.h"
#include "title_menu.h"

namespace dq9::options {

// A page is a section path: "" the categories, "Controls" a category,
// "Controls/Keyboard" one of its own pages. A page lists its sub-pages
// (Category rows, to open them), then its options.
struct Row {
    enum class Kind { Category, Item, Back } kind = Kind::Back;
    std::string section;   // Category: the page it opens
    NdsTitleMenuRow item;  // Item
};

struct Model {
    std::string section;  // the page shown
    std::vector<Row> rows;
    std::map<std::string, uint32_t> cursor;  // per page
    // A page with more rows than fit (or with a break in kLayout) shows them
    // a part at a time: the part shown, per page, and (BuildRows) how many
    // parts the page has and the most rows a part has, Back included.
    std::map<std::string, uint32_t> part;
    uint32_t parts = 1;
    uint32_t tallest = 0;
    // Values asked for and not yet in the frontend's rows.
    std::map<std::string, int> pending;
    std::string signature;  // of the rows as the page last drew them
};

// The page `section` is in ("" for a category).
inline std::string Parent(const std::string& section) {
    const size_t slash = section.rfind('/');
    return slash == std::string::npos ? std::string() : section.substr(0, slash);
}

// A page's own name: the last part of its path.
inline std::string PageName(const std::string& section) {
    const size_t slash = section.rfind('/');
    return slash == std::string::npos ? section : section.substr(slash + 1);
}

inline std::string Slug(const std::string& text) {
    std::string out;
    for (const char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c)))
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        else if (!out.empty() && out.back() != '_')
            out += '_';
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out;
}

// [options] `key` in the game's language, or `english`.
inline std::string Text(const std::string& key, const std::string& english) {
    const char* text = dq9::Tr("options", key.c_str(),
                               dq9::TextLanguage());
    return *text ? text : english;
}

inline bool IsNetplayToggle(const NdsTitleMenuRow& row) {
    return row.key == "netplay.enabled";
}

// The one Action row the pages show: the Discord sign-in (sign-out when
// signed in). Its label follows the account. The frontend enables it only
// while the lobby server wants a sign-in, so it is shown only then.
inline bool IsNetplayAccount(const NdsTitleMenuRow& row) {
    return row.key == "netplay.account";
}

inline bool Hidden(const NdsTitleMenuRow& row) {
    // Not shown: System (Resume and Quit) and, of Netplay, everything the
    // game does itself: hosting follows the Rapportal, joining is the game's
    // own host list, and the lobby is named after the host.
    // By key, not section: the pages move the Netplay rows into General.
    if (row.section == "System") return true;
    // The game has its own volume settings.
    if (row.key == "audio.volume") return true;
    // The Japanese release has its text in Japanese only (every language's
    // files hold it).
    if (row.key == "title.language" && dq9::TextLanguage() == 6u) return true;
    // The Netplay choice is always shown (its description says whether the
    // lobby server is up); the account row only while a sign-in is wanted or
    // held, which `enabled` tells.
    if (row.key.rfind("netplay.", 0) == 0)
        return IsNetplayToggle(row) ? false
                                    : !IsNetplayAccount(row) || !row.enabled;
    return !row.enabled ||
           (row.type != NdsTitleMenuRowType::Bool &&
            row.type != NdsTitleMenuRowType::Int &&
            row.type != NdsTitleMenuRowType::Choice &&
            row.type != NdsTitleMenuRowType::Key);
}

// A row whose value left and right step (on the others they turn the part).
inline bool Steps(const Row& row) {
    return row.kind == Row::Kind::Item &&
           (row.item.type == NdsTitleMenuRowType::Bool ||
            row.item.type == NdsTitleMenuRowType::Int ||
            row.item.type == NdsTitleMenuRowType::Choice);
}

// Shown but not changeable: the battle multipliers while visiting another
// player's world, where the host's values apply (and are shown).
inline bool Locked(const NdsTitleMenuRow& row) {
    return dq9::IsBattleMultiplier(row.key) && dq9::VisitingAnotherWorld();
}

// The pages' layout: which category each row goes in, and the order of the
// categories (as they first appear here) and of the rows in each. The
// runtime menu groups its rows by where they come from (Display, Audio,
// Input, then every title setting in Game); the pages group them by what a
// player looks for, the most used first, and no page is left with only a row
// or two:
//   General   set once: language first, sound, Netplay
//   Display   what the picture looks like, the frame rate first
//   Controls  a page for the pointer (mouse and touch), one for the
//             keyboard (the keys, in two parts: buttons, then the D-pad
//             and fast-forward),
//             and the gamepad layout
//   Quality of life  intros, text speed, quest limit, Zoom, camera rotation
//   Multipliers  battle damage, EXP, gold and drops
// Only the category changes: the keys, and so settings.toml, stay as they
// are. A row not named here (System, or one added later) comes after these
// in the runtime menu's order, in the page PageSection picks; Hidden() runs
// on the arranged rows. `part` starts a new part of the page at that row.
struct Placement {
    const char* key;
    const char* section;
    bool part = false;
};

inline constexpr Placement kLayout[] = {
    {"title.language", "General"},
    {"audio.mute", "General"},
    {"netplay.enabled", "General"},
    {"netplay.account", "General"},  // only while a sign-in is wanted
    {"discord.activity", "General"},  // only with Discord support
    {"display.fullscreen", "Display"},  // hidden on Android: always on
    {"title.fps_mode", "Display"},
    {"title.ui_size", "Display"},
    {"title.camera_mode", "Display"},
    {"display.resolution_scale", "Display"},
    {"display.scaling", "Display"},
    {"display.second_screen_size", "Display"},
    {"title.second_screen_over", "Display"},
    {"title.battle_stats_pos", "Display"},
    {"display.show_fps", "Display"},
    {"display.smooth_3d", "Display"},  // never shown: fps_mode owns it
    {"title.mouse_friendly", "Controls/Pointer"},
    {"title.camera_buttons", "Controls/Pointer"},
    // Only with a keyboard to bind, and not with --keyboard-bindings.
    {"input.key_a", "Controls/Keyboard"},
    {"input.key_b", "Controls/Keyboard"},
    {"input.key_x", "Controls/Keyboard"},
    {"input.key_y", "Controls/Keyboard"},
    {"input.key_l", "Controls/Keyboard"},
    {"input.key_r", "Controls/Keyboard"},
    {"input.key_start", "Controls/Keyboard"},
    {"input.key_select", "Controls/Keyboard"},
    {"input.key_up", "Controls/Keyboard", true},
    {"input.key_down", "Controls/Keyboard"},
    {"input.key_left", "Controls/Keyboard"},
    {"input.key_right", "Controls/Keyboard"},
    {"input.key_fast_forward", "Controls/Keyboard"},  // also without the others
    {"input.gamepad_layout", "Controls"},  // hidden with custom bindings
    {"title.skip_intros", "Quality of life"},
    {"title.skip_text", "Quality of life"},
    {"title.unlimited_quests", "Quality of life"},
    {"title.zoom_indoors", "Quality of life"},
    {"title.unlock_camera_rotation", "Quality of life"},
    {"title.mult_damage_dealt", "Multipliers"},
    {"title.mult_damage_taken", "Multipliers"},
    {"title.mult_exp", "Multipliers"},
    {"title.mult_gold", "Multipliers"},
    {"title.mult_drops", "Multipliers"},
};

// The page for a row kLayout does not name.
inline std::string PageSection(const std::string& section) {
    if (section == "Input") return "Controls";
    if (section == "Game" || section == "Audio" || section == "Netplay")
        return "General";
    return section;
}

// Whether kLayout starts a new part of its page at `key`.
inline bool StartsPart(const std::string& key) {
    for (const Placement& place : kLayout)
        if (key == place.key) return place.part;
    return false;
}

// `rows` in the pages' order, with the pages' sections.
inline std::vector<NdsTitleMenuRow> Arranged(std::vector<NdsTitleMenuRow> rows) {
    std::vector<NdsTitleMenuRow> out;
    out.reserve(rows.size());
    std::vector<bool> placed(rows.size(), false);
    for (const Placement& place : kLayout) {
        for (size_t i = 0; i < rows.size(); ++i) {
            if (placed[i] || rows[i].key != place.key) continue;
            placed[i] = true;
            rows[i].section = place.section;
            out.push_back(std::move(rows[i]));
            break;
        }
    }
    for (size_t i = 0; i < rows.size(); ++i) {
        if (placed[i]) continue;
        rows[i].section = PageSection(rows[i].section);
        out.push_back(std::move(rows[i]));
    }
    return out;
}

inline int ValueOf(const Model& model, const NdsTitleMenuRow& row) {
    if (Locked(row)) return dq9::HostMultiplier(row.key);
    const auto it = model.pending.find(row.key);
    return it != model.pending.end() ? it->second : row.value;
}

inline int Step(const NdsTitleMenuRow& row) {
    int step = row.step > 0 ? row.step : 1;
    if ((row.maximum - row.minimum) / step > 40) step *= 10;
    return step;
}

inline std::string ValueText(const Model& model, const NdsTitleMenuRow& row) {
    const int value = ValueOf(model, row);
    switch (row.type) {
    case NdsTitleMenuRowType::Key:
        if (value < 0) return Text("v_press_a_key", "Press a key");
        return row.value_text.empty() ? Text("v_none", "None") : row.value_text;
    case NdsTitleMenuRowType::Bool:
        if (IsNetplayToggle(row))
            return value ? Text("v_online", "Online") : Text("v_local", "Local");
        return value ? Text("v_on", "On") : Text("v_off", "Off");
    case NdsTitleMenuRowType::Choice:
        for (size_t i = 0; i < row.choices.size(); ++i) {
            if (row.choice_values[i] == value)
                return Text("v_" + Slug(row.choices[i]), row.choices[i]);
        }
        return std::to_string(value);
    default: {
        const bool percent = row.key == "display.second_screen_size";
        return std::to_string(value) + (percent ? "%" : "");
    }
    }
}

// The account row's label, which the runner words in English and changes with
// the sign-in (netplay_menu.cpp refresh_label): each wording has its key.
inline std::string AccountText(const std::string& label) {
    if (label == "Sign in with Discord")
        return Text("account_sign_in", label);
    if (label == "Signing in: see the browser")
        return Text("account_signing_in", label);
    if (label == "Press again to sign out")
        return Text("account_confirm", label);
    const std::string head = "Sign out (";
    if (label.size() > head.size() && label.compare(0, head.size(), head) == 0 &&
        label.back() == ')') {
        std::string text = Text("account_sign_out", "Sign out ({})");
        const size_t at = text.find("{}");
        if (at != std::string::npos)
            text.replace(at, 2, label.substr(head.size(),
                                             label.size() - head.size() - 1));
        return text;
    }
    return label;
}

inline std::string RowText(const Model& model, const Row& row) {
    switch (row.kind) {
    case Row::Kind::Category:
        return Text("cat_" + Slug(row.section), PageName(row.section));
    case Row::Kind::Item:
        if (IsNetplayAccount(row.item)) return AccountText(row.item.label);
        if (row.item.type == NdsTitleMenuRowType::Action)
            return Text(Slug(row.item.key), row.item.label);
        return Text(Slug(row.item.key), row.item.label) + ": " +
               ValueText(model, row.item);
    default:
        return Text("back", "Back");
    }
}

// The row's description, on one line ("" for Back).
inline std::string DescriptionText(const Row& row) {
    switch (row.kind) {
    case Row::Kind::Category:
        return Text("d_cat_" + Slug(row.section), "");
    case Row::Kind::Item:
        if (Locked(row.item)) return Text("d_locked_by_host", "Set by the host.");
        if (IsNetplayToggle(row.item))  // its description is the server's state
            return Text("d_netplay_enabled", "") + " " +
                   Text("server_status", "Server status:") + " " +
                   Text("server_" + row.item.description, row.item.description);
        // The keyboard's rows share theirs, but for one with its own.
        if (row.item.type == NdsTitleMenuRowType::Key) {
            const std::string own = Text("d_" + Slug(row.item.key), "");
            return own.empty() ? Text("d_input_key", row.item.description) : own;
        }
        return Text("d_" + Slug(row.item.key), row.item.description);
    default:
        return "";
    }
}

// `text` in at most `max_lines` lines of `line_chars` characters (a <tag>
// counts as one, a two-byte Shift JIS character of the Japanese text as two:
// it is twice as wide, and a line may break after it).
inline std::vector<std::string> Wrap(const std::string& text, size_t line_chars,
                                     size_t max_lines) {
    std::vector<std::string> lines;
    std::string line, word;
    size_t width = 0, word_width = 0;
    bool space = false;  // a space comes before `word`
    auto flush = [&]() {
        if (word.empty()) return;
        const size_t gap = !line.empty() && space ? 1 : 0;
        if (!line.empty() && width + gap + word_width > line_chars) {
            lines.push_back(line);
            line.clear();
            width = 0;
        }
        if (!line.empty() && space) {
            line += ' ';
            ++width;
        }
        line += word;
        width += word_width;
        word.clear();
        word_width = 0;
        space = false;
    };
    for (size_t i = 0; i < text.size() && lines.size() < max_lines; ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == ' ') {
            flush();
            space = true;
        } else if (((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) &&
                   i + 1 < text.size()) {
            flush();
            word = text.substr(i, 2);
            word_width = 2;
            ++i;
            flush();
        } else if (c == '<') {
            const size_t close = text.find('>', i);
            const size_t end = close == std::string::npos ? i : close;
            word += text.substr(i, end - i + 1);
            ++word_width;
            i = end;
        } else {
            word += static_cast<char>(c);
            ++word_width;
        }
    }
    flush();
    if (!line.empty() && lines.size() < max_lines) lines.push_back(line);
    return lines;
}

// The rows of the model's page, of the part shown: at most `max_rows`, with
// a Back row at the end when `back` is set. A page with more is shown in
// parts, as is one with a part break in kLayout; model.parts and
// model.tallest tell how many there are and how tall the tallest is.
inline void BuildRows(Model& model, size_t max_rows, bool back = true) {
    model.rows.clear();
    const std::vector<NdsTitleMenuRow> all = Arranged(nds_title_menu_rows());
    // A value the frontend now reports needs no override any more.
    for (const NdsTitleMenuRow& row : all) {
        const auto it = model.pending.find(row.key);
        if (it != model.pending.end() && it->second == row.value)
            model.pending.erase(it);
    }
    const std::string& page = model.section;
    const std::string prefix = page.empty() ? std::string() : page + "/";
    const size_t per_part = max_rows > (back ? 1u : 0u)
        ? max_rows - (back ? 1u : 0u) : 1u;
    std::vector<std::vector<Row>> parts(1);
    for (const NdsTitleMenuRow& row : all) {
        if (Hidden(row)) continue;
        Row r;
        if (row.section == page) {
            r.kind = Row::Kind::Item;
            r.item = row;
            if (StartsPart(row.key) && !parts.back().empty())
                parts.emplace_back();
        } else if (row.section.compare(0, prefix.size(), prefix) == 0) {
            // A row of a page under this one: that page's row, once.
            const size_t slash = row.section.find('/', prefix.size());
            r.kind = Row::Kind::Category;
            r.section = row.section.substr(0, slash);
            bool seen = false;
            for (const std::vector<Row>& part : parts)
                for (const Row& other : part)
                    seen |= other.kind == Row::Kind::Category &&
                            other.section == r.section;
            if (seen) continue;
        } else {
            continue;
        }
        if (parts.back().size() >= per_part) parts.emplace_back();
        parts.back().push_back(std::move(r));
    }
    model.parts = static_cast<uint32_t>(parts.size());
    model.tallest = 0;
    for (const std::vector<Row>& part : parts) {
        const uint32_t rows = static_cast<uint32_t>(part.size() + (back ? 1 : 0));
        if (rows > model.tallest) model.tallest = rows;
    }
    uint32_t& shown = model.part[page];
    if (shown >= model.parts) shown = model.parts - 1;
    model.rows = std::move(parts[shown]);
    if (back) model.rows.emplace_back();
}

// Shows the next (+1) or previous (-1) part of the page, wrapping. True when
// the page has parts.
inline bool TurnPart(Model& model, int direction) {
    if (model.parts <= 1) return false;
    uint32_t& shown = model.part[model.section];
    shown = static_cast<uint32_t>(
        (static_cast<int>(shown) + direction + static_cast<int>(model.parts)) %
        static_cast<int>(model.parts));
    return true;
}

// What the page shows (every row's text and description), to tell when the
// frontend's rows changed under it (the Netplay account, the server status).
inline std::string Signature(const Model& model) {
    std::string out;
    for (const Row& row : model.rows) {
        out += RowText(model, row);
        out += '\x01';
        out += DescriptionText(row);
        out += '\x02';
    }
    return out;
}

inline bool SetValue(Model& model, const NdsTitleMenuRow& row, int value) {
    if (value == ValueOf(model, row)) return false;
    model.pending[row.key] = value;
    nds_title_menu_set(row.key, value);
    return true;
}

// Steps the row's value. direction: -1 left, +1 right, 0 chosen (steps
// forward and wraps). True when the value changed.
inline bool Change(Model& model, const NdsTitleMenuRow& row, int direction) {
    if (Locked(row)) return false;
    const int value = ValueOf(model, row);
    switch (row.type) {
    case NdsTitleMenuRowType::Bool:
        return SetValue(model, row, direction == 0 ? !value : direction > 0);
    case NdsTitleMenuRowType::Choice: {
        const int count = static_cast<int>(row.choices.size());
        if (!count) return false;
        int index = 0;
        for (int i = 0; i < count; ++i)
            if (row.choice_values[static_cast<size_t>(i)] == value) index = i;
        if (direction == 0)
            index = (index + 1) % count;
        else
            index += direction;
        if (index < 0 || index >= count) return false;
        return SetValue(model, row, row.choice_values[static_cast<size_t>(index)]);
    }
    case NdsTitleMenuRowType::Int: {
        int next = value + Step(row) * (direction == 0 ? 1 : direction);
        if (direction == 0 && value >= row.maximum) next = row.minimum;
        if (next > row.maximum) next = row.maximum;
        if (next < row.minimum) next = row.minimum;
        return SetValue(model, row, next);
    }
    case NdsTitleMenuRowType::Action:
        // Chosen only: left and right do not sign in or out.
        if (direction != 0 || !IsNetplayAccount(row)) return false;
        nds_title_menu_run(row.key);
        return true;
    case NdsTitleMenuRowType::Key:
        // Chosen: the frontend waits for the key (the row says so a frame
        // later), and chosen again stops waiting.
        if (direction != 0) return false;
        nds_title_menu_run(row.key);
        return true;
    default:
        return false;
    }
}

inline void WriteString(uint32_t address, uint32_t size, const std::string& text) {
    const uint32_t len = static_cast<uint32_t>(
        text.size() < size ? text.size() : size - 1u);
    for (uint32_t i = 0; i < len; ++i)
        bus_write_u8(address + i, static_cast<uint8_t>(text[i]));
    bus_write_u8(address + len, 0u);
}

}  // namespace dq9::options
