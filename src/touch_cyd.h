#pragma once
#if defined(MODE_ROVER) && defined(BOARD_CYD)

#include <stdint.h>

enum class TouchGesture : uint8_t {
    None,
    Tap,
    LongPress,
};

struct TouchEvent {
    TouchGesture gesture;
    int16_t      x;       // screen-space x (0..319) at the moment the gesture fired
    int16_t      y;       // screen-space y (0..239)
    int8_t       button;  // 0..3 for A..D, or -1 if not over a HUD button
};

void       touchInit();
TouchEvent touchPoll();   // returns {None,...} if no completed gesture this tick

#endif
