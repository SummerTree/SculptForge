// SculptForge - open source digital sculpting
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Turning a phone capture (a video or a folder of photos) into a dataset a
// Gaussian-splat trainer can read, and reading back what the trainer says.
// No processes or networking here (those are in ai/SplatTrainer.h), so all of
// it is unit-tested:
//  - frame selection: the sharpest frame in each stretch of the video, so the
//    views stay spread around the object and motion blur is dropped
//  - capture.json from the phone link (kind, device, files, and optional
//    camera poses from the phone's AR tracking, which skip COLMAP)
//  - nerfstudio transforms.json writing and COLMAP images.bin/txt reading
//  - framing: the object the cameras orbit becomes the centre, their average
//    up becomes +Y (captures otherwise arrive tilted or upside down)
//  - Brush's command line and its log lines (refine and eval steps, PSNR)
#pragma once

#include "core/Image.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace sf::capture {

// Variance of the Laplacian of the luminance (higher = sharper). The image is
// first reduced to at most 512 px so scores of different sizes are comparable.
double sharpness(const Image& image);

// Picks up to `count` frames in time order: the frames are split into `count`
// equal stretches and the sharpest of each is kept (so a blurry second costs
// one view, not a gap). Returns indices into `scores`, ascending.
std::vector<size_t> selectFrames(const std::vector<double>& scores, size_t count);

// One posed photo. `camToWorld` uses the OpenGL / nerfstudio convention
// (x right, y up, the camera looks down -z). Intrinsics in pixels.
struct CameraView {
    std::string file; // relative to the dataset folder, e.g. "images/00001.jpg"
    glm::mat4 camToWorld{1.0f};
    float fx = 0, fy = 0, cx = 0, cy = 0;
    int width = 0, height = 0;
};

// Writes a nerfstudio transforms.json (read by Brush, nerfstudio, gsplat...).
bool writeTransformsJson(const std::string& path, const std::vector<CameraView>& views, std::string& error);
bool readTransformsJson(const std::string& path, std::vector<CameraView>& views, std::string& error);

// COLMAP sparse model images (world-to-camera, OpenCV axes) converted to CameraView
// (intrinsics are left at 0: only the poses are read).
bool readColmapImages(const std::string& sparseDir, std::vector<CameraView>& views, std::string& error);

// The placement for a capture's splats: moves the point the cameras look at to
// the origin, turns the cameras' average up to +Y and scales so the cameras sit
// about 3 units away (a new sphere has radius 1). Identity for fewer than 2 views.
glm::mat4 orbitFraming(const std::vector<CameraView>& views);

// capture.json written by the phone link next to the uploaded files.
struct CaptureInfo {
    std::string kind;   // "splat", "photo", "ply"
    std::string device; // free text, e.g. "iPhone 15 Pro"
    std::vector<std::string> files; // relative to the capture folder
    std::vector<CameraView> poses;  // optional "frames": AR poses per photo (then no COLMAP is needed)
};
bool readCaptureJson(const std::string& path, CaptureInfo& info, std::string& error);

// What a source path is.
enum class SourceKind {
    Missing,
    Video,             // .mp4 .mov .m4v .webm .mkv .avi
    Images,            // a folder of photos (or a capture folder with photos)
    VideoFolder,       // a capture folder whose capture.json / contents is one video
    NerfstudioDataset, // a folder with transforms.json
    ColmapDataset,     // a folder with images/ and sparse/0
    Unsupported,
};
SourceKind classifySource(const std::string& path);
const char* sourceKindName(SourceKind kind);
bool isVideoFile(const std::string& path);
// Photos in a folder (sorted by name; not recursive).
std::vector<std::string> listImages(const std::string& dir);
// The first video in a folder ("" if none).
std::string findVideo(const std::string& dir);

// Training presets.
struct TrainPreset {
    const char* id;    // "fast", "quality"
    const char* label; // "Fast preview"
    int steps;
    int maxResolution; // longest image side Brush loads
    int shDegree;
    int exportEvery;   // a checkpoint (live preview) every this many steps
    int maxSplats;
    int maxFrames;     // frames kept from a video
};
const std::vector<TrainPreset>& trainPresets();
const TrainPreset& trainPreset(const std::string& id); // unknown -> the first (fast)

// Brush's arguments (after the program name) to train `dataset` into `exportDir`.
std::vector<std::string> brushArguments(const std::string& dataset, const std::string& exportDir,
                                        const TrainPreset& preset, bool evaluate);

// One line of Brush's log (RUST_LOG=info on stdout). Fields stay -1 / negative when absent.
struct TrainerLog {
    int refineIter = -1; // "Refine iter 400, 51234 splats."
    int splats = -1;
    int evalIter = -1;   // "Eval iter 1000: PSNR 24.31, ssim 0.81"
    double psnr = -1.0;
    double ssim = -1.0;
    bool error = false;  // a panic or "Error:" line
};
bool parseTrainerLog(const std::string& line, TrainerLog& out);

// The step number in a checkpoint name such as "export_1200.ply" (-1 if none).
int checkpointStep(const std::string& fileName);
// The newest checkpoint (highest step) in `dir`; "" when none. `step` receives its step.
std::string latestCheckpoint(const std::string& dir, int& step);

// A rough quality word for a PSNR in dB (for the progress overlay).
const char* psnrQuality(double psnr);

} // namespace sf::capture
