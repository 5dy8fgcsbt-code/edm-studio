#include "paint_assembly.h"
#include "attachment.h"
#include <charconv>
#include <limits>

namespace edm {
namespace {
void cancelled(const std::atomic_bool* cancel) {
    require(!cancel || !cancel->load(), "涂装工程组合恢复已取消");
}
int argumentIndex(std::string_view text) {
    int index = -1;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), index);
    require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && index >= 0 &&
                std::to_string(index) == text,
            "涂装工程含无效动画参数编号");
    return index;
}
int integer(const Json& value, int minimum = 0) {
    require(value.is_number_integer(), "涂装工程组合索引不是整数");
    require(!value.is_number_unsigned() || value.get<uint64_t>() <= uint64_t(std::numeric_limits<int>::max()),
            "涂装工程组合索引越界");
    const auto index = value.get<int64_t>();
    require(index >= minimum && index <= std::numeric_limits<int>::max(), "涂装工程组合索引越界");
    return int(index);
}
Args defaultArguments(const Json& value) {
    require(value.is_object() && value.size() <= 65536, "涂装工程动画参数配置无效或过大");
    Args result;
    for (auto it = value.begin(); it != value.end(); ++it) {
        const int argument = argumentIndex(it.key());
        require(it.value().is_number(), "涂装工程动画参数值不是数字");
        const double number = it.value().get<double>();
        require(std::isfinite(number), "涂装工程动画参数值不是有限数字");
        result[argument] = number;
    }
    return result;
}
fs::path sourcePath(const std::string& value, const fs::path& directory) {
    require(!value.empty() && value.find('\0') == std::string::npos, "涂装工程缺少有效外挂 EDM 路径");
    fs::path path = wide(value);
    if (path.is_relative())
        path = directory / path;
    require(fs::is_regular_file(path), "涂装工程源 EDM 不存在：" + pathString(path));
    return fs::weakly_canonical(path);
}
Json comparable(Json metadata, const fs::path& directory) {
    // The same local file may have different slash, casing or relative path spellings. All graph
    // indices and connector names remain exact, including duplicate Point names on separate racks.
    for (auto& attachment : metadata["attachments"]) {
        fs::path source = wide(attachment.at("source").get<std::string>());
        if (source.is_relative())
            source = directory / source;
        attachment["source"] = lower(pathString(fs::weakly_canonical(source)));
    }
    return metadata;
}
} // namespace

std::shared_ptr<Scene> restorePaintAssembly(std::shared_ptr<Scene> baseScene, const fs::path& projectPath,
                                            Progress progress, const std::atomic_bool* cancel) {
    require(baseScene != nullptr, "恢复涂装工程组合需要已加载的主模型");
    cancelled(cancel);
    const auto project = fs::is_directory(projectPath) ? projectPath / "project.edmpaint.json" : projectPath;
    const auto directory = fs::absolute(project).parent_path();
    const auto manifest = Json::parse(readFile(project, 2000000));
    require(manifest.is_object() && manifest.value("format", "") == "EDM Studio Paint" &&
                manifest.value("version", 0) == 2,
            "不是受支持的 EDM 绘制工程（需要版本 2）");
    Json expected = manifest.value(
        "assembly", Json{{"version", 1}, {"attachments", Json::array()}, {"default_args", Json::object()}});
    require(expected.is_object() && expected.value("version", 0) == 1 &&
                expected.at("attachments").is_array() && expected.at("attachments").size() <= 4096,
            "涂装工程外挂组合配置无效或版本不受支持");
    const auto defaults = defaultArguments(expected.at("default_args"));
    std::vector<fs::path> sources;
    std::vector<std::map<int, int>> argumentMaps;
    std::set<int> externalArguments;
    for (const auto& saved : expected["attachments"]) {
        cancelled(cancel);
        require(saved.is_object(), "涂装工程外挂记录无效");
        sources.push_back(sourcePath(saved.at("source").get<std::string>(), directory));
        for (const auto* key : {"target_node", "root", "node_begin", "node_count", "material_begin",
                                "material_count", "mesh_begin", "mesh_count"})
            integer(saved.at(key));
        integer(saved.at("attach_node"), -1);
        require(saved.at("target_name").is_string() && saved.at("attach_name").is_string(),
                "涂装工程连接点名称无效");
        const auto& map = saved.at("argument_map");
        require(map.is_object() && map.size() <= 65536, "涂装工程外挂参数映射无效或过大");
        std::map<int, int> arguments;
        std::set<int> unique;
        for (auto it = map.begin(); it != map.end(); ++it) {
            const int original = argumentIndex(it.key()), merged = integer(it.value());
            require(unique.insert(merged).second, "涂装工程外挂参数映射存在重复目标");
            arguments[original] = merged;
            externalArguments.insert(merged);
        }
        argumentMaps.push_back(std::move(arguments));
    }
    const auto expectedComparable = comparable(expected, directory);
    if (comparable(paintAssemblyMetadata(*baseScene), fs::current_path()) == expectedComparable) {
        cancelled(cancel);
        return baseScene;
    }
    cancelled(cancel);
    require(!baseScene->source.empty() && fs::is_regular_file(baseScene->source),
            "无法重新加载主模型 EDM：" + pathString(baseScene->source));
    if (progress)
        progress("恢复涂装工程主模型与外挂组合…");
    cancelled(cancel);
    auto result = Scene::load(baseScene->source, progress, cancel);
    // Base defaults reserve their original argument indices before attachScene allocates the
    // independent parameter namespace of each store. Do not reserve future stores' merged indices.
    for (auto [argument, value] : defaults)
        if (!externalArguments.contains(argument))
            result->defaultArgs[argument] = value;
    for (size_t index = 0; index < sources.size(); ++index) {
        cancelled(cancel);
        const auto& saved = expected["attachments"][index];
        const int target = integer(saved["target_node"]);
        const auto name = saved["target_name"].get<std::string>();
        require(target < int(result->nodes.size()) && isConnector(result->nodes[target]) &&
                    result->nodes[target].name == name,
                "涂装工程挂点索引或名称与源 EDM 不一致：" + name + " (#" + std::to_string(target) + ")");
        if (progress)
            progress("恢复外挂 " + std::to_string(index + 1) + " / " + std::to_string(sources.size()) +
                     " · " + pathString(sources[index].filename()));
        cancelled(cancel);
        auto child = Scene::load(sources[index], progress, cancel);
        Args childDefaults;
        for (auto [original, merged] : argumentMaps[index])
            if (defaults.contains(merged))
                childDefaults[original] = defaults.at(merged);
        cancelled(cancel);
        result = attachScene(*result, *child, target, childDefaults);
        const auto actual = comparable(paintAssemblyMetadata(*result), fs::current_path());
        require(actual["attachments"].size() == index + 1 &&
                    actual["attachments"][index] == expectedComparable["attachments"][index],
                "涂装工程外挂结构或动画参数映射与源 EDM 不一致：" + pathString(sources[index]));
    }
    // Preserve the saved values exactly, including a deliberate absence of default overrides.
    result->defaultArgs = defaults;
    result->defaultWorld = result->evaluate({});
    require(comparable(paintAssemblyMetadata(*result), fs::current_path()) == expectedComparable,
            "涂装工程组合结构与源 EDM 不一致");
    cancelled(cancel);
    return result;
}
} // namespace edm
