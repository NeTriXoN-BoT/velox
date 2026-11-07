// Things the launcher draws on top of the emulator: the animated boot screen and the custom
// mouse cursor shown while the pointer is over the phone.
#pragma once

#include "common.hpp"

namespace phone::overlay {

// Shows the boot screen over the phone's display until phone::boot says it's done.
void showBootScreen(const std::wstring& subtitle, bool roundCorners, int style);
// Plays a boot screen style in a small window of its own, to preview it.
void previewBootScreen(int style);
bool bootScreenVisible();

// Custom cursor: 0 = the normal Windows cursor.
void setCursorStyle(int style);
void tickCursor();     // call every ~30 ms; swaps the cursor while it's over the phone
void restoreCursor();  // puts the user's own cursors back

}  // namespace phone::overlay
