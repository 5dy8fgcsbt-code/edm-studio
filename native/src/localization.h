#pragma once
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace edm {
enum class Language { Chinese, English };
Language language();
const char* languageCode(Language value);
Language parseLanguage(std::string_view code);
// Startup only, before creating UI objects or worker threads. Returned translation
// pointers remain valid until the next initialization (normally process exit).
void initializeLanguage(Language value, const std::filesystem::path& pack = {});
void configureLanguage(int argc, wchar_t** argv, bool readSettings = true);
const std::string& languagePackWarning();
std::size_t translationCount();
const char* translateLiteral(const char* source, std::size_t length);
const wchar_t* translateWideLiteral(const wchar_t* source, std::size_t length);
template <std::size_t N> const char* tr(const char (&source)[N]) {
    return translateLiteral(source, N - 1);
}
template <std::size_t N> const wchar_t* trw(const wchar_t (&source)[N]) {
    return translateWideLiteral(source, N - 1);
}
} // namespace edm
