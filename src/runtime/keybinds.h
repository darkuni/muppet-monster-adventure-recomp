#pragma once
// Keyboard bindings: one key per PlayStation control, stored in the .ini as
// "keys = cross=X,circle=Z,..." (key names are SDL key names). Unlisted controls use the defaults.
#include <cstdint>
#include <string>

struct KeyBind {
    const char* id;      // name in the .ini
    const char* label;   // shown in the menu
    const char* def;     // default key
    uint32_t bit;        // hostinput.h IN_* bit
};
int keybind_count();
const KeyBind& keybind(int i);
std::string keybind_key(int i);                 // current key name
void keybind_set(int i, const std::string& key);  // saves the .ini
void keybind_reset();                           // all defaults; saves the .ini
unsigned keybind_version();                     // changes whenever a binding changes
