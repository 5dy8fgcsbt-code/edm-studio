#include "assets.h"
#include "lua_bootstrap.h"
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
namespace edm {
namespace {
struct Memory {
    size_t used = 0;
};
void* allocate(void* ud, void* ptr, size_t old, size_t now) {
    auto& m = *static_cast<Memory*>(ud);
    if (!ptr)
        old = 0;
    if (!now) {
        m.used -= old;
        free(ptr);
        return nullptr;
    }
    if (now > old && now - old > 32 * 1024 * 1024 - m.used)
        return nullptr;
    auto* p = realloc(ptr, now);
    if (p)
        m.used = m.used - old + now;
    return p;
}
void push(lua_State* L, const Json& j, int depth = 0) {
    require(depth < 20, "Lua context nesting limit");
    require(lua_checkstack(L, 4), "Lua context stack limit");
    if (j.is_null())
        lua_pushnil(L);
    else if (j.is_boolean())
        lua_pushboolean(L, j.get<bool>());
    else if (j.is_number()) {
        double v = j.get<double>();
        require(std::isfinite(v), "Nonfinite Lua context");
        lua_pushnumber(L, v);
    } else if (j.is_string()) {
        auto& v = j.get_ref<const std::string&>();
        lua_pushlstring(L, v.data(), v.size());
    } else if (j.is_array()) {
        lua_createtable(L, int(j.size()), 0);
        for (size_t i = 0; i < j.size(); i++) {
            push(L, j[i], depth + 1);
            lua_rawseti(L, -2, i + 1);
        }
    } else if (j.is_object()) {
        lua_createtable(L, 0, int(j.size()));
        for (auto it = j.begin(); it != j.end(); ++it) {
            push(L, it.value(), depth + 1);
            lua_setfield(L, -2, it.key().c_str());
        }
    } else
        throw std::runtime_error("Invalid Lua context");
}
Json plain(lua_State* L, int index, int& count, int depth = 0) {
    require(++count < 100000 && depth <= 32, "Lua result is too large or cyclic");
    require(lua_checkstack(L, 4), "Lua result stack limit");
    index = lua_absindex(L, index);
    switch (lua_type(L, index)) {
    case LUA_TNIL:
        return nullptr;
    case LUA_TBOOLEAN:
        return bool(lua_toboolean(L, index));
    case LUA_TNUMBER: {
        double v = lua_tonumber(L, index);
        require(std::isfinite(v), "Nonfinite Lua result");
        return v;
    }
    case LUA_TSTRING: {
        size_t n = 0;
        auto* s = lua_tolstring(L, index, &n);
        return std::string(s, n);
    }
    case LUA_TTABLE: {
        Json out = Json::object();
        lua_pushnil(L);
        while (lua_next(L, index)) {
            std::string key;
            if (lua_type(L, -2) == LUA_TSTRING)
                key = lua_tostring(L, -2);
            else if (lua_isinteger(L, -2))
                key = std::to_string(lua_tointeger(L, -2));
            else if (lua_type(L, -2) == LUA_TNUMBER) {
                double n = lua_tonumber(L, -2);
                require(std::isfinite(n) && n == std::floor(n), "Noninteger Lua table key");
                key = std::to_string(int64_t(n));
            } else
                throw std::runtime_error("Invalid Lua table key");
            out[key] = plain(L, -1, count, depth + 1);
            lua_pop(L, 1);
        }
        return out;
    }
    default:
        throw std::runtime_error("Livery result must contain data, not functions or userdata");
    }
}
struct Includes {
    fs::path path;
    std::string entry;
    size_t bytes = 0, calls = 0;
    std::vector<std::string> files;
};
int include(lua_State* L) {
    std::string error;
    try {
        auto& s = *static_cast<Includes*>(lua_touserdata(L, lua_upvalueindex(1)));
        require(++s.calls <= 64, "Lua include limit exceeded (64)");
        size_t len = 0;
        auto* rawName = lua_tolstring(L, 1, &len);
        require(rawName, "Expected Lua filename");
        std::string name(rawName, len);
        std::replace(name.begin(), name.end(), '\\', '/');
        require(name.find('\0') == std::string::npos, "Invalid include path");
        fs::path relative = wide(name);
        require(lower(pathString(relative.extension())) == ".lua" && !relative.is_absolute() &&
                    !relative.has_root_name(),
                "Only relative Lua text includes allowed");
        auto caller = fs::path(wide(lua_tostring(L, 2) ? lua_tostring(L, 2) : ""));
        require(!s.path.empty(), "No Lua include directory");
        std::vector<uint8_t> bytes;
        std::string resolved;
        if (!s.entry.empty()) {
            auto base = fs::path(wide(s.entry)).parent_path();
            auto target = (base / caller.parent_path() / relative).lexically_normal();
            auto location = pathString(target.generic_wstring());
            require(!location.starts_with("../") && !target.is_absolute() &&
                        location.find(':') == std::string::npos,
                    "Lua include escapes ZIP");
            auto entries = zipEntries(s.path);
            auto it = std::find_if(entries.begin(), entries.end(),
                                   [&](auto& n) { return lower(n) == lower(location); });
            require(it != entries.end(), "Lua include not found: " + name);
            bytes = zipRead(s.path, *it, 2000000);
            resolved = pathString(fs::path(wide(*it)).lexically_relative(base).generic_wstring());
        } else {
            auto root = s.path.parent_path();
            auto target = fs::weakly_canonical(root / caller.parent_path() / relative);
            require(within(target, root.parent_path()), "Lua include escapes livery unit directory");
            bytes = readFile(target, 2000000);
            resolved = pathString(target.lexically_relative(root).generic_wstring());
        }
        s.bytes += bytes.size();
        require(s.bytes <= 8000000, "Lua includes exceed 8 MB");
        s.files.push_back(resolved);
        auto text = decodeText(bytes);
        lua_pushlstring(L, text.data(), text.size());
        lua_pushlstring(L, resolved.data(), resolved.size());
        return 2;
    } catch (const std::exception& e) {
        error = e.what();
    }
    lua_pushlstring(L, error.data(), error.size());
    return lua_error(L);
}
Json execute(const Json& request) {
    Memory mem;
    lua_State* L = lua_newstate(allocate, &mem);
    require(L, "Cannot allocate Lua state");
    std::unique_ptr<lua_State, decltype(&lua_close)> state(L, lua_close);
    luaL_openlibs(L);
    Includes reader;
    reader.path = wide(request.value("path", ""));
    reader.entry = request.value("entry", "");
    auto checked = [&](int result) {
        if (result != LUA_OK)
            throw std::runtime_error(lua_tostring(L, -1) ? lua_tostring(L, -1) : "Lua error");
    };
    checked(luaL_loadbufferx(L, LuaBootstrap, std::strlen(LuaBootstrap), "@sandbox", "t"));
    lua_pushlightuserdata(L, &reader);
    lua_pushcclosure(L, include, 1);
    push(L, request.value("context", Json::object()));
    checked(lua_pcall(L, 2, 1, 0));
    auto text = request.at("text").get<std::string>();
    require(text.size() <= 2000000, "description.lua exceeds 2 MB");
    lua_pushlstring(L, text.data(), text.size());
    checked(lua_pcall(L, 1, 1, 0));
    Json env = Json::object();
    int count = 0;
    for (auto key : {"livery", "name", "custom_args", "countries", "order", "info"}) {
        lua_getfield(L, -1, key);
        if (!lua_isnil(L, -1))
            env[key] = plain(L, -1, count);
        lua_pop(L, 1);
    }
    return {{"environment", env},
            {"evaluation",
             {{"engine", "Lua 5.4.8 native sandbox"},
              {"includes", reader.files},
              {"context", request.value("context", Json::object())}}}};
}
struct Handle {
    HANDLE h = nullptr;
    ~Handle() {
        if (h && h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
    }
    operator HANDLE() const {
        return h;
    }
};
} // namespace
int luaWorker(const fs::path& input, const fs::path& output) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    try {
        auto bytes = readFile(input, 4000000);
        auto j = Json::parse(bytes);
        writeJson(output, {{"ok", true}, {"result", execute(j)}});
        return 0;
    } catch (...) {
        writeJson(output, {{"ok", false}, {"error", exceptionText()}});
        return 1;
    }
}
Json evaluateLua(const std::string& text, const fs::path& path, const std::string& entry,
                 const Json& context) {
    require(text.size() <= 2000000, "description.lua exceeds 2 MB");
    require(context.is_object(), "Lua context must be a JSON object");
    wchar_t temp[MAX_PATH], exe[32768];
    GetTempPathW(MAX_PATH, temp);
    GetModuleFileNameW(nullptr, exe, 32768);
    static std::atomic_uint64_t serial = 0;
    auto base = fs::path(temp) /
                (L"edm-lua-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++serial));
    auto input = base;
    input += L".request.json";
    auto output = base;
    output += L".response.json";
    struct Cleanup {
        fs::path a, b;
        ~Cleanup() {
            std::error_code ec;
            fs::remove(a, ec);
            fs::remove(b, ec);
        }
    } cleanup{input, output};
    writeJson(input, {{"text", text}, {"path", pathString(path)}, {"entry", entry}, {"context", context}});
    std::wstring command = L"\"" + std::wstring(exe) + L"\" --lua-worker \"" + input.wstring() + L"\" \"" +
                           output.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    Handle job{CreateJobObjectW(nullptr, nullptr)};
    require(job.h, "Cannot create Lua job");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    limits.ProcessMemoryLimit = 512ull * 1024 * 1024;
    require(SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)),
            "Cannot configure Lua job");
    require(CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                           nullptr, nullptr, &startup, &process),
            "Cannot launch Lua worker");
    Handle proc{process.hProcess}, thread{process.hThread};
    if (!AssignProcessToJobObject(job, proc)) {
        TerminateProcess(proc, 1);
        throw std::runtime_error("Cannot isolate Lua worker");
    }
    ResumeThread(thread);
    if (WaitForSingleObject(proc, 5000) != WAIT_OBJECT_0) {
        TerminateJobObject(job, 1);
        WaitForSingleObject(proc, 1000);
        throw std::runtime_error("Lua evaluation timed out (5 seconds)");
    }
    DWORD exitCode = 0;
    GetExitCodeProcess(proc, &exitCode);
    require(exitCode == 0 || exitCode == 1,
            "Lua worker crashed (exit code " + std::to_string(exitCode) + ")");
    auto bytes = readFile(output, 16000000);
    auto result = Json::parse(bytes);
    require(result.value("ok", false),
            "动态 Lua 求值失败：" + result.value("error", "worker exited without result"));
    return result.at("result");
}
} // namespace edm
