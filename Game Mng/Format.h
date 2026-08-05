#pragma once
#include <string>
#include <sstream>
#include <iomanip>
#include <cmath>

// Formats a number with a K/M/B/T suffix once it gets large, so idle-game
// scale values (huge stockpiles, late-game cash) stay readable instead of
// printing a wall of digits. Small values just get two decimal places.
inline std::string formatNumber(double value) {
    double absVal = std::fabs(value);
    const char* suffix = "";
    double divisor = 1.0;
    if (absVal >= 1e12) { suffix = "T"; divisor = 1e12; }
    else if (absVal >= 1e9) { suffix = "B"; divisor = 1e9; }
    else if (absVal >= 1e6) { suffix = "M"; divisor = 1e6; }
    else if (absVal >= 1e3) { suffix = "K"; divisor = 1e3; }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << (value / divisor) << suffix;
    return oss.str();
}
