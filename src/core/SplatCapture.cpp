// SculptForge - open source digital sculpting
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/SplatCapture.h"

#include "core/Json.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace sf::capture {

namespace fs = std::filesystem;

namespace {

fs::path u8path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string pathString(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}
std::string lowerExt(const fs::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return e;
}
bool readText(const std::string& path, std::string& out) {
    std::ifstream f(u8path(path), std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// A 4x4 matrix from JSON: nested rows [[..],[..],[..],[..]] or 16 numbers, row-major.
bool readMatrix(const Json& j, glm::mat4& m) {
    float v[16];
    if (j.isArray() && j.size() == 4) {
        for (int r = 0; r < 4; ++r) {
            const Json& row = j[size_t(r)];
            if (!row.isArray() || row.size() != 4) return false;
            for (int c = 0; c < 4; ++c) v[r * 4 + c] = float(row[size_t(c)].asNumber());
        }
    } else if (j.isArray() && j.size() == 16) {
        for (int i = 0; i < 16; ++i) v[i] = float(j[size_t(i)].asNumber());
    } else {
        return false;
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) m[c][r] = v[r * 4 + c]; // glm is column-major
    return true;
}

Json matrixRows(const glm::mat4& m) {
    Json rows = Json::array();
    for (int r = 0; r < 4; ++r) {
        Json row = Json::array();
        for (int c = 0; c < 4; ++c) row.push(double(m[c][r]));
        rows.push(std::move(row));
    }
    return rows;
}

void readIntrinsics(const Json& f, CameraView& v) {
    v.fx = float(f["fl_x"].asNumber(v.fx));
    v.fy = float(f["fl_y"].asNumber(v.fx));
    v.cx = float(f["cx"].asNumber(v.cx));
    v.cy = float(f["cy"].asNumber(v.cy));
    v.width = int(f["w"].asNumber(v.width));
    v.height = int(f["h"].asNumber(v.height));
    if (v.cx <= 0 && v.width > 0) v.cx = float(v.width) * 0.5f; // principal point: the image centre by default
    if (v.cy <= 0 && v.height > 0) v.cy = float(v.height) * 0.5f;
}

// COLMAP pose (world-to-camera rotation as a quaternion and translation, OpenCV axes) to an OpenGL camToWorld.
glm::mat4 colmapToCamToWorld(double qw, double qx, double qy, double qz, double tx, double ty, double tz) {
    const glm::mat3 r = glm::mat3_cast(glm::quat(float(qw), float(qx), float(qy), float(qz)));
    const glm::mat3 rt = glm::transpose(r);
    const glm::vec3 centre = -(rt * glm::vec3(float(tx), float(ty), float(tz)));
    glm::mat4 m(1.0f);
    m[0] = glm::vec4(rt[0], 0.0f);
    m[1] = glm::vec4(-rt[1], 0.0f); // OpenCV y down -> OpenGL y up
    m[2] = glm::vec4(-rt[2], 0.0f); // OpenCV looks down +z -> OpenGL -z
    m[3] = glm::vec4(centre, 1.0f);
    return m;
}

bool readColmapImagesBin(const std::string& path, std::vector<CameraView>& views) {
    std::ifstream f(u8path(path), std::ios::binary);
    if (!f) return false;
    auto rd = [&](void* p, size_t n) { return bool(f.read(static_cast<char*>(p), std::streamsize(n))); };
    uint64_t count = 0;
    if (!rd(&count, 8) || count > 10'000'000) return false;
    for (uint64_t i = 0; i < count; ++i) {
        uint32_t id = 0, camera = 0;
        double q[4], t[3];
        if (!rd(&id, 4) || !rd(q, sizeof q) || !rd(t, sizeof t) || !rd(&camera, 4)) return false;
        std::string name;
        for (char c; f.get(c) && c != '\0';) name += c;
        uint64_t points = 0;
        if (!rd(&points, 8) || points > 100'000'000) return false;
        f.seekg(std::streamoff(points * 24), std::ios::cur); // x, y (double) + point3D id (int64)
        if (!f) return false;
        CameraView v;
        v.file = "images/" + name;
        v.camToWorld = colmapToCamToWorld(q[0], q[1], q[2], q[3], t[0], t[1], t[2]);
        views.push_back(std::move(v));
    }
    return true;
}

bool readColmapImagesTxt(const std::string& path, std::vector<CameraView>& views) {
    std::ifstream f(u8path(path));
    if (!f) return false;
    std::string line;
    bool poseLine = true;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (poseLine) {
            std::istringstream ss(line);
            long id = 0, camera = 0;
            double q[4], t[3];
            std::string name;
            if (!(ss >> id >> q[0] >> q[1] >> q[2] >> q[3] >> t[0] >> t[1] >> t[2] >> camera >> name)) return false;
            CameraView v;
            v.file = "images/" + name;
            v.camToWorld = colmapToCamToWorld(q[0], q[1], q[2], q[3], t[0], t[1], t[2]);
            views.push_back(std::move(v));
        }
        poseLine = !poseLine; // every pose line is followed by its 2D points line
    }
    return true;
}

} // namespace

double sharpness(const Image& image) {
    if (image.empty()) return 0.0;
    const Image small = image.downscaled(512);
    const int w = small.width, h = small.height;
    if (w < 3 || h < 3) return 0.0;
    std::vector<float> lum(size_t(w) * size_t(h));
    for (size_t i = 0; i < lum.size(); ++i) {
        const uint8_t* p = &small.rgba[i * 4];
        lum[i] = (0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]) / 255.0f;
    }
    double sum = 0.0, sum2 = 0.0;
    size_t n = 0;
    for (int y = 1; y < h - 1; ++y)
        for (int x = 1; x < w - 1; ++x) {
            const size_t i = size_t(y) * size_t(w) + size_t(x);
            const double lap = double(lum[i - 1]) + lum[i + 1] + lum[i - size_t(w)] + lum[i + size_t(w)] - 4.0 * lum[i];
            sum += lap;
            sum2 += lap * lap;
            ++n;
        }
    const double mean = sum / double(n);
    return std::max(0.0, sum2 / double(n) - mean * mean) * 1000.0;
}

