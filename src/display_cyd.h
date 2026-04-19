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
    void selectPreset(uint8_t i) override;
    void savePreset  (uint8_t i) override;

private:
    RoverState _state{};
    bool       _autoSavedA            = false;   // set after first RTK auto-save or an NVS load of slot 0
    bool       _hasLiveFix            = false;   // true once data.valid && fix_type >= 3
    uint32_t   _pillFlashUntilMs      = 0;       // accent override on selectPreset
    uint32_t   _buttonFlashUntilMs[4] = {0, 0, 0, 0};

    void drawHeader();
    void drawPresetPill();
    void drawCoords();
    void drawDelta();
    void drawStats();
    void drawButtons();

    void loadPresetsFromNvs();
    void writePresetToNvs(uint8_t i);

#ifdef CYD_FAKE_STATE
    // Bench-test helper: rotates `_state` through every visual branch
    // (selected cycling, empty vs saved, corr_age sweeping). Ignores `data`.
    void rotateFakeState();
#endif
};

#endif
