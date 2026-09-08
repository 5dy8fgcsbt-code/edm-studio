#pragma once
#include "paint_document.h"
#include "renderer.h"
#include <imgui.h>

namespace edm {
// ImGui editing workspace. CPU raster jobs are separate from camera rendering.
class PaintEditor {
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    PaintEditor();
    ~PaintEditor();
    void bind(Renderer& renderer, std::shared_ptr<Livery> livery, const fs::path& textureDirectory);
    void reset(Renderer& renderer);
    void quiesce();
    void tick(Renderer& renderer, const Args& args);
    void panel(HWND window, Renderer& renderer, const Args& args);
    bool viewport(Renderer& renderer, ImVec2 origin, ImVec2 size, bool hovered);
    bool active() const;
    bool busy() const;
    bool hasEdits() const;
    PaintSnapshot snapshot() const;
    std::optional<PaintProjectDocument> takeRestoredAppearance();
    // Used before replacing a document or closing the application to preserve unfinished work.
    void recover(const fs::path& directory) const;
    Json diagnostic() const;
    Json verifyLiveryPreview(Renderer& renderer, const fs::path& directory);
    bool exercise(Renderer& renderer, const std::string& material, const fs::path& output);
    bool exerciseWrap(Renderer& renderer, const fs::path& image, const fs::path& output);
    bool exerciseProjection(Renderer& renderer, const fs::path& image, const fs::path& output);
    bool exerciseStroke(Renderer& renderer);
    bool exerciseProject(Renderer& renderer, const fs::path& directory);
    bool exerciseAutomatic(Renderer& renderer, const fs::path& image, const fs::path& directory,
                           int maximumDimension = 512);
    bool exerciseDecal(Renderer& renderer, const fs::path& image, const fs::path& directory,
                       int maximumDimension = 2048);
};
} // namespace edm