std::vector<size_t> selectFrames(const std::vector<double>& scores, size_t count) {
    std::vector<size_t> out;
    const size_t n = scores.size();
    if (count == 0 || n == 0) return out;
    if (count >= n) {
        for (size_t i = 0; i < n; ++i) out.push_back(i);
        return out;
    }
    for (size_t k = 0; k < count; ++k) {
        const size_t begin = k * n / count, end = std::max(begin + 1, (k + 1) * n / count);
        size_t best = begin;
        for (size_t i = begin + 1; i < end; ++i)
            if (scores[i] > scores[best]) best = i;
        out.push_back(best);
    }
    return out;
}

bool writeTransformsJson(const std::string& path, const std::vector<CameraView>& views, std::string& error) {
    Json root = Json::object();
    root.set("camera_model", "PINHOLE");
    Json frames = Json::array();
    for (const CameraView& v : views) {
        Json f = Json::object();
        f.set("file_path", v.file);
        f.set("transform_matrix", matrixRows(v.camToWorld));
        if (v.fx > 0) {
            f.set("fl_x", double(v.fx));
            f.set("fl_y", double(v.fy > 0 ? v.fy : v.fx));
        }
        if (v.width > 0 && v.height > 0) {
            f.set("cx", double(v.cx > 0 ? v.cx : v.width * 0.5f));
            f.set("cy", double(v.cy > 0 ? v.cy : v.height * 0.5f));
            f.set("w", v.width);
            f.set("h", v.height);
        }
        frames.push(std::move(f));
    }
    root.set("frames", std::move(frames));
    std::ofstream out(u8path(path), std::ios::binary);
    if (!out) {
        error = "Cannot write " + path;
        return false;
    }
    out << root.dump(true);
    if (!out) {
        error = "Cannot write " + path;
        return false;
    }
    return true;
}

bool readTransformsJson(const std::string& path, std::vector<CameraView>& views, std::string& error) {
    std::string text;
    if (!readText(path, text)) {
        error = "Cannot read " + path;
        return false;
    }
    Json root;
    if (!Json::parse(text, root, error)) return false;
    CameraView shared;
    readIntrinsics(root, shared);
    for (const Json& f : root["frames"].items()) {
        CameraView v = shared;
        v.file = f["file_path"].isString() ? f["file_path"].asString() : std::string();
        if (!readMatrix(f["transform_matrix"], v.camToWorld)) {
            error = "A frame in " + path + " has no 4x4 transform_matrix";
            return false;
        }
        readIntrinsics(f, v);
        views.push_back(std::move(v));
    }
    return true;
}

bool readColmapImages(const std::string& sparseDir, std::vector<CameraView>& views, std::string& error) {
    const fs::path dir = u8path(sparseDir);
    std::error_code ec;
    if (fs::exists(dir / "images.bin", ec)) {
        if (readColmapImagesBin(pathString(dir / "images.bin"), views)) return true;
        error = "Cannot read " + pathString(dir / "images.bin");
        return false;
    }
    if (fs::exists(dir / "images.txt", ec)) {
        if (readColmapImagesTxt(pathString(dir / "images.txt"), views)) return true;
        error = "Cannot read " + pathString(dir / "images.txt");
        return false;
    }
    error = "No COLMAP images.bin or images.txt in " + sparseDir;
    return false;
}

