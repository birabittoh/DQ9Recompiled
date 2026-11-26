// Last used first (Quality of life > Last used first, [title]
// recent_commands in settings.toml, off by default): in battle, the spells,
// abilities and items a character chose last come first in their lists, the
// most recent at the top, the others after them in the game's order. Each
// character has their own. What they chose is kept in a file of its own
// next to the game's save file (<save>.recent), written when the game saves
// (at a church, or a quick save) and read when it loads a save, so turning
// the power off without saving forgets what was chosen since.
//
// The guest code is ov000's command menu (config/hooks/dq9_arm9_ov000.toml)
// and the save and load (config/hooks/dq9_arm9.toml). A party member's
// command panel (the records ov000 0x02171640 / 0x02171968 fill, from ov026
// 0x021DC8FC at the start of each battle) keeps its lists as pointers into
// the action table:
// - +156: the spells, up to 66, s16 +0x82 of them;
// - +420: the abilities, up to 147, s16 +0x84 of them;
// - an action's id is its +4 & 0xFFF, which the game takes from the entry
//   chosen (so the lists' order is the menu's only), into u16 +0x26;
// - s8 +0x20 / s16 +0x1E: the cursor in the spells / abilities;
// - +1008: the items, 16 cells of a grid 4 wide, which 0x02171C04 fills
//   each time the list opens: first what the game lists before them (the
//   entries whose +8 & 0xF is above 7, from ov026), then from the next row
//   the character's carried items (32-byte slots at character +404, s16 +24
//   the item's id, which the game takes into u16 +0x2C), with gaps left
//   empty; only the carried items change places, among their own cells;
// - s8 +0x22: the cursor in the items;
// - s32 +0x4C: the member's actor id; s8 +0x18 the top of the state stack
//   at s8 +0x10 (26: not enough MP).
// The actor's +336 is the character, whose name (+60, a zero-ended string)
// tells them apart: the save keeps the characters in roster order, which
// changes as they come and go. The game itself remembers the last spell
// and ability chosen (+2400 / +2402, not saved) and opens the list on it,
// which here is the top; the items open on the first cell.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <system_error>
#include <vector>

#include "io.h"
#include "save_image.h"
#include "title_hooks.h"
#include "title_settings.h"

