// SculptForge - open source digital sculpting
// SPDX-License-Identifier: GPL-3.0-or-later
//
// QR codes for the phone link's pairing window (third_party/qrcodegen, MIT).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sf::link {

struct QrCode {
    int size = 0;                 // modules per side (without the quiet zone)
    std::vector<uint8_t> modules; // size * size, row-major, 1 = dark
    bool dark(int x, int y) const { return x >= 0 && y >= 0 && x < size && y < size && modules[size_t(y * size + x)] != 0; }
};

// Encodes `text` (UTF-8) with medium error correction (a phone reads it from a screen at an angle).
bool makeQrCode(const std::string& text, QrCode& out, std::string& error);

} // namespace sf::link
