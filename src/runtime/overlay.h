#pragma once
enum { OV_KEY_UP, OV_KEY_DOWN, OV_KEY_LEFT, OV_KEY_RIGHT, OV_KEY_OK, OV_KEY_BACK };
enum { OV_NONE, OV_FULLSCREEN, OV_VSYNC, OV_QUIT, OV_WINDOW };
bool overlay_open();
int overlay_capturing();                     // 1: keyboard page waits for a key, 2: controller page for an input
void overlay_key_captured(const char* name); // SDL key name / controller input, or nullptr to cancel
void overlay_clear_binding();                // unbind the selected row
extern char g_pad_name[64];
void overlay_toggle();
int overlay_input(int key);          // emulation/window thread
void overlay_draw(int ww, int wh);   // presentation thread, GL context current
extern char g_perf_text[128];
