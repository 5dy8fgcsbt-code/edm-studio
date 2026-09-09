#include "paint_quality.h"
#include <iostream>

using namespace edm;
namespace {
int checks = 0;
void check(bool value, const char* message) {
    ++checks;
    require(value, std::string("Paint quality regression: ") + message);
}
void checkNear(double actual, double expected, const char* message) {
    check(std::abs(actual - expected) <= std::max(1., std::abs(expected)) * 1e-9, message);
}
using Coordinates = std::array<Eigen::Vector2d, 3>;
Coordinates unitCoordinates() {
    return {Eigen::Vector2d{0, 0}, Eigen::Vector2d{1, 0}, Eigen::Vector2d{0, 1}};
}
PaintTriangle triangle() {
    PaintTriangle value{};
    value.world = {V3{0, 0, 0}, V3{1, 0, 0}, V3{0, 1, 0}};
    value.normals = {V3::UnitZ(), V3::UnitZ(), V3::UnitZ()};
    value.uv = {F2{0, 0}, F2{1, 0}, F2{0, 1}};
    value.hasUV = true;
    return value;
}
DecalTexelEstimate estimate(const PaintTriangle& face, const Coordinates& xy, uint32_t width,
                            uint32_t height, double decalWidth = 1, double decalHeight = 1) {
    auto result = estimateDecalTexels(face, xy, width, height, decalWidth, decalHeight);
    check(result.has_value(), "A well-formed local mapping has a texel-density estimate");
    return *result;
}
void analyticCoverage() {
    auto face = triangle();
    auto xy = unitCoordinates();
    auto value = estimate(face, xy, 4096, 2048, .25, .5);
    checkNear(value.width, 1024, "Decal width scales by horizontal canvas resolution and physical extent");
    checkNear(value.height, 1024, "Decal height independently uses the non-square canvas height");

    face.uv = {F2{.125f, .25f}, F2{.625f, .25f}, F2{.125f, .5f}};
    value = estimate(face, xy, 4096, 2048, .25, .5);
    checkNear(value.width, 512, "An island using half the U range halves available horizontal detail");
    checkNear(value.height, 256, "An island using a quarter of V independently reduces vertical detail");

    face = triangle();
    xy = {Eigen::Vector2d{0, 0}, Eigen::Vector2d{0, 1}, Eigen::Vector2d{-1, 0}};
    value = estimate(face, xy, 4096, 2048, .25, .5);
    checkNear(value.width, 512, "A quarter-turn exchanges the UV axes seen by the decal width");
    checkNear(value.height, 2048, "A quarter-turn uses U density for the decal height");

    const double c = std::sqrt(.5);
    xy = {Eigen::Vector2d{0, 0}, Eigen::Vector2d{c, c}, Eigen::Vector2d{-c, c}};
    value = estimate(face, xy, 4096, 2048);
    checkNear(value.width, std::hypot(4096 * c, 2048 * c),
         "Oblique placement uses the length of the complete pixel-space derivative");
    checkNear(value.height, value.width, "A diagonal placement has equal densities on this fixture");

    xy = unitCoordinates();
    face.uv = {F2{1, 0}, F2{0, 0}, F2{1, 1}};
    value = estimate(face, xy, 1024, 512);
    checkNear(value.width, 1024, "Mirrored UVs retain positive horizontal detail");
    checkNear(value.height, 512, "Mirroring does not change vertical detail");

    face.uv = {F2{0, 0}, F2{1, .5f}, F2{.5f, 1}};
    value = estimate(face, xy, 1024, 512);
    checkNear(value.width, std::hypot(1024., 256.), "Sheared UVs include both pixel axes in X density");
    checkNear(value.height, std::hypot(512., 512.), "Sheared UVs include both pixel axes in Y density");

    auto shifted = xy;
    for (auto& point : shifted)
        point += Eigen::Vector2d{321.5, -67.25};
    const auto translated = estimate(face, shifted, 1024, 512);
    checkNear(translated.width, value.width, "Moving the decal origin does not alter local density");
    checkNear(translated.height, value.height, "Local density is independent of chart translation");
    for (auto& point : xy)
        point *= 1e-15;
    const auto tiny = estimate(face, xy, 1024, 512, 1e-15, 1e-15);
    checkNear(tiny.width, value.width, "Small well-shaped triangles are not mistaken for degenerate XYs");
    checkNear(tiny.height, value.height, "A change of physical units preserves pixel coverage");
}
void invalidCoverage() {
    auto face = triangle();
    auto xy = unitCoordinates();
    auto invalid = [&](const char* message) {
        check(!estimateDecalTexels(face, xy, 4096, 2048, 1, 1), message);
    };
    face.hasUV = false;
    invalid("A surface with no painting UVs has no meaningful coverage estimate");
    face = triangle();
    face.uv = {F2{0, 0}, F2{.5f, .5f}, F2{1, 1}};
    invalid("Collinear UVs do not produce a misleading positive estimate");
    face = triangle();
    xy[2] = xy[1];
    invalid("Coincident chart vertices are rejected");
    xy[2] = Eigen::Vector2d{2, 0};
    invalid("A collinear chart is rejected");
    xy = unitCoordinates();
    xy[1].x() = std::numeric_limits<double>::infinity();
    invalid("Infinite chart coordinates are rejected");
    xy = unitCoordinates();
    face.uv[0][0] = std::numeric_limits<float>::quiet_NaN();
    invalid("Nonfinite UVs are rejected");
    face = triangle();
    check(!estimateDecalTexels(face, xy, 0, 2048, 1, 1), "A zero-width canvas is rejected");
    check(!estimateDecalTexels(face, xy, 4096, 0, 1, 1), "A zero-height canvas is rejected");
    check(!estimateDecalTexels(face, xy, 4096, 2048, 0, 1), "A zero-width decal is rejected");
    check(!estimateDecalTexels(face, xy, 4096, 2048, 1, -1), "A negative decal height is rejected");
    check(!estimateDecalTexels(face, xy, 4096, 2048, 1,
                               std::numeric_limits<double>::infinity()),
          "An infinite decal dimension is rejected");
    check(!estimateDecalTexels(face, xy, 4096, 2048,
                               std::numeric_limits<double>::quiet_NaN(), 1),
          "A NaN decal dimension is rejected");
    check(!estimateDecalTexels(face, xy, 4096, 2048, std::numeric_limits<double>::max(), 1),
          "An overflowing texel estimate is rejected");
}

struct TestFiles {
    fs::path root;
    TestFiles() {
        root = fs::temp_directory_path() /
               ("edm-paint-quality-tests-" + std::to_string(GetCurrentProcessId()) + "-" +
                std::to_string(Clock::now().time_since_epoch().count()));
        require(fs::create_directory(root), "Cannot create isolated paint-quality fixture directory");
    }
    ~TestFiles() {
        // Remove only the four files this test writes; leave unexpected contents untouched.
        std::error_code error;
        for (const auto* name : {"source-4096.dds", "source-8192.dds", "non-square.png", "small.png"})
            fs::remove(root / name, error);
        fs::remove(root, error);
    }
};

// Write an authored BC1 mip chain with a fine red/white top-level pattern. Lower mip levels have
// different solid colours, making an accidental selection of a reduced mip observable in pixels.
// Fixtures contain no external artwork and use little memory while writing the full 8K case.
void writeMipFixture(const fs::path& path, uint32_t dimension) {
    uint32_t levels = 1;
    for (uint32_t size = dimension; size > 1; size /= 2)
        ++levels;
    std::array<uint32_t, 32> header{};
    header[0] = 0x20534444; // "DDS "
    header[1] = 124;
    header[2] = 0x000a1007; // CAPS, WIDTH, HEIGHT, PIXELFORMAT, MIPMAPCOUNT, LINEARSIZE.
    header[3] = header[4] = dimension;
    header[5] = ((dimension + 3) / 4) * ((dimension + 3) / 4) * 8;
    header[7] = levels;
    header[19] = 32;
    header[20] = 4; // DDPF_FOURCC.
    header[21] = 0x31545844; // "DXT1"
    header[27] = 0x00401008; // DDSCAPS_TEXTURE | COMPLEX | MIPMAP.
    std::ofstream file(path, std::ios::binary);
    require(bool(file), "Cannot create synthetic DDS fixture");
    file.write(reinterpret_cast<const char*>(header.data()), sizeof(header));
    for (uint32_t level = 0, size = dimension; level < levels; ++level, size = std::max(1u, size / 2)) {
        const uint32_t blocks = (size + 3) / 4;
        std::vector<uint8_t> row(size_t(blocks) * 8, 0);
        for (uint32_t block = 0; block < blocks; ++block) {
            const uint16_t colour = level == 0 ? (block % 2 ? 0xffff : 0xf800) :
                                    level == 1 ? 0x07e0 : 0x001f;
            row[size_t(block) * 8] = uint8_t(colour);
            row[size_t(block) * 8 + 1] = uint8_t(colour >> 8);
        }
        for (uint32_t rowIndex = 0; rowIndex < blocks; ++rowIndex)
            file.write(reinterpret_cast<const char*>(row.data()), std::streamsize(row.size()));
    }
    file.flush();
    require(bool(file), "Cannot write complete synthetic DDS mip chain");
}
std::array<uint8_t, 4> pixel(const PaintImage& image, uint32_t x, uint32_t y) {
    const size_t offset = (size_t(y) * image.width + x) * 4;
    return {image.rgba[offset], image.rgba[offset + 1], image.rgba[offset + 2], image.rgba[offset + 3]};
}
void sourceResolution() {
    TestFiles files;
    for (const uint32_t dimension : {4096u, 8192u}) {
        const auto path = files.root / ("source-" + std::to_string(dimension) + ".dds");
        writeMipFixture(path, dimension);
        {
            const auto image = PaintImage::load(ImageSource{path}, DefaultPaintMaxDimension);
            check(image.width == dimension && image.height == dimension,
                  "The default editing ceiling preserves an authored 4K or 8K top-level texture");
            check(pixel(image, 0, 0) == std::array<uint8_t, 4>{255, 0, 0, 255} &&
                      pixel(image, 4, 0) == std::array<uint8_t, 4>{255, 255, 255, 255},
                  "Default loading keeps fine top-level artwork instead of selecting a coarser mip");
        }
        {
            const auto image = PaintImage::load(ImageSource{path}, 2048);
            check(image.width == 2048 && image.height == 2048,
                  "An explicit 2K editing ceiling still selects a smaller authored mip");
            const std::array<uint8_t, 4> expected = dimension == 4096 ?
                std::array<uint8_t, 4>{0, 255, 0, 255} : std::array<uint8_t, 4>{0, 0, 255, 255};
            check(pixel(image, 0, 0) == expected,
                  "Explicit low-resolution loading is distinguishable from preserving the top-level pixels");
        }
    }
    const auto rectangular = files.root / "non-square.png";
    PaintImage(4097, 1025, {17, 63, 119, 211}).savePNG(rectangular);
    {
        const auto image = PaintImage::load(ImageSource{rectangular}, DefaultPaintMaxDimension);
        check(image.width == 4097 && image.height == 1025,
              "Default editing preserves non-square, non-power-of-two source dimensions exactly");
        check(pixel(image, 4096, 1024) == std::array<uint8_t, 4>{17, 63, 119, 211},
              "Default loading preserves original edge pixels and alpha");
    }
    {
        const auto image = PaintImage::load(ImageSource{rectangular}, 2048);
        check(image.width == 2048 && image.height == 512,
              "An explicit limit downsizes a non-square source while maintaining its aspect ratio");
    }
    const auto smallPath = files.root / "small.png";
    PaintImage(37, 23, {27, 73, 129, 197}).savePNG(smallPath);
    for (const int cap : {2048, DefaultPaintMaxDimension}) {
        const auto image = PaintImage::load(ImageSource{smallPath}, cap);
        check(image.width == 37 && image.height == 23,
              "An editing ceiling never enlarges an already smaller source texture");
        check(pixel(image, 36, 22) == std::array<uint8_t, 4>{27, 73, 129, 197},
              "A small original texture keeps its pixels and alpha");
    }
}
} // namespace

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    try {
        ComRuntime runtime;
        analyticCoverage();
        invalidCoverage();
        sourceResolution();
        std::cout << "Paint quality: " << checks << " checks passed\n";
        return 0;
    } catch (...) {
        std::cerr << exceptionText() << '\n';
        return 1;
    }
}
