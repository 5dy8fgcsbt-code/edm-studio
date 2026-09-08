#pragma once
#include <windows.h>
#include <objbase.h>
#include <Eigen/Dense>
#include <Eigen/Geometry>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
namespace edm {
namespace fs = std::filesystem;
using Json = nlohmann::json;
using Mat = Eigen::Matrix4d;
using V3 = Eigen::Vector3d;
using V4 = Eigen::Vector4d;
using F2 = std::array<float, 2>;
using F3 = std::array<float, 3>;
using F4 = std::array<float, 4>;
using Args = std::map<int, double>;
using Progress = std::function<void(const std::string&)>;
using Clock = std::chrono::steady_clock;
inline double seconds(Clock::time_point from) {
    return std::chrono::duration<double>(Clock::now() - from).count();
}
inline void require(bool valid, const std::string& message) {
    if (!valid)
        throw std::runtime_error(message);
}
// Balance every successful COM initialization, including nested image operations.
struct ComApartment {
    HRESULT status;
    explicit ComApartment(DWORD mode = COINIT_MULTITHREADED) : status(CoInitializeEx(nullptr, mode)) {
        require(SUCCEEDED(status) || status == RPC_E_CHANGED_MODE, "Cannot initialize Windows COM");
    }
    ~ComApartment() {
        if (SUCCEEDED(status))
            CoUninitialize();
    }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
};
// DirectXTex caches its WIC factory across threads. Keep MTA/DLLs alive across
// short-lived image jobs; create this at the executable entry point, before COM
// apartments, and destroy it on normal return after every worker has joined.
struct ComRuntime {
    CO_MTA_USAGE_COOKIE cookie = nullptr;
    ComRuntime() {
        require(SUCCEEDED(CoIncrementMTAUsage(&cookie)), "Cannot initialize image runtime");
    }
    ~ComRuntime() {
        if (cookie)
            CoDecrementMTAUsage(cookie);
    }
    ComRuntime(const ComRuntime&) = delete;
    ComRuntime& operator=(const ComRuntime&) = delete;
};
std::wstring wide(std::string_view s, unsigned cp = CP_UTF8);
std::string utf8(std::wstring_view s);
std::string pathString(const fs::path& p);
std::string lower(std::string s);
std::string normalized(std::string s);
std::vector<uint8_t> readFile(const fs::path& p, size_t limit = 512ull * 1024 * 1024);
void writeFile(const fs::path& p, std::span<const uint8_t> bytes);
void writeJson(const fs::path& p, const Json& j);
std::string decodeText(std::span<const uint8_t> bytes);
bool within(const fs::path& child, const fs::path& root);
std::vector<fs::path> uniquePaths(const std::vector<fs::path>& paths);
struct MappedFile {
    HANDLE file = INVALID_HANDLE_VALUE, mapping = nullptr;
    const uint8_t* data = nullptr;
    size_t size = 0;
    explicit MappedFile(const fs::path& path);
    ~MappedFile();
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
};
inline Mat translation(const V3& v) {
    Mat m = Mat::Identity();
    m.block<3, 1>(0, 3) = v;
    return m;
}
inline Mat scaling(const V3& v) {
    Mat m = Mat::Identity();
    m.diagonal().head<3>() = v;
    return m;
}
inline V4 qnormalize(V4 q) {
    double n = q.norm();
    require(std::isfinite(n) && n > 1e-12, "Invalid quaternion");
    return q / n;
}
inline Mat rotation(V4 q) {
    q = qnormalize(q);
    Mat m = Mat::Identity();
    m.block<3, 3>(0, 0) = Eigen::Quaterniond(q[3], q[0], q[1], q[2]).toRotationMatrix();
    return m;
}
inline V4 qinverse(V4 q) {
    q = qnormalize(q);
    q.head<3>() *= -1;
    return q;
}
inline V4 quatMatrix(const Eigen::Matrix3d& m) {
    Eigen::Quaterniond q(m);
    return qnormalize(q.coeffs());
}
V4 slerp(V4 a, V4 b, double t);
inline double argValue(const Args& args, int arg) {
    auto it = args.find(arg);
    return it == args.end() ? 0. : it->second;
}
template <class T> Json arrayJson(const T& v) {
    Json j = Json::array();
    for (auto x : v)
        j.push_back(x);
    return j;
}
inline Json matJson(const Mat& m) {
    return arrayJson(std::span(m.data(), 16));
}
inline std::string exceptionText() {
    try {
        throw;
    } catch (const std::exception& e) {
        return e.what();
    } catch (...) {
        return "Unknown native error";
    }
}
} // namespace edm
