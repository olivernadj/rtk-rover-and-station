#pragma once
#if defined(MODE_ROVER) && defined(BOARD_CYD)

#include "display.h"

// CYD (ESP32-2432S028 USB-C) driver for the rover HUD.
// First milestone: renders a plain status screen (WiFi + GNSS fix + coords).
// Second milestone (future commit): replaces the status block with the full
// HUD defined in ../docs or the UI-mockup repo (header / pill / delta band / buttons).
class CydDisplay : public IDisplay {
public:
    void init() override;
    void update(const GnssData& data) override;
};

#endif
