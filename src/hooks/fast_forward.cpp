// Fast-forward settings (Controls > F.F. behavior and F.F. speed, [title]
// ff_behavior and ff_speed in settings.toml). The runner's fast-forward (Tab,
// the pad's right trigger, the on-screen button) runs as fast as the host can;
// these two settings shape it through the queries below (title_settings.h).
//
//   F.F. behavior  Hold (the default) runs while Tab or the trigger is held;
//                  Toggle switches it on and off at each press. The
//                  on-screen button always toggles.
//   F.F. speed     the most it speeds up in 3D scenes (the field, towns and
//                  battles), in times real time; Max (the default) is no
//                  cap. Menus, 2D screens and movies stay uncapped.
#include "io.h"
#include "title_hooks.h"
#include "title_settings.h"

#include "dq9_scene.h"

namespace {

constexpr const char* kBehaviorChoices[] = {"Hold", "Toggle"};

constexpr NdsTitleSetting kBehavior = {
    "ff_behavior",
    "F.F. behavior",
    "Hold or toggle fast-forward with Tab and the right trigger.",
    0, 1, 1,
    kBehaviorChoices, sizeof(kBehaviorChoices) / sizeof(kBehaviorChoices[0]),
    nullptr,
    0,
};

const int kBehaviorId = nds_title_setting_register(&kBehavior);

constexpr const char* kSpeedChoices[] = {"2x", "3x", "4x", "6x", "Max"};
constexpr int kSpeedValues[] = {2, 3, 4, 6, 0};
static_assert(sizeof(kSpeedChoices) / sizeof(kSpeedChoices[0]) ==
                  sizeof(kSpeedValues) / sizeof(kSpeedValues[0]),
              "choices");

constexpr NdsTitleSetting kSpeed = {
    "ff_speed",
    "F.F. speed",
    "How fast fast-forward runs in 3D scenes. Max has no limit.",
    0, 6, 1,
    kSpeedChoices, sizeof(kSpeedChoices) / sizeof(kSpeedChoices[0]),
    kSpeedValues,
    0,
};

const int kSpeedId = nds_title_setting_register(&kSpeed);

bool Toggle() { return nds_title_setting_value(kBehaviorId) == 1; }

// The cap, in a 3D scene only. Read on the frontend thread, where the guest
// memory the scene is told from is only read (title_settings.h).
int Limit() {
    const int speed = nds_title_setting_value(kSpeedId);
    if (speed <= 0) return 0;
    if (dq9::InBattle() || (dq9::GameModeRunning() && dq9::FieldLoaded()))
        return speed;
    return 0;
}

const bool kRegistered = (nds_title_set_turbo_toggle_query(Toggle),
                          nds_title_set_turbo_limit_query(Limit), true);

}  // namespace
