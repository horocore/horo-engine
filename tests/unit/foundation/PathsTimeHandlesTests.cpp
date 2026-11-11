#include "Horo/Foundation/Handles.h"
#include "Horo/Foundation/PathUtils.h"
#include "Horo/Foundation/Paths.h"
#include "Horo/Foundation/Time.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

namespace {
    struct TextureTag;
    struct MeshTag;

    TEST_CASE("Project Path Normalizes Portable Separators", "[unit][foundation]") {
        const auto result = Horo::ProjectPath::Parse("Assets\\Textures/../UI/icon.png");
        REQUIRE((result.HasValue()));
        REQUIRE((result.Value().String() == "Assets/UI/icon.png"));
    }

    TEST_CASE("Project Path Rejects Root Escape", "[unit][foundation]") {
        const auto result = Horo::ProjectPath::Parse("../../outside.txt");
        REQUIRE((result.HasError()));
    }

    TEST_CASE("Project Path Resolves Portable UTF-8 To The Exact Native Path", "[unit][foundation]") {
        const auto root = std::filesystem::temp_directory_path() / u8"\u5de5\u7a0b";
        const std::u8string_view encoded{u8"assets/\u56fe \u011e/\u6a21\u578b.txt"};
        const std::string portable{encoded.begin(), encoded.end()};
        const auto relative = Horo::ProjectPath::Parse(portable);
        REQUIRE(relative.HasValue());
        const auto expected = root / std::filesystem::path{encoded};
        CHECK(Horo::Foundation::Paths::Resolve(root, relative.Value()) == expected);
        const auto resolved = Horo::Foundation::Paths::Resolve(root, portable);
        REQUIRE(resolved.HasValue());
        CHECK(resolved.Value() == expected);
    }

    TEST_CASE("Path Prefix Uses Normalized Components", "[unit][foundation]") {
        const std::filesystem::path root = std::filesystem::temp_directory_path() / "horo-path-prefix-test";
        REQUIRE(Horo::Foundation::Paths::HasPathPrefix(root, root / "nested/file.txt"));
        REQUIRE(Horo::Foundation::Paths::HasPathPrefix(root, root));
        REQUIRE_FALSE(Horo::Foundation::Paths::HasPathPrefix(root, root.parent_path() / "outside/file.txt"));
    }

    TEST_CASE("Native I/O path conversion preserves caller spelling and uses one Windows namespace", "[unit][foundation]") {
        using Horo::Foundation::Paths::NativeApiPath;
        const std::filesystem::path relative{"assets/source.bin"};
        CHECK(NativeApiPath(relative) == relative);
#if defined(_WIN32)
        const std::filesystem::path drive{LR"(C:\project\source.bin)"};
        const auto native = NativeApiPath(drive);
        CHECK(native == std::filesystem::path{LR"(\\?\C:\project\source.bin)"});
        CHECK(drive == std::filesystem::path{LR"(C:\project\source.bin)"});
        CHECK(NativeApiPath(native) == native);
        const std::filesystem::path unc{LR"(\\server\share\source.bin)"};
        CHECK(NativeApiPath(unc) == std::filesystem::path{LR"(\\?\UNC\server\share\source.bin)"});
        const std::filesystem::path device{LR"(\\.\pipe\source)"};
        CHECK(NativeApiPath(device) == device);
        const std::filesystem::path unnormalized{LR"(C:\project\..\source.bin)"};
        CHECK(NativeApiPath(unnormalized) == unnormalized);
#else
        const std::filesystem::path absolute{"/project/source.bin"};
        CHECK(NativeApiPath(absolute) == absolute);
#endif
    }

    TEST_CASE("Typed Handles Reject Different Resource Tags", "[unit][foundation]") {
        constexpr Horo::Handle<TextureTag> texture{.index = 4, .generation = 9};
        constexpr Horo::Handle<MeshTag> mesh{.index = 4, .generation = 9};
        REQUIRE((texture.IsValid()));
        REQUIRE((mesh.IsValid()));
        static_assert(!std::is_same_v<decltype(texture), decltype(mesh)>);
    }

    TEST_CASE("Default Handle Is Invalid", "[unit][foundation]") {
        constexpr Horo::Handle<TextureTag> handle{};
        REQUIRE((!handle.IsValid()));
    }

    TEST_CASE("Monotonic Duration Uses Steady Clock Duration", "[unit][foundation]") {
        constexpr Horo::Duration duration = Horo::Duration::FromMilliseconds(25);
        REQUIRE((duration.ToMilliseconds() == 25));
        constexpr Horo::Duration sum = duration + Horo::Duration::FromMilliseconds(5);
        REQUIRE((sum.ToMilliseconds() == 30));
        constexpr Horo::Duration precise = Horo::Duration::FromNanoseconds(16'666'667);
        REQUIRE((precise.ToNanoseconds() == 16'666'667));
        REQUIRE(((sum - duration).ToMilliseconds() == 5));
        REQUIRE((precise > Horo::Duration{}));
    }
}  // namespace