glm::mat4 orbitFraming(const std::vector<CameraView>& views) {
    if (views.size() < 2) return glm::mat4(1.0f);
    // The point closest to every viewing ray: sum (I - d d^T) p = sum (I - d d^T) c.
    glm::dmat3 a(0.0);
    glm::dvec3 b(0.0), up(0.0), mean(0.0);
    for (const CameraView& v : views) {
        const glm::dvec3 c(v.camToWorld[3]);
        const glm::dvec3 d = glm::normalize(-glm::dvec3(v.camToWorld[2]));
        const glm::dmat3 p = glm::dmat3(1.0) - glm::outerProduct(d, d);
        a += p;
        b += p * c;
        up += glm::dvec3(v.camToWorld[1]);
        mean += c;
    }
    mean /= double(views.size());
    glm::dvec3 target = mean;
    if (std::abs(glm::determinant(a)) > 1e-6 * double(views.size())) target = glm::inverse(a) * b;
    // Cameras all looking the same way (a straight walk past something) give no
    // sensible crossing: fall back to the middle of the cameras.
    if (!std::isfinite(target.x) || !std::isfinite(target.y) || !std::isfinite(target.z)) target = mean;
    std::vector<double> dist;
    for (const CameraView& v : views) dist.push_back(glm::length(glm::dvec3(v.camToWorld[3]) - target));
    std::nth_element(dist.begin(), dist.begin() + std::ptrdiff_t(dist.size() / 2), dist.end());
    const double median = dist[dist.size() / 2];
    const double scale = median > 1e-9 ? 3.0 / median : 1.0;

    glm::quat turn(1.0f, 0.0f, 0.0f, 0.0f);
    if (glm::length(up) > 1e-9) {
        const glm::vec3 u = glm::normalize(glm::vec3(up)), y(0.0f, 1.0f, 0.0f);
        const float cosA = glm::clamp(glm::dot(u, y), -1.0f, 1.0f);
        glm::vec3 axis = glm::cross(u, y);
        if (glm::length(axis) < 1e-6f) axis = cosA > 0.0f ? y : glm::vec3(1.0f, 0.0f, 0.0f); // aligned or opposite
        turn = glm::angleAxis(std::acos(cosA), glm::normalize(axis));
    }
    glm::mat4 m = glm::scale(glm::mat4(1.0f), glm::vec3(float(scale)));
    m = m * glm::mat4_cast(turn);
    m = glm::translate(m, -glm::vec3(target));
    return m;
}

bool readCaptureJson(const std::string& path, CaptureInfo& info, std::string& error) {
    std::string text;
    if (!readText(path, text)) {
        error = "Cannot read " + path;
        return false;
    }
    Json root;
    if (!Json::parse(text, root, error)) return false;
    if (!root.isObject()) {
        error = path + " is not a JSON object";
        return false;
    }
    if (root["kind"].isString()) info.kind = root["kind"].asString();
    if (root["device"].isString()) info.device = root["device"].asString();
    for (const Json& f : root["files"].items()) {
        if (f.isString()) info.files.push_back(f.asString());
        else if (f["name"].isString()) info.files.push_back(f["name"].asString()); // {"name":..,"t":..}
    }
    CameraView shared;
    readIntrinsics(root, shared);
    for (const Json& f : root["frames"].items()) {
        CameraView v = shared;
        const Json& name = f["file"].isString() ? f["file"] : f["file_path"];
        if (!name.isString() || !readMatrix(f["transform_matrix"], v.camToWorld)) continue;
        v.file = name.asString();
        readIntrinsics(f, v);
        info.poses.push_back(std::move(v));
    }
    return true;
}

bool isVideoFile(const std::string& path) {
    const std::string e = lowerExt(u8path(path));
    return e == ".mp4" || e == ".mov" || e == ".m4v" || e == ".webm" || e == ".mkv" || e == ".avi";
}

