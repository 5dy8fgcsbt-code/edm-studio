#include "common.h"
#include "assets.h"
#include "paint_stroke_path.h"
#include <iostream>

using namespace edm;
int main() {
    int checks = 0;
    auto check = [&](bool condition, const char* message) {
        ++checks;
        require(condition, std::string("Localization: ") + message);
    };
    try {
        const fs::path pack = fs::path(EDM_TEST_LOCALES) / "en-US.json";
        const auto doc = Json::parse(readFile(pack, 4000000));
        const auto& entries = doc.at("translations");
        initializeLanguage(Language::Chinese);
        check(std::string(tr("打开 EDM")) == "打开 EDM", "Chinese is preserved");
        initializeLanguage(Language::English, pack);
        check(languagePackWarning().empty(), "shipped pack loads without warning");
        check(translationCount() == entries.size(), "embedded and external entry counts match");
        check(std::string(tr("打开 EDM")) == "Open EDM", "UI text is translated");
        check(std::wstring(trw(L"选择贴图目录")) == L"Select a texture folder", "Win32 titles translate");
        const auto* options = tr("曲面贴合\0平面投射\0");
        check(std::string(options) == "Surface conform", "first combo item");
        options += strlen(options) + 1;
        check(std::string(options) == "Planar projection", "second combo item");
        check(options[strlen(options) + 1] == '\0', "double NUL termination");
        check(std::string(tr("Untranslated data")) == "Untranslated data", "unknown text is unchanged");
        char formatted[256];
        snprintf(formatted, sizeof(formatted), tr("受影响网格 %zu · 最大位移 %.3f m"), size_t(23), 1.25);
        check(std::string(formatted) == "Affected meshes 23 / Max. displacement 1.250 m",
              "formatted UI text");
        check(Livery{}.origin == "Manual selection", "default member values use startup language");
        // Every compiled translation must match the editable pack, including embedded NULs.
        initializeLanguage(Language::English);
        for (const auto& [key, value] : entries.items()) {
            auto expected = value.get<std::string>();
            auto actual = translateLiteral(key.data(), key.size());
            check(std::string(actual, expected.size()) == expected, "embedded translation equals JSON");
            auto wk = wide(key), wv = wide(expected);
            check(std::wstring(translateWideLiteral(wk.data(), wk.size()), wv.size()) == wv,
                  "wide translation equals JSON");
        }
        const auto* stable = tr("打开 EDM");
        std::atomic_bool consistent = true;
        std::vector<std::thread> workers;
        for (int i = 0; i < 8; ++i)
            workers.emplace_back([&] {
                for (int j = 0; j < 10000; ++j)
                    if (tr("打开 EDM") != stable || std::string(tr("画笔")) != "Brush")
                        consistent = false;
            });
        for (auto& worker : workers)
            worker.join();
        check(consistent, "worker lookups have stable storage");
        auto temp =
            fs::current_path() / ("localization-test-" + std::to_string(GetCurrentProcessId()) + ".json");
        struct Cleanup {
            fs::path p;
            ~Cleanup() {
                std::error_code e;
                fs::remove(p, e);
            }
        } cleanup{temp};
        auto invalid = [&](Json candidate) {
            writeJson(temp, candidate);
            initializeLanguage(Language::English, temp);
            check(!languagePackWarning().empty(), "invalid pack reports fallback");
            check(std::string(tr("打开 EDM")) == "Open EDM", "invalid pack is rejected atomically");
        };
        auto modified = doc;
        modified["translations"]["打开 EDM"] = "Open aircraft";
        writeJson(temp, modified);
        initializeLanguage(Language::English, temp);
        check(std::string(tr("打开 EDM")) == "Open aircraft", "valid external override used");
        modified["translations"]["参数 %03d"] = "Parameter %s";
        invalid(modified);
        modified = doc;
        modified["translations"]["打开 EDM"] = "%n";
        invalid(modified);
        modified = doc;
        modified["translations"]["打开 EDM"] = "Open##changed";
        invalid(modified);
        modified = doc;
        modified["translations"].erase("打开 EDM");
        invalid(modified);
        modified = doc;
        const std::string comboKey("曲面贴合\0平面投射\0", sizeof("曲面贴合\0平面投射\0") - 1);
        modified["translations"][comboKey] = "Surface conform";
        invalid(modified);
        modified = doc;
        modified["version"] = 99;
        invalid(modified);
        modified = doc;
        modified["translations"]["打开 EDM"] = "";
        invalid(modified);
        {
            std::ofstream output(temp);
            output << "{broken";
        }
        initializeLanguage(Language::English, temp);
        check(!languagePackWarning().empty(), "malformed JSON falls back");
        initializeLanguage(Language::English, temp.wstring() + L".missing");
        check(languagePackWarning().empty() && std::string(tr("画笔")) == "Brush", "standalone EXE fallback");
        // Persistent settings, explicit CLI override, and smoke isolation.
        auto configure = [&](std::vector<std::wstring> values) {
            std::vector<wchar_t*> args;
            for (auto& value : values)
                args.push_back(value.data());
            configureLanguage(static_cast<int>(args.size()), args.data());
        };
        writeJson(temp, {{"language", "en-US"}});
        configure({L"app", L"--diagnostic-settings", temp.wstring()});
        check(language() == Language::English, "saved language restored");
        configure({L"app", L"--diagnostic-settings", temp.wstring(), L"--language", L"zh-CN"});
        check(language() == Language::Chinese, "CLI overrides settings");
        configure({L"app", L"--smoke"});
        check(language() == Language::Chinese, "smoke does not read user settings");
        writeJson(temp, {{"language", "unknown"}});
        configure({L"app", L"--diagnostic-settings", temp.wstring()});
        check(language() == Language::Chinese, "unknown saved language uses Chinese");
        bool rejected = false;
        try {
            configure({L"app", L"--language", L"invalid"});
        } catch (...) {
            rejected = true;
        }
        check(rejected, "invalid explicit language rejected");
        std::cout << "Localization: " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
