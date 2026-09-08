#include "export.h"
#include <iostream>
using namespace edm;
void runRegression();
int wmain(int argc, wchar_t** argv) {
    if (argc >= 4 && std::wstring_view(argv[1]) == L"--lua-worker")
        return luaWorker(argv[2], argv[3]);
    try {
        ComRuntime imageRuntime;
        Track track;
        track.visibility = true;
        track.ranges = {{.2, .6}, {.8, 1.1}};
        require(track.sample(.2)[0] == 1 && track.sample(.6)[0] == 0 && track.sample(.8)[0] == 1,
                "Visibility endpoints");
        Track rotationTrack;
        rotationTrack.channel = Channel::Rotation;
        rotationTrack.keys = {{0, V4(0, 0, 0, 1)}, {1, V4(0, 1, 0, 0)}};
        require((rotationTrack.sample(.5) - V4(0, std::sqrt(.5), 0, std::sqrt(.5))).norm() < 1e-12,
                "Quaternion interpolation");
        auto lua = evaluateLua("livery={} for i=1,3 do livery[i]={'M'..i,DIFFUSE,'texture'..i,false} end "
                               "name='Native test';custom_args={[38]=EDM_STUDIO.gear}",
                               {}, "", {{"gear", .75}});
        require(lua["environment"]["livery"].size() == 3 && lua["environment"]["custom_args"]["38"] == .75,
                "Dynamic Lua context");
        bool denied = false;
        try {
            evaluateLua("livery={};name=io.open('test')");
        } catch (...) {
            denied = true;
        }
        require(denied, "Lua filesystem isolation");
        runRegression();
        std::cout << "Native math, visibility, Lua sandbox tests passed\n";
        return 0;
    } catch (...) {
        std::cerr << exceptionText() << "\n";
        return 1;
    }
}
