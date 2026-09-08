#include "export.h"
#include "animation_analysis.h"
#include <iostream>
#include <sstream>
using namespace edm;
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    try {
        ComRuntime imageRuntime;
        if (argc >= 4 && std::wstring_view(argv[1]) == L"--lua-worker")
            return luaWorker(argv[2], argv[3]);
        require(argc >= 3,
                "Usage: edm-native-cli inspect|export|analyze|catalog|livery model.edm [--output path] "
                "[--args 0,3,9] [--baseline 0=1,3=0] [--bort 123] [--livery path] [--context "
                "JSON] [--textures dir] [--extra dir] [--no-textures] [--static] [--duration 3] "
                "[--dump-world path]");
        std::string command = utf8(argv[1]);
        fs::path source = argv[2], output, dump, liveryPath;
        std::string bort;
        Json context = Json::object();
        ExportOptions options;
        std::vector<fs::path> extras;
        for (int i = 3; i < argc; i++) {
            std::string key = utf8(argv[i]);
            auto value = [&]() {
                require(i + 1 < argc, "Missing value for " + key);
                return utf8(argv[++i]);
            };
            if (key == "--output")
                output = wide(value());
            else if (key == "--dump-world")
                dump = wide(value());
            else if (key == "--livery")
                liveryPath = wide(value());
            else if (key == "--bort")
                bort = value();
            else if (key == "--context")
                context = Json::parse(value());
            else if (key == "--textures")
                options.textureDirectory = wide(value());
            else if (key == "--extra")
                extras.emplace_back(wide(value()));
            else if (key == "--no-textures")
                options.textures = false;
            else if (key == "--duration")
                options.duration = std::stod(value());
            else if (key == "--static")
                options.arguments = std::vector<int>{};
            else if (key == "--args") {
                options.arguments = std::vector<int>{};
                std::istringstream ss(value());
                std::string item;
                while (std::getline(ss, item, ','))
                    options.arguments->push_back(std::stoi(item));
            } else if (key == "--baseline") {
                std::istringstream ss(value());
                std::string item;
                while (std::getline(ss, item, ',')) {
                    auto p = item.find('=');
                    require(p != std::string::npos, "Expected argument=value");
                    options.baseline[std::stoi(item.substr(0, p))] = std::stod(item.substr(p + 1));
                }
            } else
                throw std::runtime_error("Unknown option " + key);
        }
        auto progress = [](const std::string& text) { std::cerr << text << "\n"; };
        Json result;
        if (command == "livery")
            result = readLivery(source, "", "手动选择", "", context).metadata();
        else if (command == "catalog") {
            auto catalog = discoverLiveries(source, extras, progress);
            result = {{"liveries", Json::array()},
                      {"roots", Json::array()},
                      {"installations", Json::array()},
                      {"warnings", catalog.warnings}};
            for (auto& l : catalog.liveries)
                result["liveries"].push_back(l.metadata());
            for (auto& p : catalog.roots)
                result["roots"].push_back(pathString(p));
            for (auto& p : catalog.installs)
                result["installations"].push_back(pathString(p));
        } else {
            require(command == "inspect" || command == "export" || command == "analyze", "Unknown command");
            auto scene = Scene::load(source, progress);
            if (!liveryPath.empty())
                options.livery =
                    std::make_shared<Livery>(readLivery(liveryPath, "", "手动选择", "", context));
            if (!bort.empty())
                for (auto [a, v] : bortArguments(*scene, bort))
                    options.baseline[a] = v;
            if (command == "export") {
                require(!output.empty(), "--output is required for export");
                result = exportScene(*scene, output, options, progress);
            } else if (command == "analyze") {
                Args defaults = options.livery ? options.livery->args : Args{};
                for (auto [argument, value] : options.baseline)
                    defaults[argument] = value;
                result = analyzeAnimations(*scene, defaults, {}, progress).toJson();
            } else
                result = scene->summary();
            if (!dump.empty()) {
                Args args = options.livery ? options.livery->args : Args{};
                for (auto [a, v] : options.baseline)
                    args[a] = v;
                auto world = scene->evaluate(args);
                Json graph = {{"nodes", Json::array()}, {"meshes", Json::array()}};
                for (size_t i = 0; i < scene->nodes.size(); i++)
                    graph["nodes"].push_back({{"name", scene->nodes[i].name},
                                              {"extras", scene->nodes[i].extras},
                                              {"matrix", matJson(world[i])}});
                for (auto& m : scene->meshes) {
                    auto positions = scene->transformed(m, world);
                    std::vector<F3> samples;
                    for (size_t i = 0; i < positions.size(); i += std::max(size_t(1), positions.size() / 16))
                        samples.push_back(positions[i]);
                    graph["meshes"].push_back({{"name", m.name},
                                               {"vertex_count", positions.size()},
                                               {"indices", m.indices.size()},
                                               {"samples", samples},
                                               {"extras", m.extras}});
                }
                writeJson(dump, graph);
            }
        }
        if (!output.empty() && command != "export")
            writeJson(output, result);
        std::cout << result.dump(2) << "\n";
        return 0;
    } catch (...) {
        std::cerr << "Error: " << exceptionText() << "\n";
        return 1;
    }
}
