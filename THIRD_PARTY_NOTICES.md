# Third-party notices

EDM Studio's original code, documentation, application icon and synthetic samples are licensed under the root LICENSE (MIT, Copyright (c) 2026 EDM Studio contributors). Third-party and adapted code retains its own copyright and license; the root MIT license does not replace those terms.

The native application statically uses the following components. See release-info.json for the version and source commit of a binary distribution.

| Component | Version | License | Source |
|---|---|---|---|
| Dear ImGui | 1.92.5 | MIT | https://github.com/ocornut/imgui |
| stb components embedded in Dear ImGui | imstb_rectpack.h, imstb_textedit.h, imstb_truetype.h from ImGui 1.92.5 | MIT alternative, Copyright (c) 2017 Sean Barrett | https://github.com/ocornut/imgui |
| ProggyClean embedded default font | Copy embedded in ImGui 1.92.5 | MIT, Copyright (c) 2004, 2005 Tristan Grimmer | https://github.com/bluescan/proggyfonts |
| Eigen | 3.4.0 | MPL-2.0 (other included files have their stated licenses) | https://gitlab.com/libeigen/eigen |
| nlohmann/json | 3.12.0 | MIT | https://github.com/nlohmann/json |
| Lua | 5.4.8 | MIT | https://www.lua.org/ |
| miniz | 3.1.0 | MIT | https://github.com/richgel999/miniz |
| DirectXTex | oct2025 | MIT | https://github.com/microsoft/DirectXTex |

Dependency sources and original license notices are included under native/vendor in the project source distribution. Binary distributions include their license texts in licenses/imgui, licenses/stb, licenses/proggyclean, licenses/eigen, licenses/json, licenses/lua, licenses/miniz and licenses/directxtex. The stb notice covers all three headers listed above, using the MIT alternative of their dual license. No vendor code is modified; Lua is compiled in its supported C++ exception mode.

The complete, editable Eigen 3.4.0 source used to build this program is provided offline with both binary and source distributions in third_party_sources/eigen-3.4.0/. Its MPL-covered files are available under the Mozilla Public License 2.0, included as third_party_sources/eigen-3.4.0/COPYING.MPL2. All original file headers and all seven COPYING.* files are preserved. See third_party_sources/README.md for provenance and third_party_sources/eigen-3.4.0.sha256.json for per-file integrity hashes. This is an unmodified copy of the actual build dependency, verified during packaging. The complete upstream tree also contains separately licensed auxiliary code, tests and examples (including BSD, Apache, GPL and LGPL); their individual terms remain applicable. Including this source tree does not mean all its components are linked into the program. The native build uses Eigen/Dense, Eigen/Geometry and Eigen/SVD with EIGEN_MPL2_ONLY enabled.

Windows Direct3D 11, DXGI, WIC, D3DCompiler and Win32 are operating-system components. The GUI loads the installed Microsoft YaHei font; Microsoft YaHei is not redistributed. Dear ImGui also embeds ProggyClean, whose full MIT notice is included in licenses/proggyclean/LICENSE.txt. That notice is from https://github.com/bluescan/proggyfonts/blob/master/LICENSE and matches the copyright identified beside the font data in imgui_draw.cpp. The release contains no Python, Qt, LLDB or DCS runtime DLL.

The previous binary EDM reader was adapted from DCS-EDM-Blender-Importer by devozdemirhasancan and contributors (MIT): https://github.com/devozdemirhasancan/DCS-EDM-Blender-Importer . The copyright and full license remain in edm_studio/edm/LICENSE and the release licenses/edm-parser/LICENSE. The native sequential reader preserves that attribution. The animation graph, GUI, PBR preview, export pipeline and tests are separate implementations.

Format research consulted https://ndevenish.github.io/Blender_ioEDM/EDM_Specification.html and the public Blender_ioEDM / BlenderEdmExporter repositories. Research checkouts are not distributed.

The preserved legacy Python reference uses Python (PSF), NumPy (BSD), PySide6/Shiboken6/Qt (LGPLv3/GPLv3/commercial), PyOpenGL (BSD), Pillow (MIT-CMU), and Lupa/Lua (MIT). Existing license texts remain in licenses/. These are development/legacy dependencies, not part of the native runtime.

No DCS game models, textures, executables or DLLs are redistributed. samples/animation_demo.edm and native/tests/fixtures are original synthetic test data. This project is not affiliated with Eagle Dynamics.
