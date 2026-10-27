// SculptForge - open source digital sculpting
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Help > User Guide (Shift+F1): the guide in docs/guide, embedded in the app
// and shown in a floating window with the contents on the left, the chapter
// on the right, full-text search, back / forward and links between chapters.
// The Markdown parsing, contents and search live in core/Guide.
#pragma once

#include "core/Guide.h"

#include <imgui.h>

#include <map>
#include <string>
#include <vector>

namespace sf {

class App;

class UserGuide {
public:
    // Heading fonts (Inter SemiBold), added while the font atlas is built.
    void addFonts(float scale);
    // The reading fonts (Inter 17 px, regular and SemiBold), also used by the update banner; null before addFonts.
    static constexpr float kReadingSize = 17.0f;
    ImFont* bodyFont() const { return body_; }
    ImFont* bodyBoldFont() const { return bodyBold_; }
    // Opens the window at TARGET ("masking.md#mask-by-cavity", "masking.md", "" = where it was).
    void open(const std::string& target = "");
    void toggle();
    bool isOpen() const { return open_; }
    void draw(App& app);

    // The chapter that explains a palette path from the Ctrl-help footer ("Tool > Masking" -> "masking.md"),
    // or "" when there is none.
    static std::string chapterFor(const std::string& palette, const std::string& actionId);
    // Title of a chapter file ("masking.md" -> "Masking"), "" if unknown. Loads the guide on first use.
    std::string chapterTitle(const std::string& file);

    // UI tests.
    std::string currentFile() const; // "index.md" or the chapter's file
    size_t searchHits() const { return hits_.size(); }

private:
    struct Place {
        int chapter = -1; // -1: the contents page (index.md)
        float scroll = 0.0f;
    };
    struct Texture {
        unsigned tex = 0;
        int width = 0, height = 0;
        bool failed = false;
    };

    bool ensureLoaded();
    void navigate(const std::string& target, bool record = true);
    void go(Place place, const std::string& anchor, bool record);
    const guide::Document& page() const;
    void drawToolbar(App& app);
    void drawContents(App& app);
    void drawSearchResults(App& app);
    void drawPage(App& app);
    float drawBlock(App& app, const guide::Block& block, int index, ImVec2 origin, float width, bool draw);
    const Texture* texture(const std::string& path);
    std::string websiteUrl() const;

    bool open_ = false, loaded_ = false, focusSearch_ = false;
    std::string loadError_;
    guide::Book book_;
    Place place_;
    std::vector<Place> back_, forward_;
    std::string pendingAnchor_; // scrolled to once the page is laid out
    int pendingBlock_ = -1;     // a search hit's block, likewise
    bool pendingTop_ = false;
    float pendingScroll_ = -1.0f;
    float scroll_ = 0.0f;       // the page's scroll position last frame (for history)
    char query_[128] = "";
    std::string lastQuery_;
    std::vector<guide::SearchHit> hits_;
    std::vector<std::string> highlight_; // query words marked on the page opened from a search
    std::map<std::string, float> anchorY_; // this page's heading positions
    std::vector<float> blockY_, blockH_;   // block positions / heights for the current page and width
    float layoutWidth_ = 0.0f;
    int layoutPage_ = -2;
    std::map<std::string, Texture> textures_;
    ImFont* h1_ = nullptr, *h2_ = nullptr, *h3_ = nullptr;
    ImFont* body_ = nullptr, *bodyBold_ = nullptr;
    int linkCounter_ = 0;
};

} // namespace sf
