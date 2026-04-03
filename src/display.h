#pragma once
#ifdef MODE_ROVER

#include "gnss.h"

// Abstract display interface.
// Concrete drivers (OLED, TFT, …) subclass IDisplay and implement init()/update().
// Add the driver's header + source file and swap the pointer in main.cpp.
class IDisplay {
public:
    virtual ~IDisplay() = default;
    virtual void init() = 0;
    virtual void update(const GnssData& data) = 0;
};

// Default no-op implementation used until a real driver is connected.
class NullDisplay : public IDisplay {
public:
    void init() override {}
    void update(const GnssData& /*data*/) override {}
};

#endif // MODE_ROVER