namespace {

constexpr const char* kChoices[] = {"Off", "On"};

constexpr NdsTitleSetting kRecentCommands = {
    "recent_commands",
    "Last used first",
    "In battle, each character's last used spells, abilities and items come "
    "first in their lists. Saving at a church or quick saving keeps them.",
    0, 1, 1,
    kChoices, sizeof(kChoices) / sizeof(kChoices[0]), nullptr,
    0,
};

const int kRecentCommandsId = nds_title_setting_register(&kRecentCommands);

bool Enabled() { return nds_title_setting_value(kRecentCommandsId) != 0; }

constexpr uint32_t kActorTable = 0x020F33D8u;  // + 8 + 4 * id: actor
constexpr int kActorCount = 233;
constexpr uint16_t kActorMember = 0x800u;       // has a character (+336)
constexpr uint32_t kActorCharacter = 336u;
constexpr uint32_t kCharacterName = 60u;
constexpr uint32_t kNameMax = 32u;

constexpr uint32_t kPanelMember = 0x4Cu;
constexpr uint32_t kPanelStates = 0x10u;
constexpr uint32_t kPanelTop = 0x18u;
constexpr uint32_t kPanelChosen = 0x26u;     // a spell's or ability's id
constexpr uint32_t kPanelChosenItem = 0x2Cu;
constexpr int8_t kStateNoMp = 26;
constexpr uint32_t kActionId = 0xFFFu;

enum Kind { kSpells, kAbilities, kItems, kKinds };

constexpr uint32_t kSpellList = 156u, kSpellCount = 0x82u, kSpellMax = 66u;
constexpr uint32_t kAbilityList = 420u, kAbilityCount = 0x84u, kAbilityMax = 147u;
constexpr uint32_t kItemList = 1008u, kItemCells = 16u;
constexpr uint32_t kItemId = 24u;
constexpr uint32_t kItemKind = 8u;  // & 0xF; above 7: not a carried item

// How many of a character's last choices of a kind come first.
constexpr int kDepth = 8;

struct Character {
    uint32_t key = 0;  // CRC32 of the name
    uint16_t recent[kKinds][kDepth] = {};  // ids (EntryId), the latest first; 0 none
};

// The characters, the one who chose last first.
std::vector<Character> g_characters;

// Each panel's list as the game made it (by panel and kind), to put the
// others back in the game's order after a reorder.
std::map<uint64_t, std::vector<uint32_t>> g_original;

// The file: magic, u32 count, the characters (u32 key, then
// kKinds * kDepth u16), u32 CRC32 of all before it.
constexpr char kMagic[8] = {'D', 'Q', '9', 'R', 'R', 'C', 0, 1};
constexpr uint32_t kHeader = 12u;
constexpr uint32_t kRecordSize = 4u + kKinds * kDepth * 2u;
// The save holds 13 characters (roster and party); room for some more who
// left it, the ones who chose last kept.
constexpr uint32_t kMaxCharacters = 32u;

// The file next to the battery file; empty while saves are not kept.
std::filesystem::path FilePath() {
    const char* save = nds_io_cartridge_save_path();
    if (save == nullptr || *save == 0) return {};
    const std::string name = std::string(save) + ".recent";  // UTF-8
    return std::filesystem::path(reinterpret_cast<const char8_t*>(name.data()),
                                 reinterpret_cast<const char8_t*>(name.data() +
                                                                  name.size()));
}

int16_t ReadS16(uint32_t addr) { return static_cast<int16_t>(bus_read_u16(addr)); }
int8_t ReadS8(uint32_t addr) { return static_cast<int8_t>(bus_read_u8(addr)); }

// The character of the panel's member: the CRC32 of their name, or 0.
uint32_t CharacterKey(uint32_t panel) {
    const int32_t id = static_cast<int32_t>(bus_read_u32(panel + kPanelMember));
    if (id < 0 || id >= kActorCount) return 0;
    const uint32_t actor = bus_read_u32(kActorTable + 8u + 4u * id);
    if (actor == 0 || (bus_read_u16(actor) & kActorMember) == 0) return 0;
    const uint32_t character = bus_read_u32(actor + kActorCharacter);
    if (character == 0) return 0;
    uint8_t name[kNameMax];
    uint32_t len = 0;
    while (len < kNameMax) {
        const uint8_t c = bus_read_u8(character + kCharacterName + len);
        if (c == 0) break;
        name[len++] = c;
    }
    if (len == 0) return 0;
    const uint32_t key = dq9::save::Crc32(name, len);
    return key != 0 ? key : 1u;
}

Character* Find(uint32_t key) {
    for (Character& c : g_characters)
        if (c.key == key) return &c;
    return nullptr;
}

uint32_t ListBase(Kind kind) {
    return kind == kSpells ? kSpellList : kind == kAbilities ? kAbilityList : kItemList;
}

// The id the game takes from an entry of `kind`.
uint16_t EntryId(Kind kind, uint32_t entry) {
    if (kind == kItems) return bus_read_u16(entry + kItemId);
    return static_cast<uint16_t>(bus_read_u32(entry + 4u) & kActionId);
}

bool CarriedItem(uint32_t entry) {
    return entry != 0 && (bus_read_u32(entry + kItemKind) & 0xFu) <= 7u &&
           static_cast<int16_t>(bus_read_u16(entry + kItemId)) > 0;
}

// The list's places whose entries may change places: the spells' and
// abilities' first `count`, the items' cells of carried items.
std::vector<uint32_t> Places(uint32_t panel, Kind kind) {
    std::vector<uint32_t> places;
    if (kind == kItems) {
        for (uint32_t i = 0; i < kItemCells; ++i)
            if (CarriedItem(bus_read_u32(panel + kItemList + 4u * i)))
                places.push_back(i);
        return places;
    }
    const int16_t count =
        ReadS16(panel + (kind == kSpells ? kSpellCount : kAbilityCount));
    const uint32_t max = kind == kSpells ? kSpellMax : kAbilityMax;
    if (count <= 0 || static_cast<uint32_t>(count) > max) return places;
    for (int16_t i = 0; i < count; ++i) places.push_back(static_cast<uint32_t>(i));
    return places;
}

// Puts the panel's list of `kind` in the order the character's choices
// give, before the menu opens it.
void Reorder(uint32_t panel, Kind kind) {
    if (!Enabled()) return;
    const uint32_t base = panel + ListBase(kind);
    const std::vector<uint32_t> places = Places(panel, kind);
    if (places.size() <= 1) return;
    std::vector<uint32_t> shown(places.size());
    for (size_t i = 0; i < places.size(); ++i)
        shown[i] = bus_read_u32(base + 4u * places[i]);

    // The game's order: the one kept for this panel while it has the same
    // entries (an earlier reorder), else the list as it is (a new battle;
    // the items, made anew each time).
    std::vector<uint32_t>& original =
        g_original[(static_cast<uint64_t>(panel) << 2) | kind];
    std::vector<uint32_t> a = shown, b = original;
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    if (a != b) original = shown;

    const uint32_t key = CharacterKey(panel);
    const Character* character = key ? Find(key) : nullptr;
    std::vector<uint32_t> order;
    order.reserve(original.size());
    std::vector<bool> used(original.size(), false);
    if (character) {
        for (const uint16_t id : character->recent[kind]) {
            if (id == 0) continue;
            for (size_t i = 0; i < original.size(); ++i) {
                if (used[i] || original[i] == 0 ||
                    EntryId(kind, original[i]) != id)
                    continue;
                used[i] = true;
                order.push_back(original[i]);
                break;
            }
        }
    }
    for (size_t i = 0; i < original.size(); ++i)
        if (!used[i]) order.push_back(original[i]);
    for (size_t i = 0; i < order.size(); ++i)
        if (order[i] != shown[i]) bus_write_u32(base + 4u * places[i], order[i]);
}

// The entry at `cursor` of the panel's list of `kind` was chosen: it goes
// first in the character's choices, unless the game refused it.
void Chosen(uint32_t panel, Kind kind, int cursor) {
    if (!Enabled() || cursor < 0) return;
    const std::vector<uint32_t> places = Places(panel, kind);
    if (std::find(places.begin(), places.end(), static_cast<uint32_t>(cursor)) ==
        places.end())
        return;
    const int8_t top = ReadS8(panel + kPanelTop);
    if (top < 0 || ReadS8(panel + kPanelStates + top) == kStateNoMp) return;
    const uint32_t entry =
        bus_read_u32(panel + ListBase(kind) + 4u * static_cast<uint32_t>(cursor));
    if (entry == 0) return;
    const uint16_t id = EntryId(kind, entry);
    const uint32_t chosen = kind == kItems ? kPanelChosenItem : kPanelChosen;
    if (id == 0 || id != bus_read_u16(panel + chosen)) return;
    const uint32_t key = CharacterKey(panel);
    if (key == 0) return;

    Character c;
    if (Character* found = Find(key)) c = *found;
    c.key = key;
    g_characters.erase(std::remove_if(g_characters.begin(), g_characters.end(),
                                      [&](const Character& o) { return o.key == key; }),
                       g_characters.end());
    uint16_t* recent = c.recent[kind];
    int at = kDepth - 1;
    for (int i = 0; i < kDepth; ++i)
        if (recent[i] == id) at = i;
    for (int i = at; i > 0; --i) recent[i] = recent[i - 1];
    recent[0] = id;
    g_characters.insert(g_characters.begin(), c);
    if (g_characters.size() > kMaxCharacters) g_characters.resize(kMaxCharacters);
}

void PutU32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}

}  // namespace

