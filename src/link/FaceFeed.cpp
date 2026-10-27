// SculptForge - open source digital sculpting
// SPDX-License-Identifier: GPL-3.0-or-later
#include "link/FaceFeed.h"

#include "core/Json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

namespace sf::link {

namespace {

std::filesystem::path u8path(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }

void appendNumber(std::string& out, double v, int decimals) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    // Trim trailing zeros ("0.3100" -> "0.31", "1.0000" -> "1").
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    out += s;
}

} // namespace

const std::array<const char*, kArkitShapeCount>& arkitShapeNames() {
    static const std::array<const char*, kArkitShapeCount> names = {
        "eyeBlinkLeft",      "eyeLookDownLeft",    "eyeLookInLeft",     "eyeLookOutLeft",     "eyeLookUpLeft",
        "eyeSquintLeft",     "eyeWideLeft",        "eyeBlinkRight",     "eyeLookDownRight",   "eyeLookInRight",
        "eyeLookOutRight",   "eyeLookUpRight",     "eyeSquintRight",    "eyeWideRight",       "jawForward",
        "jawLeft",           "jawRight",           "jawOpen",           "mouthClose",         "mouthFunnel",
        "mouthPucker",       "mouthLeft",          "mouthRight",        "mouthSmileLeft",     "mouthSmileRight",
        "mouthFrownLeft",    "mouthFrownRight",    "mouthDimpleLeft",   "mouthDimpleRight",   "mouthStretchLeft",
        "mouthStretchRight", "mouthRollLower",     "mouthRollUpper",    "mouthShrugLower",    "mouthShrugUpper",
        "mouthPressLeft",    "mouthPressRight",    "mouthLowerDownLeft", "mouthLowerDownRight", "mouthUpperUpLeft",
        "mouthUpperUpRight", "browDownLeft",       "browDownRight",     "browInnerUp",        "browOuterUpLeft",
        "browOuterUpRight",  "cheekPuff",          "cheekSquintLeft",   "cheekSquintRight",   "noseSneerLeft",
        "noseSneerRight",    "tongueOut"};
    return names;
}

int arkitShapeIndex(const std::string& name) {
    const auto& names = arkitShapeNames();
    for (int i = 0; i < kArkitShapeCount; ++i)
        if (name == names[size_t(i)]) return i;
    return -1;
}

int arkitMirrorIndex(int index) {
    if (index < 0 || index >= kArkitShapeCount) return index;
    std::string name = arkitShapeNames()[size_t(index)];
    std::string other;
    if (name.size() > 4 && name.compare(name.size() - 4, 4, "Left") == 0) other = name.substr(0, name.size() - 4) + "Right";
    else if (name.size() > 5 && name.compare(name.size() - 5, 5, "Right") == 0) other = name.substr(0, name.size() - 5) + "Left";
    else if (name == "jawLeft") other = "jawRight";
    else if (name == "jawRight") other = "jawLeft";
    else if (name == "mouthLeft") other = "mouthRight";
    else if (name == "mouthRight") other = "mouthLeft";
    else return index;
    const int i = arkitShapeIndex(other);
    return i >= 0 ? i : index;
}

float FaceFrame::weight(const char* name) const {
    const int i = arkitShapeIndex(name);
    return i >= 0 && present[size_t(i)] ? weights[size_t(i)] : 0.0f;
}

int FaceFrame::presentCount() const { return int(std::count(present.begin(), present.end(), true)); }

FaceFrame FaceFrame::mirrored() const {
    FaceFrame m = *this;
    for (int i = 0; i < kArkitShapeCount; ++i) {
        const int j = arkitMirrorIndex(i);
        m.weights[size_t(j)] = weights[size_t(i)];
        m.present[size_t(j)] = present[size_t(i)];
    }
    // Mirroring across the YZ plane: x stays, the rotation's y and z flip.
    m.head = glm::quat(head.w, head.x, -head.y, -head.z);
    return m;
}

