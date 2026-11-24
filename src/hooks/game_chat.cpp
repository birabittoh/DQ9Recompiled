// Netplay chat typed on the game's own keyboard
// (ndsrecomp runner/src/net/netplay_chat.h, nds_netplay_chat_set_game_keyboard).
// Holding the chat's Start (or its button) in the field opens the Greeting
// editor of Profile Settings with an empty text; its OK sends the text as a
// chat line and goes straight back to the field, B on the empty text just
// goes back. The profile's greeting is never touched.
//
// The hooks are in config/hooks/dq9_arm9_ov017.toml (the field's menu check
// and the Profile Settings task) and dq9_arm9_ov012.toml (the Profile
// Settings scene); the addresses are described there.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>

#include "chat_overlay.h"
#include "gpu2d.h"
#include "net/netplay_chat.h"
#include "title_hooks.h"

#include "dq9_text.h"
#include "lang.h"
#include "options_model.h"

namespace {

// ---- the field: starting the Profile Settings task -------------------------

constexpr uint32_t kProfileTask = 0x3B54u;   // mgr: the Profile Settings task
constexpr uint8_t kProfileTaskType = 0x2F;
constexpr uint32_t kTaskState = 28u;         // u8
constexpr uint32_t kTaskFromMenu = 29u;      // u8
constexpr uint32_t kTaskScene = 32u;         // u32
constexpr uint8_t kTaskLastState = 3;
constexpr uint32_t kHeroBusy = 0xACu;        // s16, 0 when free
constexpr uint32_t kHeroAction = 0xC4u;      // u16, low 15 bits: 0 when free
constexpr uint16_t kHeroActionMask = 0x7FFFu;

// ---- the scene -------------------------------------------------------------

constexpr uint32_t kScenePage = 0x1371u;     // u8
constexpr uint32_t kSceneChoice = 0x13D4u;   // u32, the menu entry chosen
constexpr uint32_t kSceneText = 0x13D0u;     // u32, the text being edited
// u32, the text the greeting box (window 13, 0x021E8270) shows; 0 = the
// profile's greeting. The Greeting page points it at kSceneText.
constexpr uint32_t kSceneShownText = 0x1328u;

// Free guest memory: the end of overlay slot 0, past overlay 3 (it ends at
// 0x02180D40) and past what camp_options.cpp uses (up to 0x02183FA0).
constexpr uint32_t kPromptText = 0x02183FA0u;
constexpr uint32_t kPromptTextSize = 0x60u;
constexpr uint32_t kEmptyText = kPromptText + kPromptTextSize;
constexpr uint8_t kGreetingPage = 14;
constexpr uint32_t kGreetingEntry = 4;       // the menu's "Greeting"
constexpr uint32_t kTextBytes = 0x200u;

using Clock = std::chrono::steady_clock;
constexpr auto kFieldFresh = std::chrono::milliseconds(250);
constexpr auto kRequestLife = std::chrono::milliseconds(500);

int64_t Now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now().time_since_epoch())
        .count();
}

// Written by the CPU thread (the hooks), read by the frontend.
std::atomic<int64_t> g_field_seen_ms{INT64_MIN / 2};
// Written by the frontend, taken by the CPU thread.
std::atomic<int64_t> g_request_ms{INT64_MIN / 2};

// Written by the CPU thread, read by the frontend too.
enum class Step : uint8_t { Off, Starting, Menu, Editing };
std::atomic<Step> g_step{Step::Off};
// Written by the frontend, taken by the CPU thread: send what was typed.
std::atomic<bool> g_send{false};
// CPU thread only: close the scene, once its step gets there.
bool g_close = false;

// The frontend's chat Start: while typing, send (as OK does); otherwise
// open the keyboard when the field is up and free.
bool GameKeyboardStart() {
    if (g_step.load(std::memory_order_acquire) == Step::Editing) {
        g_send.store(true, std::memory_order_release);
        return true;
    }
    const int64_t now = Now();
    if (now - g_field_seen_ms.load(std::memory_order_acquire) >
        kFieldFresh.count())
        return false;
    g_request_ms.store(now, std::memory_order_release);
    return true;
}

const bool kRegistered =
    (nds_netplay_chat_set_game_keyboard(&GameKeyboardStart), true);

bool Chatting() { return g_step != Step::Off; }

