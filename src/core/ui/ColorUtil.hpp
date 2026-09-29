#pragma once

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace worldanalysis::ui {

inline int colorHexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline std::string_view trimColorText(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
    return value;
}

inline std::optional<std::uint32_t> parseColorText(std::string_view raw) {
    std::string_view value = trimColorText(raw);
    if (value.empty()) return std::nullopt;

    // Launcher color controls have used both #RRGGBB and Android-style
    // #AARRGGBB encodings over time. Accept both, plus their short forms.
    if (value.front() == '#') {
        value.remove_prefix(1);
        if (value.size() == 3 || value.size() == 4) {
            const bool hasAlpha = value.size() == 4;
            std::uint32_t n[4]{255, 0, 0, 0};
            std::size_t dst = hasAlpha ? 0 : 1;
            for (char c : value) {
                const int h = colorHexNibble(c);
                if (h < 0) return std::nullopt;
                n[dst++] = static_cast<std::uint32_t>((h << 4) | h);
            }
            return (n[0] << 24) | (n[1] << 16) | (n[2] << 8) | n[3];
        }
        if (value.size() == 6 || value.size() == 8) {
            std::uint32_t packed = 0;
            for (char c : value) {
                const int h = colorHexNibble(c);
                if (h < 0) return std::nullopt;
                packed = (packed << 4) | static_cast<std::uint32_t>(h);
            }
            if (value.size() == 6) packed |= 0xFF000000u;
            return packed; // eight-digit form is #AARRGGBB
        }
        return std::nullopt;
    }

    if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
        value.remove_prefix(2);
        if (value.empty() || value.size() > 8) return std::nullopt;
        std::uint32_t packed = 0;
        for (char c : value) {
            const int h = colorHexNibble(c);
            if (h < 0) return std::nullopt;
            packed = (packed << 4) | static_cast<std::uint32_t>(h);
        }
        if (value.size() <= 6) packed |= 0xFF000000u;
        return packed;
    }

    auto startsWithInsensitive = [](std::string_view text, std::string_view prefix) {
        if (text.size() < prefix.size()) return false;
        for (std::size_t i = 0; i < prefix.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(text[i])) !=
                std::tolower(static_cast<unsigned char>(prefix[i]))) return false;
        }
        return true;
    };
    if (startsWithInsensitive(value, "rgb(")) {
        const std::size_t close = value.find(')', 4);
        if (close == std::string_view::npos) return std::nullopt;
        std::string args(value.substr(4, close - 4));
        int r = -1, g = -1, b = -1;
        if (std::sscanf(args.c_str(), " %d , %d , %d ", &r, &g, &b) == 3 &&
            r >= 0 && r <= 255 && g >= 0 && g <= 255 && b >= 0 && b <= 255) {
            return 0xFF000000u | (static_cast<std::uint32_t>(r) << 16) |
                   (static_cast<std::uint32_t>(g) << 8) | static_cast<std::uint32_t>(b);
        }
        return std::nullopt;
    }

    // Some Android bridges stringify the packed Color int (occasionally as a
    // signed 32-bit value). Accept either signed or unsigned decimal forms.
    long long signedValue = 0;
    const auto* first = value.data();
    const auto* last = value.data() + value.size();
    const auto parsed = std::from_chars(first, last, signedValue, 10);
    if (parsed.ec == std::errc{} && parsed.ptr == last) {
        const auto packed = static_cast<std::uint32_t>(signedValue);
        return ((packed & 0xFF000000u) == 0u) ? (0xFF000000u | packed) : packed;
    }
    return std::nullopt;
}

inline std::uint32_t parseColorOr(std::string_view value, std::uint32_t fallback) {
    if (const auto parsed = parseColorText(value)) return *parsed;
    return fallback;
}

inline std::string normalizeColorString(std::string_view value, std::string_view fallback = "#FFFFFF") {
    auto parsed = parseColorText(value);
    if (!parsed) parsed = parseColorText(fallback);
    const std::uint32_t rgb = parsed.value_or(0xFFFFFFFFu) & 0x00FFFFFFu;
    char text[8]{};
    std::snprintf(text, sizeof(text), "#%06X", rgb);
    return text;
}

inline std::uint32_t withAlpha(std::uint32_t argb, float alphaScale) {
    alphaScale = std::clamp(alphaScale, 0.0f, 1.0f);
    const std::uint32_t sourceAlpha = (argb >> 24) & 0xFFu;
    const std::uint32_t alpha = static_cast<std::uint32_t>(sourceAlpha * alphaScale + 0.5f);
    return (alpha << 24) | (argb & 0x00FFFFFFu);
}

} // namespace worldanalysis::ui