std::vector<std::string> listImages(const std::string& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::directory_iterator it(u8path(dir), ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string e = lowerExt(it->path());
        if (e == ".jpg" || e == ".jpeg" || e == ".png") out.push_back(pathString(it->path()));
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string findVideo(const std::string& dir) {
    std::vector<std::string> found;
    std::error_code ec;
    for (fs::directory_iterator it(u8path(dir), ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && isVideoFile(pathString(it->path()))) found.push_back(pathString(it->path()));
    std::sort(found.begin(), found.end());
    return found.empty() ? std::string() : found.front();
}

SourceKind classifySource(const std::string& path) {
    std::error_code ec;
    const fs::path p = u8path(path);
    if (path.empty() || !fs::exists(p, ec)) return SourceKind::Missing;
    if (fs::is_regular_file(p, ec)) return isVideoFile(path) ? SourceKind::Video : SourceKind::Unsupported;
    if (!fs::is_directory(p, ec)) return SourceKind::Unsupported;
    if (fs::exists(p / "transforms.json", ec)) return SourceKind::NerfstudioDataset;
    if (fs::is_directory(p / "images", ec) && fs::is_directory(p / "sparse" / "0", ec)) return SourceKind::ColmapDataset;
    if (!listImages(path).empty()) return SourceKind::Images;
    if (fs::is_directory(p / "images", ec) && !listImages(pathString(p / "images")).empty()) return SourceKind::Images;
    if (!findVideo(path).empty()) return SourceKind::VideoFolder;
    return SourceKind::Unsupported;
}

const char* sourceKindName(SourceKind kind) {
    switch (kind) {
    case SourceKind::Missing: return "missing";
    case SourceKind::Video: return "video";
    case SourceKind::Images: return "photos";
    case SourceKind::VideoFolder: return "video";
    case SourceKind::NerfstudioDataset: return "nerfstudio dataset";
    case SourceKind::ColmapDataset: return "COLMAP dataset";
    case SourceKind::Unsupported: break;
    }
    return "unsupported";
}

const std::vector<TrainPreset>& trainPresets() {
    static const std::vector<TrainPreset> presets = {
        {"fast", "Fast preview", 5000, 1024, 1, 250, 600'000, 80},
        {"quality", "Quality", 30000, 1920, 3, 1000, 3'000'000, 200},
    };
    return presets;
}

const TrainPreset& trainPreset(const std::string& id) {
    for (const TrainPreset& p : trainPresets())
        if (id == p.id) return p;
    return trainPresets().front();
}

std::vector<std::string> brushArguments(const std::string& dataset, const std::string& exportDir,
                                        const TrainPreset& preset, bool evaluate) {
    std::vector<std::string> a = {dataset,
                                  "--total-steps", std::to_string(preset.steps),
                                  "--max-resolution", std::to_string(preset.maxResolution),
                                  "--sh-degree", std::to_string(preset.shDegree),
                                  "--max-splats", std::to_string(preset.maxSplats),
                                  "--export-every", std::to_string(preset.exportEvery),
                                  "--export-path", exportDir,
                                  "--export-name", "export_{iter}.ply"};
    if (evaluate) { // hold back every 8th view to measure PSNR on
        a.insert(a.end(), {"--eval-split-every", "8", "--eval-every", std::to_string(preset.exportEvery * 2)});
    }
    return a;
}

bool parseTrainerLog(const std::string& line, TrainerLog& out) {
    out = TrainerLog();
    if (const size_t p = line.find("Refine iter "); p != std::string::npos) {
        int iter = -1, splats = -1;
        if (std::sscanf(line.c_str() + p, "Refine iter %d, %d splats", &iter, &splats) >= 1) {
            out.refineIter = iter;
            out.splats = splats;
            return true;
        }
    }
    if (const size_t p = line.find("Eval iter "); p != std::string::npos) {
        int iter = -1;
        double psnr = -1.0, ssim = -1.0;
        if (std::sscanf(line.c_str() + p, "Eval iter %d: PSNR %lf, ssim %lf", &iter, &psnr, &ssim) >= 2) {
            out.evalIter = iter;
            out.psnr = psnr;
            out.ssim = ssim;
            return true;
        }
    }
    if (line.find("panicked at") != std::string::npos || line.rfind("Error:", 0) == 0 ||
        line.find(" ERROR ") != std::string::npos) {
        out.error = true;
        return true;
    }
    return false;
}

int checkpointStep(const std::string& fileName) {
    const fs::path p = u8path(fileName);
    if (lowerExt(p) != ".ply") return -1;
    const std::string stem = pathString(p.stem());
    size_t end = stem.size(), begin = end;
    while (begin > 0 && std::isdigit(static_cast<unsigned char>(stem[begin - 1]))) --begin;
    if (begin == end || end - begin > 9) return -1;
    return std::stoi(stem.substr(begin, end - begin));
}

std::string latestCheckpoint(const std::string& dir, int& step) {
    step = -1;
    std::string best;
    std::error_code ec;
    for (fs::directory_iterator it(u8path(dir), ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string name = pathString(it->path().filename());
        const int s = checkpointStep(name);
        if (s > step) {
            step = s;
            best = pathString(it->path());
        }
    }
    return best;
}

const char* psnrQuality(double psnr) {
    if (psnr <= 0.0) return "";
    if (psnr < 18.0) return "rough";
    if (psnr < 22.0) return "taking shape";
    if (psnr < 26.0) return "good";
    if (psnr < 30.0) return "sharp";
    return "very sharp";
}

} // namespace sf::capture
