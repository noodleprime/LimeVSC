#pragma once

#include "limecore.h"

#include <cstdint>

namespace lime {

inline constexpr std::uint64_t kPinTag  = std::uint64_t{1} << 63;
inline constexpr std::uint64_t kLinkTag = std::uint64_t{1} << 62;

inline constexpr std::uint64_t encNodeId(std::uint32_t node) {
    return std::uint64_t{node} + 1;
}
inline constexpr std::uint32_t decNodeId(std::uint64_t raw) {
    return static_cast<std::uint32_t>(raw - 1);
}

inline constexpr std::uint64_t encPinId(std::uint32_t node, std::uint32_t pin) {
    return kPinTag | (std::uint64_t{node} << 32) | std::uint64_t{pin};
}
inline constexpr std::uint32_t decPinNode(std::uint64_t raw) {
    return static_cast<std::uint32_t>((raw & ~kPinTag) >> 32);
}
inline constexpr std::uint32_t decPinName(std::uint64_t raw) {
    return static_cast<std::uint32_t>(raw & 0xFFFFFFFFu);
}

inline constexpr std::uint64_t encLinkId(std::uint32_t index) {
    return kLinkTag | std::uint64_t{index};
}
inline constexpr std::uint32_t decLinkIndex(std::uint64_t raw) {
    return static_cast<std::uint32_t>(raw & ~(kLinkTag | kPinTag));
}

inline constexpr bool isPinId(std::uint64_t raw)  { return (raw & kPinTag) != 0; }
inline constexpr bool isLinkId(std::uint64_t raw) {
    return (raw & kLinkTag) != 0 && (raw & kPinTag) == 0;
}
inline constexpr bool isNodeId(std::uint64_t raw) {
    return raw != 0 && (raw & (kPinTag | kLinkTag)) == 0;
}

}