std::string FaceFrame::weightsJson() const {
    std::string out = "{";
    bool first = true;
    for (int i = 0; i < kArkitShapeCount; ++i) {
        if (!present[size_t(i)]) continue;
        if (!first) out += ',';
        first = false;
        out += '"';
        out += arkitShapeNames()[size_t(i)];
        out += "\":";
        appendNumber(out, weights[size_t(i)], 4);
    }
    return out + "}";
}

std::string FaceFrame::messageJson() const {
    std::string out = "{\"type\":\"face\",\"t\":";
    appendNumber(out, t, 1);
    out += ",\"weights\":" + weightsJson();
    if (hasHead) {
        out += ",\"head\":[";
        const float q[4] = {head.x, head.y, head.z, head.w};
        for (int i = 0; i < 4; ++i) {
            if (i) out += ',';
            appendNumber(out, q[i], 5);
        }
        out += ']';
    }
    return out + "}";
}

bool parseFaceMessage(const std::string& text, FaceFrame& frame, std::string& error) {
    if (text.size() > kMaxFaceMessageBytes) {
        error = "Face message too large";
        return false;
    }
    Json msg;
    if (!Json::parse(text, msg, error)) return false;
    if (!msg.isObject() || msg["type"].asString() != "face") {
        error = "Not a face message";
        return false;
    }
    FaceFrame f;
    if (msg.has("t")) {
        if (!msg["t"].isNumber() || !std::isfinite(msg["t"].asNumber())) {
            error = "Face message: t must be a number";
            return false;
        }
        f.t = msg["t"].asNumber();
    }
    const Json& weights = msg["weights"];
    if (!weights.isObject()) {
        error = "Face message: weights must be an object";
        return false;
    }
    for (const auto& [name, value] : weights.members()) {
        if (!value.isNumber() || !std::isfinite(value.asNumber())) {
            error = "Face message: weight " + name + " is not a number";
            return false;
        }
        const int i = arkitShapeIndex(name);
        if (i < 0) {
            if (name != "_neutral") ++f.unknownNames;
            continue;
        }
        f.weights[size_t(i)] = std::clamp(float(value.asNumber()), 0.0f, 1.0f);
        f.present[size_t(i)] = true;
    }
    if (msg.has("head") && !msg["head"].isNull()) {
        const Json& h = msg["head"];
        if (!h.isArray() || h.size() != 4) {
            error = "Face message: head must be [qx, qy, qz, qw]";
            return false;
        }
        float q[4];
        for (size_t i = 0; i < 4; ++i) {
            if (!h[i].isNumber() || !std::isfinite(h[i].asNumber())) {
                error = "Face message: head must be four numbers";
                return false;
            }
            q[i] = float(h[i].asNumber());
        }
        const float len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        if (len < 1e-6f) {
            error = "Face message: head quaternion is zero";
            return false;
        }
        f.head = glm::quat(q[3] / len, q[0] / len, q[1] / len, q[2] / len);
        f.hasHead = true;
    }
    frame = f;
    return true;
}

bool FaceTakeWriter::open(const std::string& path, std::string& error) {
    close();
    out_.open(u8path(path), std::ios::binary | std::ios::trunc);
    if (!out_) {
        error = "Cannot write " + path;
        return false;
    }
    frames_ = 0;
    firstT_ = lastT_ = 0.0;
    return true;
}

void FaceTakeWriter::write(const FaceFrame& frame) {
    if (!out_.is_open()) return;
    if (frames_ == 0) firstT_ = frame.t;
    lastT_ = frame.t;
    ++frames_;
    out_ << frame.messageJson() << '\n';
}

void FaceTakeWriter::close() {
    if (out_.is_open()) out_.close();
}

bool readFaceTake(const std::string& path, std::vector<FaceFrame>& frames, std::string& error) {
    std::ifstream in(u8path(path), std::ios::binary);
    if (!in) {
        error = "Cannot read " + path;
        return false;
    }
    frames.clear();
    std::string line;
    int number = 0;
    while (std::getline(in, line)) {
        ++number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        FaceFrame f;
        if (!parseFaceMessage(line, f, error)) {
            error = path + " line " + std::to_string(number) + ": " + error;
            return false;
        }
        frames.push_back(f);
    }
    return true;
}

} // namespace sf::link
