#pragma once
// Keyboard bindings: one key per PlayStation control, stored in the .ini as
// "keys = cross=X,circle=Z,..." (key names are SDL key names). Unlisted controls use the defaults.
#include <cstdint>
#include <string>

struct KeyBind {
    const char* id;      // name in the .ini
    const char* label;   // shown in the menu
    const char* def;     // default key ("" = none)
    uint32_t bit;        // hostinput.h IN_* bit
    const char* pad_def; // default controller input (SDL game controller name, "" = none)
};
int keybind_count();
const KeyBind& keybind(int i);
std::string keybind_key(int i);                 // current key name
void keybind_set(int i, const std::string& key);  // saves the .ini
void keybind_reset();                           // all defaults; saves the .ini
unsigned keybind_version();                     // changes whenever a binding changes

// Controller bindings, stored as "pad = cross=a,circle=b,...". An input is an SDL game controller
// button/axis name ("a", "dpup", "lefttrigger", "leftx-") or, for controllers SDL does not know,
// a raw joystick input: "b3" (button), "h0u" (hat direction u/d/l/r), "a2+" (axis direction).
std::string padbind_input(int i);
void padbind_set(int i, const std::string& input);
void padbind_reset();
