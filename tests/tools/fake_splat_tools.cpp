// SculptForge - open source digital sculpting
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A stand-in for the external splat tools, so training is tested without a
// GPU, COLMAP or ffmpeg (tests/test_splat_train.cpp, tests/ui/splat_train.sfui).
// One program, three roles, chosen by its arguments the way SculptForge calls
// the real ones:
//  - ffmpeg  (-i VIDEO ... OUT/%05d.jpg): writes FAKE_FFMPEG_FRAMES (40) frames;
//    every 4th one is blurred, so frame selection has something to drop
//  - colmap  (automatic_reconstructor --workspace_path W --image_path I ...):
//    writes W/sparse/0/images.txt with the cameras on a circle around (1,2,3),
//    world up = +Z (as arbitrary as a real reconstruction); FAKE_COLMAP_FAIL=1 fails
//  - Brush   (DATASET --total-steps N --export-every K --export-path DIR ...):
//    checks the dataset, prints Brush's log lines and writes export_<step>.ply
//    checkpoints of a sphere around (1,2,3) that gets sharper;
//    FAKE_BRUSH_MS_PER_EXPORT (20) slows it down, FAKE_BRUSH_FAIL_AT=<step> crashes
#include "core/Splats.h"
#include "io/PlyIO.h"

#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

int envInt(const char* name, int fallback) {
    const char* v = std::getenv(name);
    return v && *v ? std::atoi(v) : fallback;
}

std::map<std::string, std::string> options(int argc, char** argv, int first, std::vector<std::string>& positional) {
    std::map<std::string, std::string> o;
    for (int i = first; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind("--", 0) == 0 && i + 1 < argc) o[a.substr(2)] = argv[++i];
        else positional.push_back(a);
    }
    return o;
}

