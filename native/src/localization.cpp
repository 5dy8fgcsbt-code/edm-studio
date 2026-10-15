#include "common.h"
#include <regex>

namespace edm {
namespace {
struct Catalog {
    Language current = Language::Chinese;
    std::map<std::string, std::string, std::less<>> text;
    std::map<std::wstring, std::wstring, std::less<>> wideText;
    std::string warning;
};
Catalog catalog;

std::vector<std::string> formats(const std::string& s) {
    static const std::regex spec(R"(%(?:%|[-+ #0]*\d*(?:\.\d+)?(?:hh|ll|[hljztL])?[diuoxXfFeEgGaAcspn]))");
    std::vector<std::string> result;
    for (auto i = std::sregex_iterator(s.begin(), s.end(), spec); i != std::sregex_iterator(); ++i)
        result.push_back(i->str());
    return result;
}
void validate(const std::string& key, const std::string& value) {
    require(!value.empty() && value.size() <= 32768, "Invalid translation length");
    // Validate UTF-8 strictly; Windows conversion may otherwise replace invalid bytes.
    require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                                nullptr, 0) > 0,
            "Invalid translation UTF-8");
    require(formats(key) == formats(value), "Translation format placeholders differ");
    require(std::count(key.begin(), key.end(), '\0') == std::count(value.begin(), value.end(), '\0') &&
                (key.back() == '\0') == (value.back() == '\0'),
            "Translation option separators differ");
    auto a = key.find("##"), b = value.find("##");
    require((a == std::string::npos && b == std::string::npos) ||
                (a != std::string::npos && b != std::string::npos && key.substr(a) == value.substr(b)),
            "Translation changed an ImGui ID");
    // Reject stray percent signs that could introduce unsupported printf directives.
    require(std::count(key.begin(), key.end(), '%') == std::count(value.begin(), value.end(), '%'),
            "Translation percent count differs");
}
} // namespace

Language language() {
    return catalog.current;
}
const char* languageCode(Language value) {
    return value == Language::English ? "en-US" : "zh-CN";
}
Language parseLanguage(std::string_view code) {
    if (code == "en" || code == "en-US")
        return Language::English;
    if (code == "zh" || code == "zh-CN")
        return Language::Chinese;
    throw std::runtime_error("Unsupported language; use en-US or zh-CN");
}
const std::string& languagePackWarning() {
    return catalog.warning;
}
std::size_t translationCount() {
    return catalog.text.size();
}

void initializeLanguage(Language value, const fs::path& pack) {
    Catalog next;
    next.current = value;
    auto add = [&](const char* key, size_t kn, const char* text, size_t tn) {
        next.text.emplace(std::string(key, kn), std::string(text, tn));
    };
#include "localization_catalog.inc"
    if (!pack.empty() && value == Language::English) {
        try {
            if (fs::exists(pack)) {
                auto doc = Json::parse(readFile(pack, 4000000));
                require(doc.value("language", "") == "en-US" && doc.value("version", 0) == 1,
                        "Unsupported English language pack");
                const auto& translations = doc.at("translations");
                require(translations.is_object() && translations.size() == next.text.size(),
                        "Language pack is incomplete or belongs to another version");
                auto overrides = next.text;
                for (const auto& [key, text] : translations.items()) {
                    require(overrides.contains(key) && text.is_string(), "Unknown or invalid translation");
                    const auto& translated = text.get_ref<const std::string&>();
                    validate(key, translated);
                    overrides.at(key) = translated;
                }
                next.text = std::move(overrides);
            }
        } catch (const std::exception& e) {
            next.warning =
                std::string("English language pack could not be loaded; using built-in English. ") + e.what();
        }
    }
    for (const auto& [key, text] : next.text)
        next.wideText.emplace(wide(key), wide(text));
    catalog = std::move(next);
}

const char* translateLiteral(const char* source, size_t length) {
    if (catalog.current == Language::English) {
        auto found = catalog.text.find(std::string_view(source, length));
        if (found != catalog.text.end())
            return found->second.c_str();
    }
    return source;
}
const wchar_t* translateWideLiteral(const wchar_t* source, size_t length) {
    if (catalog.current == Language::English) {
        auto found = catalog.wideText.find(std::wstring_view(source, length));
        if (found != catalog.wideText.end())
            return found->second.c_str();
    }
    return source;
}

void configureLanguage(int argc, wchar_t** argv, bool readSettings) {
    wchar_t exe[32768]{};
    GetModuleFileNameW(nullptr, exe, 32768);
    auto directory = fs::path(exe).parent_path();
    auto settings = directory / "edm-studio.settings.json";
    bool smoke = false, diagnostic = false;
    std::optional<Language> requested;
    for (int i = 1; i < argc; ++i) {
        std::wstring_view option(argv[i]);
        if (option == L"--language") {
            require(i + 1 < argc, "Missing --language value");
            requested = parseLanguage(utf8(argv[++i]));
        } else if (option == L"--diagnostic-settings") {
            require(i + 1 < argc, "Missing --diagnostic-settings value");
            settings = argv[++i];
            diagnostic = true;
        } else if (option == L"--smoke")
            smoke = true;
    }
    auto selected = Language::Chinese;
    if (readSettings && (!smoke || diagnostic)) {
        try {
            if (fs::exists(settings)) {
                auto data = Json::parse(readFile(settings, 4000000));
                selected = parseLanguage(data.value("language", "zh-CN"));
            }
        } catch (...) { /* Corrupt settings must not prevent startup. */
        }
    }
    initializeLanguage(requested.value_or(selected), directory / "locales" / "en-US.json");
}
} // namespace edm