// The top screen keeps the field's picture (the map) while the scene, which
// draws its own page there, is up: held from the task being queued, kept
// while the text is typed, let go once the field is back.
constexpr int kTopScreen = 0;
constexpr uint32_t kHoldFrames = 300;     // a safety net if never let go
constexpr uint32_t kReleaseFrames = 10;   // the field redrawing its own
bool g_holding = false;                   // CPU thread only

void HoldTop() {
    nds_gpu2d_hold_screen(kTopScreen, kHoldFrames);
    g_holding = true;
}

std::string ReadText(uint32_t address) {
    uint8_t bytes[kTextBytes];
    for (uint32_t i = 0; i < kTextBytes; ++i)
        bytes[i] = bus_read_u8(address + i);
    return dq9::DecodeText(bytes, kTextBytes);
}

// The line being typed shows in the runner's chat panel, with the recent
// lines above it.
void ShowInput(const std::string& text) {
    NdsChatOverlayInput input;
    input.open = true;
    input.text = text;
    input.cursor = text.size();
    nds_chat_overlay_set_input(input);
}

void HideInput() { nds_chat_overlay_set_input(NdsChatOverlayInput{}); }

void Stop() {
    if (g_step == Step::Editing) HideInput();
    g_step = Step::Off;
    g_close = false;
    g_send.store(false, std::memory_order_release);
}

}  // namespace

// Before `cmp r8, #0` at 0x021A53A0: the field's menu check, reached only
// while no task runs and the hero can open a menu; r8 = X was pressed, r5 the
// field manager, r7 the hero. A pending request takes X's path, with the
// same two checks X's own test (0x021A5258..0x021A5294) makes.
NDS_TITLE_HOOK void Dq9ChatFieldMenu(uint32_t* mgr, uint32_t* hero,
                                     uint32_t* x_pressed) {
    (void)mgr;
    const int64_t now = Now();
    g_field_seen_ms.store(now, std::memory_order_release);
    if (g_holding && !Chatting()) {
        nds_gpu2d_hold_screen(kTopScreen, kReleaseFrames);
        g_holding = false;
    }
    if (*x_pressed != 0u || Chatting()) return;
    if (now - g_request_ms.load(std::memory_order_acquire) >
        kRequestLife.count())
        return;
    if (static_cast<int16_t>(bus_read_u16(*hero + kHeroBusy)) != 0) return;
    if ((bus_read_u16(*hero + kHeroAction) & kHeroActionMask) != 0) return;
    g_request_ms.store(INT64_MIN / 2, std::memory_order_release);
    *x_pressed = 1u;
    g_step = Step::Starting;
}

// After `ldr r1, [r1, #0x71C]` at 0x021A53FC: r1 is the camp menu's task,
// about to be queued. On a chat request the Profile Settings task goes in
// instead, set up as 0x021C17CC does, with "from the menu" set: then the
// task loads overlay 3 (the keyboard), and the scene's close-all goes back
// to the field (0x021C18F0).
NDS_TITLE_HOOK void Dq9ChatQueueTask(uint32_t* task, uint32_t* mgr) {
    if (g_step != Step::Starting) return;
    const uint32_t t = bus_read_u32(*mgr + kProfileTask);
    bus_write_u8(t + 0u, kProfileTaskType);
    bus_write_u8(t + 1u, 0u);
    bus_write_u8(t + 2u, 0u);
    bus_write_u8(t + 3u, 0u);
    bus_write_u32(t + 8u, 0u);
    bus_write_u8(t + kTaskState, 0u);
    bus_write_u32(t + kTaskScene, 0u);
    bus_write_u8(t + kTaskFromMenu, 1u);
    *task = t;
    g_step = Step::Menu;
    HoldTop();
    std::fprintf(stderr, "[dq9-chat] opening the greeting keyboard\n");
}

// At 0x021C17FC, the Profile Settings task's step: its last state ends the
// task, and the chat with it.
NDS_TITLE_HOOK void Dq9ChatProfileTask(uint32_t* task) {
    if (Chatting() && bus_read_u8(*task + kTaskState) == kTaskLastState)
        Stop();
}

// Before `cmp r0, #0` at 0x02186B0C: the Profile Settings menu, r0 = an
// entry was chosen, r5 the scene. The chat chooses Greeting at once.
NDS_TITLE_HOOK void Dq9ChatProfileMenu(uint32_t* chosen, uint32_t* scene) {
    if (g_step != Step::Menu) return;
    bus_write_u32(*scene + kSceneChoice, kGreetingEntry);
    *chosen = 1u;
    g_step = Step::Editing;
}

