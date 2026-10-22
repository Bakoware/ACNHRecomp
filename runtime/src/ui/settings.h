// The settings screen's pages (settings.cpp), drawn by ui.cpp inside an ImGui frame.
#pragma once

struct ImFont;

namespace nx::ui {

bool settings_draw(float scale, ImFont* title);  // false once the player closed the screen
void settings_hint(float scale);                 // the "F1: settings" hint shown for a few seconds at startup

}  // namespace nx::ui
