#pragma once
#if defined(MODE_ROVER) && defined(BOARD_CYD)

#include "display.h"
#include "rover_state.h"

// CYD rover HUD driver. Renders the full HUD from ui/mockup.py one-for-one
// using TFT_eSPI + GFX Free Fonts + drawLine primitives for the arrow icons.
class CydDisplay : public IDisplay {
public:
    void init() override;
    void update(const GnssData& data) override;

private:
    RoverState _state{};

    void drawHeader();
    void drawPresetPill();
    void drawCoords();
    void drawDelta();
    void drawStats();
    void drawButtons();

#ifdef CYD_FAKE_STATE
    // Bench-test helper: rotates `_state` through every visual branch
    // (selected cycling, empty vs saved, corr_age sweeping). Ignores `data`.
    void rotateFakeState();
#endif
};

#endif