// At 0x02188D34, after the Greeting page copied the profile's greeting into
// the text being edited (r5 the scene): the chat starts empty, and the
// greeting box stays empty too, the line being typed shows in the chat
// panel instead.
NDS_TITLE_HOOK void Dq9ChatGreetingText(uint32_t* scene) {
    if (g_step != Step::Editing) return;
    const uint32_t text = bus_read_u32(*scene + kSceneText);
    if (text) bus_write_u8(text, 0u);
    bus_write_u8(kEmptyText, 0u);
    bus_write_u32(*scene + kSceneShownText, kEmptyText);
    ShowInput("");
}

// Before `mov r1, #1` at 0x0218AB84, after 0x0218AB34 (the keyboard's
// window) fetched its title, "Write your greeting" (r0): the chat's own.
NDS_TITLE_HOOK void Dq9ChatGreetingPrompt(uint32_t* title) {
    if (g_step != Step::Editing) return;
    dq9::options::WriteString(kPromptText, kPromptTextSize,
                dq9::Tr("chat", "prompt", dq9::TextLanguage()));
    *title = kPromptText;
}

// Before `cmp r0, #0xB` at 0x02188FBC, every frame of the Greeting page's
// typing state (r5 the scene): r0 = what the keyboard step did, 10 for OK;
// the frontend's Start counts as OK too. The chat closes the scene instead
// (Dq9ChatForceClose: that checks the text the same way OK does, then
// Dq9ChatClose sends it), and OK's own path to the profile becomes
// "nothing happened".
NDS_TITLE_HOOK void Dq9ChatGreetingStep(uint32_t* result, uint32_t* scene) {
    constexpr uint32_t kOk = 10u;
    if (g_step != Step::Editing) return;
    HoldTop();
    const uint32_t text = bus_read_u32(*scene + kSceneText);
    if (text) ShowInput(ReadText(text));
    if (g_send.exchange(false, std::memory_order_acq_rel)) *result = kOk;
    if (*result != kOk) return;
    g_close = true;
    *result = 0u;
}

// Before `cmp r0, #0` at 0x02188F84: B (or the back button) was pressed and
// r0 is the text's first byte; zero would save the empty text as the
// greeting. The chat closes instead, and the page sees a text.
NDS_TITLE_HOOK void Dq9ChatGreetingBack(uint32_t* first_byte) {
    if (g_step != Step::Editing || *first_byte != 0u) return;
    g_close = true;
    *first_byte = 1u;
}

// Before `bl 0x020426CC` at 0x02189140, the Greeting page writing the text
// to the profile (r1 the profile's copy). Never in a chat: with no
// destination it writes nothing. The two hooks above keep the page from
// getting there.
NDS_TITLE_HOOK void Dq9ChatGreetingSave(uint32_t* profile) {
    if (Chatting()) *profile = 0u;
}

// Before `cmp r0, #0` at 0x02184C4C: r0 = 0x021959B4, true when the
// multiplayer session pulls this player out of the menus. The scene then
// checks the Greeting's text as OK does and closes back to the field
// (0x02184CD4).
NDS_TITLE_HOOK void Dq9ChatForceClose(uint32_t* pulled_out) {
    // Every frame of the scene's pages: the top screen stays held.
    if (Chatting()) HoldTop();
    if (!g_close) return;
    g_close = false;
    *pulled_out = 1u;
}

// At 0x02184CD4, the close-all going ahead (r6 the scene; the text, if
// any, passed the page's check). The text is the chat line.
NDS_TITLE_HOOK void Dq9ChatClose(uint32_t* scene) {
    if (g_step != Step::Editing) return;
    Stop();
    if (bus_read_u8(*scene + kScenePage) != kGreetingPage) return;
    const uint32_t text = bus_read_u32(*scene + kSceneText);
    if (!text) return;
    const std::string line = ReadText(text);
    if (line.empty()) return;
    const NdsChatSendResult sent = nds_netplay_chat_send(line);
    std::fprintf(stderr, "[dq9-chat] greeting keyboard: %zu bytes, %s\n",
                 line.size(),
                 sent == NdsChatSendResult::Sent    ? "sent"
                 : sent == NdsChatSendResult::NoOne ? "nobody to send to"
                                                    : "empty");
}
