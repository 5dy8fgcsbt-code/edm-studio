# EDM Studio Native 0.13.0

[简体中文](README.md) | **English**

A native Windows desktop application built with C++20, Dear ImGui and Direct3D 11. Open DCS EDM v8/v10 files, preview parameter animations, liveries, RoughMet textures and dynamic aircraft numbers, then export to GLB, glTF 2.0, OBJ or FBX. Running the application does not require Python, Qt, DCS, Blender or an internet connection. Supply your own EDM files and any textures they reference.

## English interface

Version 0.13.0 adds an English language pack. Select **English** in the language menu at the top right, save your paint project, then restart the application. Your language choice is saved automatically. The interface, painting tools, animation analysis, file dialogs and export messages are translated. Model nodes, material names, livery names, user-created layer names, paths and Lua configuration keys remain unchanged.

The editable pack is `native/locales/en-US.json` in the source tree, or `locales/en-US.json` beside the EXE in the portable package. The same translations are embedded in the executable: English still works if the external file is missing. Invalid or incompatible packs produce a warning and fall back to the embedded translations. Restart after editing a pack.

For development, run `python tools/localization_catalog.py --generate` to update the embedded catalog, or `--check` to check source coverage, format placeholders and generated-file consistency. Preserve translation keys, `%` format placeholders, `\u0000` option separators and control IDs after `##`. Python is a development tool only, not a native application or build dependency.

## Getting started

Open `EDM-Studio.exe` and choose or drop an EDM file. Use the left panel to search for animation parameters or node names, move sliders, enter exact values, play a parameter or reset it. On the right, choose a detected livery or import a `description.lua` file or ZIP archive.

Livery discovery includes redirected Windows Saved Games folders, DCS installations, Steam libraries, user mods and additional folders you specify. Texture references can cross sibling livery folders and archives: for example, `../B/wing` in `A.zip` can resolve `wing.dds` in `B.zip` or a sibling `B` directory. This applies to diffuse and RoughMet textures during preview, painting and export.

- **View controls:** left/right-drag to orbit, middle-drag to pan, mouse wheel to zoom. Press **F** to fit the model and **F11** to focus the viewport. While painting, use right-drag to orbit.
- **Shortcuts:** **Ctrl+O** opens a model, **Ctrl+E** exports, and **Space** plays the selected parameter when painting controls are inactive.
- **Show edited livery:** turn this off below the viewport to compare with the original textures. Edits and undo history are retained; saving and exporting still include your work. Newer paint projects also retain the original diffuse textures for comparison after reopening.
- **Livery settings:** set the aircraft number, provide JSON variables for dynamic Lua, and manage additional search folders.
- **Texture quality:** preview limits are 1024, 2048, 4096 or 8192 pixels. The preview memory budget defaults to **2 GB**. Enter **0.25–100 GB** under **Livery → Texture quality**, then press Enter or **Apply texture budget**. The setting is saved and preview mip levels are selected again.
- Preview limits do not restrict export resolution. Unedited textures retain their source resolution; painted textures use their actual canvas resolution.
- Settings are stored in `edm-studio.settings.json` beside the EXE. The original sample model is `samples/animation_demo.edm`.

## Attachments and collision models

Enable **Mount points** below the viewport, then click a marker to load another EDM. Its `AttachPoint` is aligned with the selected connection point. Rack points such as `Point01` work like aircraft `Pylon` points, allowing chains such as aircraft → rack → weapon. Blue markers are available Pylon/Point connectors; orange markers have an attachment. Hover to see the name. Markers remain visible through the model.

Click an occupied orange marker to show a **Detach** button beside it. Only pressing that button removes the attachment and its descendants. Other objects keep their paint, undo history and animation pose. If the detached object has paint edits, a recovery copy is saved first.

**Attachments** controls attachment visibility. Hidden objects do not participate in brush picking, decal occlusion or camera-projection occlusion. Their edits and undo history remain available. You can paint the aircraft and its attachments together. Model exports include all connected attachments, even when hidden in the preview. With texture export enabled, the exporter also saves a combined `description.lua` and DDS livery dependencies, except in mesh-only mode.

