# Native C++ migration

The requested deliverable is a complete C++ desktop application using Dear ImGui,
with no Python or Qt runtime. Preserve the existing 0.3.4 features and validated
EDM behavior. The Python implementation remains a reference for cross-validation.

## Implementation and acceptance

- [x] C++20 / Win32 / Direct3D 11 / Dear ImGui build and packaged executable.
- [x] Memory-mapped sequential EDM v8/v10 parser, all currently supported nodes,
      strict bounds/references/version checks, C-130 and F-100D extensions.
- [x] Double-precision animation graph, affine/SVD factors, visibility gates,
      8-weight skins, corrected mirrored surfaces, numbered atlas controls.
- [x] GPU skinning and static vertex/index buffers. Camera-only frames must
      perform zero geometry uploads and zero animation graph evaluations.
- [x] Background cancellable model/texture jobs, DDS BC/mipmap uploads, bounded
      upload work, resource deduplication and configurable texture quality.
- [x] Saved Games / DCS / Steam / registry / extra-directory livery discovery,
      ZIP and description.lua, common/local textures and explicit missing paths.
- [x] Native Lua 5.4 evaluation with restricted includes, memory/instruction/time
      limits in a disposable worker process, editable JSON context.
- [x] RoughMet preview/export, NumberNode/bort preview and STEP atlas export.
- [x] GLB and embedded glTF, all/current/static modes, duration, defaults from
      custom_args and selected bort, export progress and reports, CLI.
- [x] Polished Chinese dark UI, consistent spacing/fonts, responsive panels,
      file drop/dialogs, parameter search/play/reset, texture/wire/PBR switches.
- [x] Real F-14, F/A-18, C-130, F-100D verification, GLB validator and independent
      Python-reference comparisons, meaningful native regression tests.
- [x] Loading/frame-time measurements and screenshots from the real native app.
- [x] Source/build instructions, licenses, and local Git baseline commit.
- [x] Motion-based part analysis, evidence, search, focus and affected-mesh highlight.
- [x] Editable livery templates, 3D brush, bounded cross-frame stroke sampling and GPU dirty updates.
- [x] Cylindrical image wrapping and current-camera image projection with occlusion and undo.
- [x] Self-contained DCS livery/project export, complete appearance recovery and edited GLB textures.
- [x] 855 native checks, real F14/F100 editing/GPU validation, portable executable and Lua worker tests.

## Performance design

Heavy work runs off the UI thread. ZIP central directories and decoded resources
are cached; compressed DDS mip chains stay compressed for GPU preview. The UI
shows geometry before textures complete. Shader skinning references a palette;
orbiting updates camera constants only. Animation updates node matrices/palettes,
not the vertex buffers. Texture generation IDs prevent stale livery jobs from
replacing current resources. GPU uploads are serviced within a per-frame budget.

## Reference environment

MSVC and bundled CMake/Ninja:
`D:/Program Files/Microsoft Visual Studio/18/Community`.
Legacy baseline commit: `9fc17b1`. Test models are local DCS files and are not
redistributed. Existing Python validation tools may be used as development
oracles only; the shipped native executable must not launch Python or Qt.

