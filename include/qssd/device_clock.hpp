#pragma once
#include <cstdint>

namespace qssd {
// Frequency of the virtual storage oscillator, in cycles per guest second.
inline constexpr std::uint64_t device_clock_hz = 4'500'000'000'000ULL;
}  // namespace qssd
