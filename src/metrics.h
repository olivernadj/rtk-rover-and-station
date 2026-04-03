#pragma once
#include "gnss.h"
#include <stddef.h>

// Formats a JSON metrics payload into buf.
// Returns bytes written (excluding null terminator), or 0 on error/truncation.
size_t metricsFormat(char* buf, size_t len, const GnssData& data);