Standalone collision EDMs are supported. When a file has no visual triangle meshes, its `ShellNode` geometry is shown automatically with its parent hierarchy, visibility and parameter animations. Distinct colors identify shells; they do not indicate damage levels or material properties. Collision shells are displayed double-sided and have no generated livery UVs. They can be exported to all four supported model formats. Files with visual meshes do not also display their internal collision shells.

Detailed guides: [Attachments](docs/ATTACHMENTS.md) and [Collision models](docs/COLLISION_MODELS.md) — currently in Chinese.

## Exporting models and animation

Choose a format in **Export**. Enable **Mesh only (current pose)** to freeze the aircraft and all attachments at their current settings, including gear, control surfaces and canopy positions. Rigid transforms and skin deformation are baked into vertices. The result retains meshes and materials, plus textures if selected, and removes skeletons, animation, attachment points, empty helper nodes and DCS-specific metadata. It does not create Lua or JSON report sidecars. The default filename ends in `_model`.

Disable mesh-only export to retain the original hierarchy and parameter animation. Choose all parameter animations or only the current parameter, and set the clip duration. Texture export is a separate option.

| Format | Behavior |
| --- | --- |
| GLB | Single-file glTF 2.0 model with embedded assets. Supports animation or mesh-only export. |
| glTF | Embedded glTF model. Supports animation or mesh-only export. |
| OBJ | Static current pose baked into vertices, with MTL and texture assets. No bones or animation. Keep the MTL and texture folder with the OBJ. |
| FBX | Supports hierarchy, skinning and parameter animation, or a baked mesh-only export. Color and opacity textures are embedded; RoughMet may require manual material connections after import. |

GLB, glTF and FBX convert each EDM parameter range into a separate animation clip, lasting three seconds by default. Negative parameter ranges, hierarchy, oriented scaling, rotation, up to eight skin weights and inverse bind matrices are retained. FBX adaptively samples quaternion rotations into continuous Euler curves. Each clip holds other parameters at their current export-time values, including livery `custom_args` and aircraft numbers. OBJ always exports a static pose.

Since 0.12.4, the highest-detail internal LOD branch is selected from the EDM distance ranges. Independent and nested LOD groups, including attachments, are handled separately. Preview, paint picking, decal occlusion, part analysis and all four exporters use the same selection. Original mesh indices, materials and UVs are retained for compatibility with paint projects; inactive lower-detail meshes are not uploaded to the GPU. Reopen the EDM to apply this behavior; no setting is required.

See [Export formats](docs/EXPORT_FORMATS.md) for format limitations and the normal UV workflow in Substance 3D Painter. This detailed guide is currently in Chinese.

## Materials, numbering and limitations

RoughMet channels are **R = ambient occlusion**, **G = roughness**, and **B = metalness**. NumberNode atlases are converted to standard STEP visibility animations. DCS blend modes distinguish opaque, alpha-tested and transparent materials. Opaque materials do not incorrectly use the transparency field; original texture alpha and paint project data are retained. FBX adds RGB textures for opaque materials to accommodate importers that automatically connect image alpha.

Dynamic livery configuration runs in an isolated subprocess with embedded Lua 5.4.8. Functions, loops, conditions and directory-scoped `dofile`/`require` are supported. Memory, instruction and five-second time limits prevent faulty configurations from blocking the interface.

DCS-specific lighting, normal/effect shaders, environment reflections, damage effects and other material parameter animations cannot all be mapped to standard PBR materials. Unsupported EDM layouts produce explicit errors. Shared or overlapping UVs remain shared: automatic texture targeting does not change the model topology.

Format research and older behavior are documented in [EDM parsing and animation export analysis](docs/DCS_EDM_读取与动画导出分析.md) and the [0.3.4 reference](docs/LEGACY_0.3.4.md), both currently in Chinese.

## Animated part identification

After loading, the application samples each parameter's actual geometry motion in the background. It shows candidate part names, a heuristic score, spatial location, maximum displacement and supporting evidence. Search by part name, focus the affected region or enable **Highlight motion**.

