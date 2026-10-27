// SculptForge - open source digital sculpting
// SPDX-License-Identifier: GPL-3.0-or-later
#include "link/Qr.h"

#include "qrcodegen.hpp"

#include <exception>

namespace sf::link {

bool makeQrCode(const std::string& text, QrCode& out, std::string& error) {
    try {
        const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        out.size = qr.getSize();
        out.modules.assign(size_t(out.size) * size_t(out.size), 0);
        for (int y = 0; y < out.size; ++y)
            for (int x = 0; x < out.size; ++x) out.modules[size_t(y * out.size + x)] = qr.getModule(x, y) ? 1 : 0;
        return true;
    } catch (const std::exception& e) {
        error = std::string("QR code: ") + e.what();
        out = {};
        return false;
    }
}

} // namespace sf::link
