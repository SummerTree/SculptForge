// SculptForge - open source digital sculpting
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/Commands.h"
#include "core/Face.h"
#include "core/Multires.h"

#include <cmath>
#include <cstdio>

namespace sf {

namespace {

std::string percent(float f) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f%%", double(f) * 100.0);
    return buf;
}

// The fit error as a share of the head: the template head is about 34 cm tall.
float headShare(const FaceFit& fit) { return fit.rmsError / (34.0f * fit.align.scale); }

std::string presetNames() {
    std::string s;
    for (const auto& p : facePresets()) s += (s.empty() ? "" : ", ") + std::string(p.name);
    return s;
}

} // namespace

void registerFaceCommands(CommandRegistry& registry) {
    using T = ParamSpec::Type;
    registry.add({"face_shapes_generate",
                  "Generate Facial Shapes: gives the active SubTool (a sculpted humanoid head looking down +Z, Y up) "
                  "the 52 Apple ARKit blend shapes (eyeBlinkLeft, jawOpen, mouthSmileRight, ...) as sculpt layers at "
                  "weight 0, fitted to its proportions, ready to fix by sculpting into each layer and to export as "
                  "morph targets. Layers with those names are replaced (undo brings them back). Eyes, teeth and tongue "
                  "SubTools (by name) and SubTools on the face (brows, lashes) get the shapes they need.",
                  {{"landmarks", T::String,
                    "Optional JSON of points on the sculpt guiding the fit, e.g. {\"nose_tip\":[0,1.2,0.4],"
                    "\"chin\":[0,0.9,0.3]}: eye_outer_right, eye_inner_right, eye_inner_left, eye_outer_left, "
                    "nose_tip, mouth_right, mouth_left, chin (right / left are the character's). At least 3, or none "
                    "for the automatic fit.",
                    false},
                   {"symmetry", T::String,
                    "auto (default: mirror Left / Right when the sculpt is symmetric about X = 0), on or off.", false},
                   {"subtools", T::Bool, "Also give the shapes to eyes, teeth, tongue and SubTools on the face (default true).",
                    false},
                   {"level", T::Number,
                    "Subdivision level (1-based, as SDiv) to make the shapes on; default: the lowest with at least 20,000 "
                    "vertices. Higher levels carry them up with their detail.",
                    false}},
                  [](Document& doc, const Args& a) -> CommandResult {
                      FaceShapesOptions o;
                      std::string error;
                      const std::string lm = argString(a, "landmarks");
                      if (!lm.empty()) {
                          Json j;
                          if (!Json::parse(lm, j, error) || !FaceLandmarks::fromJson(j, o.landmarks, error))
                              return {false, "Landmarks: " + error};
                      }
                      const std::string sym = argString(a, "symmetry", "auto");
                      o.symmetry = sym == "on" || sym == "true" || sym == "1" ? 1 : sym == "off" || sym == "false" || sym == "0" ? 0 : -1;
                      o.subtools = argBool(a, "subtools", true);
                      if (a.count("level")) o.level = int(argNumber(a, "level", 1.0)) - 1;
                      FaceShapesReport r;
                      if (!generateFaceShapes(doc, o, r, error)) return {false, error};
                      std::string msg = "Generated 52 facial shapes on " + doc.activeSubtoolInfo().name;
                      if (subdivLevelCount(doc.mesh)) msg += " (made at SDiv " + std::to_string(r.level + 1) + ")";
                      msg += ": fit error " + percent(headShare(r.fit)) + " of the head, " + percent(r.fit.coverage) +
                             " of the template's face on the sculpt" + (r.symmetric ? ", Left/Right mirrored" : "");
                      for (const auto& s : r.subtools) msg += "; " + s;
                      return {true, msg};
                  }});

    registry.add({"face_landmarks_detect",
                  "Finds the facial landmarks (eye and mouth corners, nose tip, chin) on the active SubTool by fitting "
                  "the template head automatically, and reports them as the JSON face_shapes_generate takes (to check "
                  "or correct before generating).",
                  {},
                  [](Document& doc, const Args&) -> CommandResult {
                      std::vector<glm::uvec3> tris;
                      for (uint32_t t = 0; t < doc.mesh.triangleCount(); ++t)
                          if (!doc.mesh.triangleHidden(t)) tris.push_back(doc.mesh.triangles[t]);
                      FaceFit fit;
                      std::string error;
                      if (!fitFaceTemplate(doc.mesh.basePositions(), tris, nullptr, fit, error)) return {false, error};
                      return {true, fit.landmarks.toJson().dump() + " (fit error " + percent(headShare(fit)) +
                                        " of the head, " + percent(fit.coverage) + " found the sculpt)"};
                  }});

    registry.add({"face_weights",
                  "Live face: sets the weights of the named blend shapes (sculpt layers) on every SubTool, e.g. "
                  "json={\"jawOpen\":0.3,\"eyeBlinkLeft\":1}. Made for 30+ calls a second (the phone's face feed): no "
                  "undo step, only the vertices those shapes move are updated. Names not given keep their weight.",
                  {{"json", T::String, "JSON object of ARKit blend shape names and weights 0-1 (a whole face message works too).",
                    true}},
                  [](Document& doc, const Args& a) -> CommandResult {
                      std::vector<std::pair<std::string, float>> w;
                      std::string error;
                      if (!parseFaceWeights(argString(a, "json"), w, error)) return {false, error};
                      const FaceWeightsStats s = applyFaceWeights(doc, w);
                      if (!w.empty() && s.matched == 0)
                          return {false, "No facial shapes to drive: run Generate Facial Shapes (face_shapes_generate) first"};
                      std::string msg = "Face weights: " + std::to_string(s.matched) + " shapes, " + std::to_string(s.layers) +
                                        " changed (" + std::to_string(s.vertices) + " vertices)";
                      if (!s.unknown.empty()) msg += ", unknown: " + s.unknown.front() + (s.unknown.size() > 1 ? ", ..." : "");
                      return {true, msg};
                  }});

    registry.add({"face_preset",
                  "Face test: shows an expression made of the ARKit shapes (sets all 52 weights; no undo step). "
                  "Presets: " + presetNames() + ".",
                  {{"name", T::String, "The preset, e.g. Smile.", true},
                   {"amount", T::Number, "How strongly, 0-1 (default 1).", false}},
                  [](Document& doc, const Args& a) -> CommandResult {
                      const FacePreset* p = findFacePreset(argString(a, "name"));
                      if (!p) return {false, "Unknown face preset '" + argString(a, "name") + "' (" + presetNames() + ")"};
                      const float amount = float(argNumber(a, "amount", 1.0));
                      std::vector<std::pair<std::string, float>> w;
                      for (const auto& n : arkitBlendShapeNames()) {
                          float v = 0.0f;
                          for (const auto& [pn, pv] : p->weights)
                              if (n == pn) v = pv * amount;
                          w.push_back({n, v});
                      }
                      const FaceWeightsStats s = applyFaceWeights(doc, w);
                      if (s.matched == 0) return {false, "No facial shapes: run Generate Facial Shapes first"};
                      return {true, std::string("Face: ") + p->name};
                  }});

    registry.add({"face_take_bake",
                  "Bakes a recorded face take (JSON frames from the phone link: {\"type\":\"face\",\"t\":ms,\"weights\":"
                  "{...}}) into keyframed blend-shape weights. GLB export writes them as a glTF animation and USD export "
                  "as time samples; the take is saved with the project.",
                  {{"path", T::String, "The take file (.json array, {\"frames\":[...]} or JSON Lines).", true},
                   {"fps", T::Number, "Keys per second (default 30).", false}},
                  [](Document& doc, const Args& a) -> CommandResult {
                      MorphAnimation take;
                      std::string error;
                      if (!loadFaceTake(argString(a, "path"), take, error)) return {false, error};
                      const float fps = float(std::clamp(argNumber(a, "fps", 30.0), 1.0, 240.0));
                      MorphAnimation baked = resampleMorphAnimation(take, fps);
                      setBakedFaceAnimation(doc, &baked);
                      char buf[96];
                      std::snprintf(buf, sizeof buf, "%.2f s, %zu keys at %g fps", double(baked.duration()), baked.times.size(),
                                    double(fps));
                      return {true, "Baked take '" + baked.name + "' (" + buf + "): exports with GLB and USD"};
                  }});

    registry.add({"face_take_clear", "Removes the baked face take (exports go back to a still pose).", {},
                  [](Document& doc, const Args&) -> CommandResult {
                      MorphAnimation a;
                      if (!bakedFaceAnimation(doc, a)) return {true, "No baked take"};
                      setBakedFaceAnimation(doc, nullptr);
                      return {true, "Removed the baked take '" + a.name + "'"};
                  }});
}

} // namespace sf