Scores are not statistical accuracy estimates. Uncertain results remain unknown or retain multiple candidates; the system does not assign names solely from fixed parameter numbers. Sampling includes keyframes, intermediate poses and full skinning, with a default budget of about 12,000 vertex samples. Analysis may temporarily reveal parts hidden by other parameters without changing the actual preview or export pose.

Earlier validation covered 26 synthetic checks. F-14 analysis covered 221 parameters in about 1.79 seconds with ten key-part assertions; F-100 covered 90 parameters in about 2.43 seconds with eleven assertions. F-14 canopy highlighting and two full-vertex GPU readbacks were also checked. These are recorded measurements, not performance guarantees; orbiting the camera does not itself upload geometry or evaluate animation again.

## Paint layers, templates and brushes

The **Paint** tab offers **Browse**, **Brush**, **Decal** and **Projection** without requiring material selection or manual canvas creation. Painting automatically targets the diffuse texture beneath the pointer and can continue across material and UV seams, including attached objects.

In **Paint layers**, create, rename, duplicate, reorder, hide, change opacity, clear or delete layers. Top layers cover lower ones. Brushes, decals, camera projection and flat templates write into the current layer; layer edits can be undone. Version 3 paint projects preserve complete layer pixels. DCS liveries and model exports use the visible-layer composite. Layers use sparse tiles and share copied pixels until changed; camera movement does not recomposite them.

Select **Brush** and hold the left mouse button on the model. Brush samples are queued while textures load. The entire stroke can be undone or redone in one step. Adjust world-space radius, hardness and opacity, and choose whether to preserve the base alpha mask.

New canvases preserve the source image size by default, up to **8192 pixels**. Optional 4096 and 2048 limits reduce memory use. Changing this setting does not resample existing canvases. Detail lost in an earlier low-resolution canvas cannot be recovered by increasing the limit; repaint from the original livery.

Load a flat PNG, DDS, TGA or JPG template onto the current layer. The next stroke selects the destination texture and replaces that layer's contents for the texture; the original base and other layers are retained, and the operation can be undone. Export layered PSD files as flat PNG first. For local artwork, use **Decal** or **Projection** instead.

**Save project & DCS livery** creates a new subfolder containing the PNG project images, mipmapped DDS files, dependency textures and `description.lua`. Place it under the appropriate aircraft's Liveries directory or reopen it in EDM Studio. Texture-enabled GLB, glTF, OBJ and FBX exports include painted results. Unsaved edits are written to a recovery project under `PaintRecovery` beside the EXE before switching models or closing. If recovery saving fails, the window remains open. Use **Open paint project** to restore it.

Painting excludes dynamic number atlases, which are controlled by numbering. Material aliases with the same name and diffuse resource share a canvas. Shared UVs affect multiple surfaces together. If different painted images cannot be addressed separately by DCS material names, saving the complete livery reports the conflict. Only dirty texture regions are updated in the viewport; camera motion does not rebuild picking structures or vertices. Occlusion is geometric and does not account for holes in transparent textures.

Cross-texture strokes, template targeting, multi-canvas undo and project reopening have been checked with real GPU readback. Applying a template marks the project as modified and creates undo/recovery data even if the brush itself changes no pixels. See [Paint layers](docs/PAINT_LAYERS.md), currently in Chinese.

## Surface decals / image brush

Under **Decal**, load an image, preferably a transparent PNG. Move the pointer over the model to preview it and left-click once to stamp. The default **Surface conform** mode extends across connected triangles, including matching mesh and material seams. **Planar projection** provides the original local orthographic projection mode. Neither requires selecting a material first.

Set width in meters, lock the image aspect ratio, rotate it or adjust opacity. Use **[ / ]** to resize and **Q / E** to rotate. Right-drag, middle-drag and the mouse wheel remain available for camera movement.

Surface conform uses the same surface coordinates for preview and painting. Facing and occlusion use the camera at placement time; orbiting afterward does not change the placed decal. Adjust the allowed face angle under **Surface fit & occlusion**. In planar mode, adjust projection depth there instead.

