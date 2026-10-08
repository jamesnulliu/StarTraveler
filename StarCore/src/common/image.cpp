module;

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

module starcore.common;

import std;

namespace starcore
{

auto writePng(const ColorImage& image, const std::filesystem::path& path)
    -> std::expected<void, std::string>
{
    if (image.empty()) {
        return std::unexpected("cannot write an empty image");
    }
    if (path.has_parent_path()) {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
    }
    // packRgba8 stores R in the lowest byte, i.e. RGBA byte order in memory
    // on little-endian machines, which is what stb expects.
    const auto stride = static_cast<int>(image.width() * sizeof(std::uint32_t));
    const int ok = stbi_write_png(path.string().c_str(),
                                  static_cast<int>(image.width()),
                                  static_cast<int>(image.height()), 4,
                                  image.data(), stride);
    if (ok == 0) {
        return std::unexpected(
            std::format("failed to write PNG \"{}\"", path.string()));
    }
    return {};
}

}  // namespace starcore