// Before `ldrsb r0, [r6, #0x10]` at 0x0217B02C: Spells was chosen and the
// panel (r7) has some; the list opens next.
NDS_TITLE_HOOK void Dq9RecentSpellsOpen(uint32_t* panel) {
    Reorder(*panel, kSpells);
}

// Before `ldrsb r1, [r6, #0x10]` at 0x0217B15C: the same for Abilities.
NDS_TITLE_HOOK void Dq9RecentAbilitiesOpen(uint32_t* panel) {
    Reorder(*panel, kAbilities);
}

// At 0x0217B608, after the spell list took a choice (r8 = panel).
NDS_TITLE_HOOK void Dq9RecentSpellChosen(uint32_t* panel) {
    Chosen(*panel, kSpells, ReadS8(*panel + 0x20u));
}

// At 0x0217B7E0, after the ability list took a choice (r9 = panel).
NDS_TITLE_HOOK void Dq9RecentAbilityChosen(uint32_t* panel) {
    Chosen(*panel, kAbilities, ReadS16(*panel + 0x1Eu));
}

// Before `ldrsb r0, [r7, #0x86]` at 0x0217B228: Items was chosen and
// 0x02171C04 made the panel's (r7) list anew.
NDS_TITLE_HOOK void Dq9RecentItemsOpen(uint32_t* panel) {
    Reorder(*panel, kItems);
}