- **Bridge small gaps** is off by default, preserving strict continuous-surface behavior. Enable it to cross gaps of **0.1–20 mm**, with a default allowance of **2 mm**. It preserves the actual missing strip instead of filling geometry and supports differently subdivided boundaries.
- Since 0.12.1, gap bridging also handles slight intersections where one skin edge contacts the interior of another triangle, continuing over the exposed surface.
- **Follow sharp edges** permits bends up to **135°**, useful for spine corners and wing lips. Turning it off restores **65°**; detailed settings allow **10–150°**.
- Strong curvature, large steps, gaps beyond the allowed width and ambiguous connections still stop extension. Use smaller stamps when needed. Original geometry, UVs, animation and export structure remain unchanged.

Each stamp is one cross-texture undo operation. Cancellation or failure rolls back the entire operation. Preview alone does not edit textures, and holding the mouse does not repeatedly stamp. The panel shows canvas dimensions and estimated pixel coverage near the anchor. The floating preview samples the source image directly; baked detail is limited by the destination texture resolution.

See [Surface-conforming decals](docs/SURFACE_CONFORMING_DECALS.md), currently in Chinese. A known F/A-18 test location has a preview/bake difference that also reproduces in 0.12.4; it is documented in the validation records and was not fixed by the English language update.

## Camera image projection

Select **Projection**, load an image and drag it in the viewport to position it. Adjust size, rotation, opacity and aspect ratio. **Project from current view** captures the current camera and pose, finds every covered diffuse texture and writes into the corresponding UVs. No per-material selection is needed.

Geometric occlusion, backface filtering, cross-texture undo and cancellation rollback are supported. The placement overlay can be hidden independently to inspect the baked result. Its preview opacity does not affect the pixels written to textures.

Coverage tests include near-plane crossings, long triangles crossing image edges, shared UVs and transparent images blended only once. A single time, pixel and ray budget covers the entire operation; failures roll back all affected textures. Automatic multi-texture projection on F-14 and F-100 has been checked with GPU readback. Large projections can exceed the time budget: 300 seconds at 2K or above, and 120 seconds at lower resolutions. Reduce coverage or new-canvas resolution if this happens.

Existing projects restore the base livery, RoughMet, appearance parameters and paint pixels. Image baking runs in the background, but one rasterization step of an extremely large brush cannot be interrupted midway. See the [Painting guide](docs/绘制使用指南.md), currently in Chinese.

## Building and testing

Install Visual Studio with the Desktop development with C++ workload and Windows SDK, and use PowerShell 7. Dependency sources are included in `native/vendor`, so the native project can be built offline.

The root `./build.ps1` builds the native application, runs all native tests and packages it. To run the steps separately:

```powershell
./native/build.ps1
./native/test.ps1 -SkipBuild
# Commit source changes before producing a source archive.
./native/package.ps1 -SkipBuild -Source
./build/native/Release/EDM-Studio-Native.exe --language en-US
```

The CLI provides `inspect`, `export`, `analyze`, `catalog` and `livery` commands. Examples:

```powershell
./build/native/Release/edm-native-cli.exe inspect 'path/to/model.edm' --language en-US
./build/native/Release/edm-native-cli.exe export 'path/to/model.edm' --output 'model.glb' --args 0,9,10,38 --language en-US
./build/native/Release/edm-native-cli.exe export 'path/to/model.edm' --output 'model.fbx' --args 0,9,10,38 --language en-US
./build/native/Release/edm-native-cli.exe export 'path/to/model.edm' --output 'model.obj' --baseline 0=1,38=1 --language en-US
./build/native/Release/edm-native-cli.exe export 'path/to/model.edm' --output 'model_only.fbx' --model-only --language en-US
```

Options include `--livery`, `--context`, `--baseline`, `--bort`, `--static`, `--model-only`, `--duration`, `--textures` and `--no-textures`. `--static` omits animation clips; `--model-only` also bakes the pose and removes rig/helper structure. The CLI does not read the GUI's saved language; use `--language en-US` after the model path, or `--language zh-CN` for Chinese.

