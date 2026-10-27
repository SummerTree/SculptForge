// SculptForge - open source digital sculpting
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Keyframed blend-shape weights (a baked face take, core/Face.h): exported as a glTF animation on the morph
// target weights and as USD time samples on the SkelAnimation's blendShapeWeights.
#pragma once

#include <string>
#include <vector>

namespace sf {

struct MorphAnimation {
    std::string name = "Take";
    std::vector<std::string> channels; // blend shape (sculpt layer) names
    std::vector<float> times;          // seconds from the start, increasing
    std::vector<float> values;         // times.size() rows of channels.size() weights
    float fps = 30.0f;                 // the rate the keys were baked at (USD timeCodesPerSecond)

    bool empty() const { return times.empty() || channels.empty(); }
    float duration() const { return times.empty() ? 0.0f : times.back(); }
    int channel(const std::string& n) const {
        for (size_t i = 0; i < channels.size(); ++i)
            if (channels[i] == n) return int(i);
        return -1;
    }
    float value(size_t key, size_t ch) const { return values[key * channels.size() + ch]; }
};

} // namespace sf