// At 0x0217BA68, after the item list took a choice (r10 = panel).
NDS_TITLE_HOOK void Dq9RecentItemChosen(uint32_t* panel) {
    Chosen(*panel, kItems, ReadS8(*panel + 0x22u));
}

// While the game builds its save image (0x020AA92C): the choices go in
// the file, through a temporary one so a failed write keeps the last.
// Nothing is written while there is neither a choice nor a file.
NDS_TITLE_HOOK void Dq9RecentSave() {
    const std::filesystem::path path = FilePath();
    if (path.empty()) return;
    std::error_code ec;
    if (g_characters.empty() && !std::filesystem::exists(path, ec)) return;
    std::vector<uint8_t> data(kMagic, kMagic + sizeof(kMagic));
    PutU32(data, static_cast<uint32_t>(g_characters.size()));
    for (const Character& c : g_characters) {
        PutU32(data, c.key);
        for (int k = 0; k < kKinds; ++k)
            for (const uint16_t id : c.recent[k]) {
                data.push_back(static_cast<uint8_t>(id));
                data.push_back(static_cast<uint8_t>(id >> 8));
            }
    }
    PutU32(data, dq9::save::Crc32(data.data(), data.size()));

    std::filesystem::path temp = path;
    temp += ".tmp";
    {
        std::ofstream f(temp, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
        if (!f) {
            std::fprintf(stderr, "[recent] cannot write %s\n", temp.string().c_str());
            return;
        }
    }
    std::filesystem::rename(temp, path, ec);
    if (ec)
        std::fprintf(stderr, "[recent] cannot replace %s: %s\n",
                     path.string().c_str(), ec.message().c_str());
}

// After a load copied the save into the game (0x020AB118, 0x020AB4D4): the
// choices the file kept, or none.
NDS_TITLE_HOOK void Dq9RecentLoad() {
    g_characters.clear();
    g_original.clear();
    const std::filesystem::path path = FilePath();
    if (path.empty()) return;
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    const std::vector<uint8_t> t((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
    if (t.size() < kHeader + 4u || std::memcmp(t.data(), kMagic, sizeof(kMagic)) != 0)
        return;
    const uint32_t n = dq9::save::ReadU32(t.data() + 8);
    if (n > kMaxCharacters) return;
    const uint32_t body = kHeader + n * kRecordSize;
    if (t.size() < body + 4u ||
        dq9::save::Crc32(t.data(), body) != dq9::save::ReadU32(t.data() + body)) {
        std::fprintf(stderr, "[recent] %s is damaged, ignored\n", path.string().c_str());
        return;
    }
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t* r = t.data() + kHeader + i * kRecordSize;
        Character c;
        c.key = dq9::save::ReadU32(r);
        for (int k = 0; k < kKinds; ++k)
            for (int d = 0; d < kDepth; ++d) {
                const uint8_t* p = r + 4 + (k * kDepth + d) * 2;
                c.recent[k][d] = static_cast<uint16_t>(p[0] | (p[1] << 8));
            }
        if (c.key != 0 && !Find(c.key)) g_characters.push_back(c);
    }
}