`native/fetch-deps.ps1` can retrieve the pinned dependency versions again. The older Python implementation remains for development comparisons; the native application and native build do not invoke it. Packaging creates a versioned portable folder and Windows/source ZIP files. The source ZIP reflects local Git HEAD, so commit source changes before creating it.

### Validation highlights

| Version | Recorded checks |
| --- | --- |
| 0.13.0 | All 31 CTest groups passed; 1,468 localization checks; 217 F/A-18 layer checks; F-15E English surface-decal workflow passed. English startup verified both with the portable pack and with only the EXE copied. Language settings survived restart. |
| 0.12.4 | All 30 groups passed, including 53 LOD checks. Highest-detail selection is shared by preview, painting and export. |
| 0.12.3 | All 29 groups passed; 965 mesh-only checks and 642 OBJ checks. FA-18C imports in all four formats contained only mesh objects in Blender 5.2.1. Textured FBX had 445 meshes, 31 images, no armature and no animation; surface comparison error was about `6.29e-7 m`. |
| 0.12.2 | All 28 groups passed, including 38 static attachment checks. FA-18C current-pose exports had no animation; independent FBX readback checked 11,639 vertex samples with maximum position error about `7.42e-7 m`. |
| 0.12.1 | All 28 groups passed, including 148 surface-contact checks. FA-18C 4K platform/sidewall decals, preview/bake comparisons, undo/redo, GPU readback and project reopening were exercised; the original F-100 mode also passed. |
| 0.11.0 | All 25 groups passed, including 113 surface-conform checks. F-100 and F-15E curved decals, undo/redo, GPU readback and reopening were validated alongside the planar mode. |
| 0.10.0 | 24 groups and 35,176 counted checks passed. F-100 layered painting, full-stack reopening, templates and 2K decals were checked, as was detaching one painted weapon from a two-weapon LAU-88 assembly. |
| 0.9.2 | 22 groups and 34,962 checks passed, including collision geometry, attachment hierarchy, GLB/FBX readback, paint occlusion, combined livery dependencies and project assembly restoration. |
| 0.8.0 | 13 groups and 32,695 checks passed. F-100 OBJ and F-100/F-14 FBX were read back in Blender 4.2.20. Independent ufbx checks covered all 221 F-14 clips, 1,106 poses and about 13.82 million vertex samples, with maximum error about `2.08e-6 m`. |

Additional recorded checks cover original-texture preservation, cross-ZIP assets, decals, cross-canvas transactions, UV rasterization, queued brush strokes, 1/2/100 GB preview budgets and settings persistence. A painted two-animation GLB passed Khronos validation with zero errors and warnings. Camera movement does not rebuild projection depth or model geometry. Historical counts and measurements refer to those particular builds and assets.

Full results and known issues are in [Native validation](docs/NATIVE_VALIDATION.md) and the [Development report](docs/UNATTENDED_REPORT.md), currently in Chinese. Diagnostic DCS assets are not included in the repository.

## License and bundled sources

Original project code, documentation, icons and original test fixtures use the [MIT License](LICENSE). Commercial use, modification, resale and closed-source use are permitted subject to retaining the copyright and license notice. Third-party and adapted code retain their respective licenses; see [Third-party notices](THIRD_PARTY_NOTICES.md). The application does not include DCS game models, textures or DLLs.

[third_party_sources](third_party_sources/README.md) contains the complete Eigen 3.4.0 sources used by this project, original license files and SHA-256 manifests. The portable Windows package includes this directory for offline access. Version 0.7.3 restored 167 Eigen files omitted by upstream Git ignore rules and added separate license notices for ProggyClean and the stb code embedded in ImGui. Packaging checks the Eigen source bytes in the workspace and both ZIP archives.

The original aircraft icon is embedded in the GUI and CLI executables, including the window and taskbar icon; no external ICO is required at runtime. It contains ten sizes from 16 to 256 pixels. The vector source is `native/assets/edm-studio.svg`; after editing it, run `./native/generate-icon.ps1` on Windows to regenerate the ICO. Normal builds use the committed ICO and require no extra image tools.