void writeJpg(const std::string& path, int w, int h, const std::vector<uint8_t>& rgb) {
    std::vector<uint8_t> bytes;
    stbi_write_jpg_to_func(
        [](void* ctx, void* data, int size) {
            auto* out = static_cast<std::vector<uint8_t>*>(ctx);
            out->insert(out->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
        },
        &bytes, w, h, 3, rgb.data(), 90);
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

int fakeFfmpeg(int argc, char** argv) {
    const std::string pattern = argv[argc - 1];
    const int frames = envInt("FAKE_FFMPEG_FRAMES", 40);
    const int w = 96, h = 64;
    for (int f = 1; f <= frames; ++f) {
        std::vector<uint8_t> rgb(size_t(w) * h * 3);
        const bool blurred = f % 4 == 0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                // A checkerboard that slides a little each frame; blurred frames are flat grey.
                const int v = blurred ? 128 : (((x + f) / 6 + y / 6) % 2 ? 220 : 40);
                for (int c = 0; c < 3; ++c) rgb[(size_t(y) * w + x) * 3 + c] = uint8_t(v);
            }
        char name[512];
        std::snprintf(name, sizeof name, pattern.c_str(), f);
        writeJpg(name, w, h, rgb);
    }
    return 0;
}

int fakeColmap(int argc, char** argv) {
    std::vector<std::string> pos;
    auto o = options(argc, argv, 2, pos);
    if (envInt("FAKE_COLMAP_FAIL", 0)) {
        std::printf("E20261026 Not enough matches\n");
        return 1;
    }
    std::vector<std::string> names;
    for (const auto& e : fs::directory_iterator(o["image_path"])) names.push_back(e.path().filename().string());
    std::sort(names.begin(), names.end());
    const fs::path sparse = fs::path(o["workspace_path"]) / "sparse" / "0";
    fs::create_directories(sparse);
    std::ofstream cams(sparse / "cameras.txt");
    cams << "1 PINHOLE 96 64 80 80 48 32\n";
    std::ofstream pts(sparse / "points3D.txt");
    std::ofstream img(sparse / "images.txt");
    img << "# fake reconstruction\n";
    const glm::vec3 target(1, 2, 3), up(0, 0, 1);
    for (size_t i = 0; i < names.size(); ++i) {
        std::printf("Processed file [%zu/%zu]\n", i + 1, names.size());
        const float a = 6.2831853f * float(i) / float(names.size());
        const glm::vec3 c = target + 4.0f * glm::vec3(std::cos(a), std::sin(a), 0.5f);
        const glm::vec3 z = glm::normalize(target - c), x = glm::normalize(glm::cross(z, up)),
                        upCam = glm::cross(x, z), y = -upCam;
        glm::mat3 r; // world-to-camera: rows x, y, z
        for (int k = 0; k < 3; ++k) {
            r[k][0] = x[k];
            r[k][1] = y[k];
            r[k][2] = z[k];
        }
        const glm::quat q = glm::quat_cast(r);
        const glm::vec3 t = -(r * c);
        img << i + 1 << ' ' << q.w << ' ' << q.x << ' ' << q.y << ' ' << q.z << ' ' << t.x << ' ' << t.y << ' ' << t.z
            << " 1 " << names[i] << "\n\n";
    }
    std::printf("Registering image #%zu\n", names.size());
    return 0;
}

int fakeBrush(int argc, char** argv) {
    std::vector<std::string> pos;
    auto o = options(argc, argv, 1, pos);
    if (pos.empty() || !fs::is_directory(pos[0])) {
        std::printf("Error: no dataset at %s\n", pos.empty() ? "" : pos[0].c_str());
        return 1;
    }
    const fs::path ds = pos[0];
    if (!fs::exists(ds / "transforms.json") && !fs::exists(ds / "sparse" / "0")) {
        std::printf("Error: %s has neither transforms.json nor sparse/0\n", ds.string().c_str());
        return 1;
    }
    const int total = std::stoi(o.count("total-steps") ? o["total-steps"] : "30000");
    const int every = std::max(1, std::stoi(o.count("export-every") ? o["export-every"] : "5000"));
    const int evalEvery = o.count("eval-every") ? std::stoi(o["eval-every"]) : 0;
    const fs::path out = o.count("export-path") ? o["export-path"] : ".";
    std::string name = o.count("export-name") ? o["export-name"] : "export_{iter}.ply";
    const int ms = envInt("FAKE_BRUSH_MS_PER_EXPORT", 20), failAt = envInt("FAKE_BRUSH_FAIL_AT", -1);
    fs::create_directories(out);
    std::printf("[2026-10-26T00:00:00Z INFO  brush_process::train_stream] Start of training stream\n");
    std::fflush(stdout);
    std::mt19937 rng(42);
    std::normal_distribution<float> normal(0.0f, 1.0f);
    for (int step = every; step <= total + every - 1; step += every) {
        const int iter = std::min(step, total);
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        if (failAt >= 0 && iter >= failAt) {
            std::printf("thread 'main' panicked at crates/brush-train/src/train.rs:1:1:\nout of memory\n");
            std::fflush(stdout);
            return 101;
        }
        const int count = std::min(3000, 300 + iter / 2);
        std::printf("[2026-10-26T00:00:00Z INFO  brush_cli] Refine iter %d, %d splats.\n", iter, count);
        if (evalEvery > 0 && (iter % evalEvery == 0 || iter == total))
            std::printf("[2026-10-26T00:00:00Z INFO  brush_cli] Eval iter %d: PSNR %.2f, ssim %.3f\n", iter,
                        15.0 + 15.0 * iter / total, 0.5 + 0.4 * iter / total);
        std::fflush(stdout);
        sf::SplatCloud cloud;
        cloud.resize(size_t(count), 0);
        const float noise = 0.3f * (1.0f - float(iter) / float(total)) + 0.01f;
        for (int i = 0; i < count; ++i) {
            glm::vec3 d(normal(rng), normal(rng), normal(rng));
            d = glm::normalize(d);
            cloud.positions[size_t(i)] = glm::vec3(1, 2, 3) + 0.5f * d + noise * glm::vec3(normal(rng), normal(rng), normal(rng));
            cloud.rotations[size_t(i)] = glm::quat(1, 0, 0, 0);
            cloud.scales[size_t(i)] = glm::vec3(0.02f);
            cloud.opacities[size_t(i)] = 0.9f;
            cloud.setColor(size_t(i), glm::vec3(0.5f + 0.5f * d.x, 0.5f + 0.5f * d.y, 0.5f + 0.5f * d.z));
        }
        std::string file = name;
        if (const size_t p = file.find("{iter}"); p != std::string::npos) file.replace(p, 6, std::to_string(iter));
        std::string err;
        const fs::path tmp = out / (file + ".part");
        if (!sf::saveSplatPly(tmp.string(), cloud, err)) {
            std::printf("Error: %s\n", err.c_str());
            return 1;
        }
        fs::rename(tmp, out / file);
        if (iter == total) break;
    }
    std::printf("Training took 1s\n");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "-i") return fakeFfmpeg(argc, argv);
    if (argc > 1 && std::string(argv[1]) == "automatic_reconstructor") return fakeColmap(argc, argv);
    return fakeBrush(argc, argv);
}
