#include "EditorBaselineLightingAssets.h"
#include "support/TypedIdentityTestSupport.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <fstream>
#include <iterator>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    constexpr std::array<std::string_view, 2> Suffixes{".horoshdr", ".reflection.json"};
    constexpr std::string_view InvalidBytes{"ordinary inventory canary; not compiled shader evidence"};

    /** @brief Owns only a freshly claimed temporary directory; removes no caller package or external target. */
    struct Bundle final {
        std::filesystem::path root;

        /** @brief Claims an isolated directory with spaces and Unicode to exercise ordinary path handling. */
        Bundle() {
            static std::atomic<std::uint64_t> serial{};
            root = std::filesystem::temp_directory_path() /
                   ("horo baseline assets " + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + " " +
                    std::to_string(serial.fetch_add(1)) + " örnek");
            REQUIRE(std::filesystem::create_directory(root));
            REQUIRE(std::filesystem::create_directories(root / "shaders/editor"));
        }

        /** @brief Releases only this test's originally claimed temporary tree. */
        ~Bundle() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        /** @brief Resolves an exact declared pair in this owned directory. */
        std::filesystem::path File(std::size_t index, std::size_t suffix) const {
            return root / (std::string{PackagedMetalBaselineLightingPrefixes()[index]} + std::string{Suffixes[suffix]});
        }
    };

    /** @brief Writes actual bounded ordinary-file canary bytes, without constructing a fake shader. */
    void Write(const std::filesystem::path &path, std::string_view value = InvalidBytes) {
        std::ofstream output(path, std::ios::binary);
        REQUIRE(output.good());
        output.write(value.data(), static_cast<std::streamsize>(value.size()));
        REQUIRE(output.good());
    }

    /** @brief Borrows exact ordinary-file contents for preservation assertions. */
    std::string Read(const std::filesystem::path &path) {
        std::ifstream input(path, std::ios::binary);
        REQUIRE(input.good());
        return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    }

    /** @brief Creates only six inventory canaries; this deliberately cannot qualify positive shader decoding. */
    void CompleteInventory(const Bundle &bundle) {
        for (std::size_t i = 0; i < 3; ++i)
            for (std::size_t suffix = 0; suffix < 2; ++suffix)
                Write(bundle.File(i, suffix));
    }

    /** @brief Verifies all ordinary canaries were retained byte-for-byte after a failed cold operation. */
    void RequireUnchanged(const Bundle &bundle) {
        for (std::size_t i = 0; i < 3; ++i)
            for (std::size_t suffix = 0; suffix < 2; ++suffix)
                CHECK(Read(bundle.File(i, suffix)) == InvalidBytes);
    }
}  // namespace

TEST_CASE("Metal baseline inventory reports genuine complete absence and loading fails without fallback", "[editor][baseline-assets]") {
    Bundle bundle;
    const auto absent = HasPackagedMetalBaselineLightingInputs(bundle.root);
    REQUIRE(absent.HasValue());
    CHECK_FALSE(absent.Value());
    Tests::RequireActionableError(LoadPackagedMetalBaselineLighting(bundle.root), BaselineLightingAssetErrors::Unavailable);
    CHECK(std::filesystem::is_directory(bundle.root / "shaders/editor"));
    CHECK(std::filesystem::is_empty(bundle.root / "shaders/editor"));
    const auto missingRoot = bundle.root / "uncreated package";
    const auto missing = HasPackagedMetalBaselineLightingInputs(missingRoot);
    REQUIRE(missing.HasValue());
    CHECK_FALSE(missing.Value());
    Tests::RequireActionableError(LoadPackagedMetalBaselineLighting(missingRoot), BaselineLightingAssetErrors::Unavailable);
    CHECK_FALSE(std::filesystem::exists(missingRoot));
}

TEST_CASE("Metal baseline inventory rejects each missing pair while retaining every other byte", "[editor][baseline-assets]") {
    Bundle bundle;
    CompleteInventory(bundle);
    const auto index = GENERATE(0U, 1U, 2U);
    const auto suffix = GENERATE(0U, 1U);
    REQUIRE(std::filesystem::remove(bundle.File(index, suffix)));
    Tests::RequireActionableError(HasPackagedMetalBaselineLightingInputs(bundle.root), BaselineLightingAssetErrors::Unavailable);
    Tests::RequireActionableError(LoadPackagedMetalBaselineLighting(bundle.root), BaselineLightingAssetErrors::Unavailable);
    CHECK_FALSE(std::filesystem::exists(bundle.File(index, suffix)));
    for (std::size_t i = 0; i < 3; ++i)
        for (std::size_t s = 0; s < 2; ++s)
            if (i != index || s != suffix)
                CHECK(Read(bundle.File(i, s)) == InvalidBytes);
}

TEST_CASE("Metal baseline inventory distinguishes present ordinary files from valid shader data", "[editor][baseline-assets]") {
    Bundle bundle;
    CompleteInventory(bundle);
    const auto present = HasPackagedMetalBaselineLightingInputs(bundle.root);
    REQUIRE(present.HasValue());
    CHECK(present.Value());
    const auto malformed = LoadPackagedMetalBaselineLighting(bundle.root);
    REQUIRE(malformed.HasError());
    CHECK_FALSE(malformed.ErrorValue().code.Value().empty());
    CHECK_FALSE(malformed.ErrorValue().message.empty());
    RequireUnchanged(bundle);
}

TEST_CASE("Metal baseline inventory rejects isolated late entries instead of treating partial data as absent",
          "[editor][baseline-assets]") {
    Bundle bundle;
    Write(bundle.File(2, 1));
    Tests::RequireActionableError(HasPackagedMetalBaselineLightingInputs(bundle.root), BaselineLightingAssetErrors::Unavailable);
    Tests::RequireActionableError(LoadPackagedMetalBaselineLighting(bundle.root), BaselineLightingAssetErrors::Unavailable);
    CHECK(Read(bundle.File(2, 1)) == InvalidBytes);
}

TEST_CASE("Metal baseline cold reads enforce actual payload and metadata byte bounds without truncating files",
          "[editor][baseline-assets]") {
    Bundle bundle;
    CompleteInventory(bundle);
    const auto suffix = GENERATE(0U, 1U);
    const std::uintmax_t maximum = suffix == 0 ? 16U * 1024U * 1024U : 1024U * 1024U;
    const auto file = bundle.File(0, suffix);
    std::filesystem::resize_file(file, maximum + 1);
    const auto present = HasPackagedMetalBaselineLightingInputs(bundle.root);
    REQUIRE(present.HasValue());
    CHECK(present.Value());
    Tests::RequireActionableError(LoadPackagedMetalBaselineLighting(bundle.root), BaselineLightingAssetErrors::Unavailable);
    CHECK(std::filesystem::file_size(file) == maximum + 1);
    std::ifstream input(file, std::ios::binary);
    std::string prefix(InvalidBytes.size(), '\0');
    input.read(prefix.data(), static_cast<std::streamsize>(prefix.size()));
    REQUIRE(input.good());
    CHECK(prefix == InvalidBytes);
    CHECK(Read(bundle.File(0, 1 - suffix)) == InvalidBytes);
}

TEST_CASE("Metal baseline loading rejects symbolic leaf inputs and preserves their external targets", "[editor][baseline-assets]") {
    Bundle bundle;
    CompleteInventory(bundle);
    const auto suffix = GENERATE(0U, 1U);
    const auto external = bundle.root / "external canary";
    Write(external, "retained external bytes");
    REQUIRE(std::filesystem::remove(bundle.File(0, suffix)));
    std::filesystem::create_symlink(external, bundle.File(0, suffix));
    Tests::RequireActionableError(HasPackagedMetalBaselineLightingInputs(bundle.root), BaselineLightingAssetErrors::Unavailable);
    Tests::RequireActionableError(LoadPackagedMetalBaselineLighting(bundle.root), BaselineLightingAssetErrors::Unavailable);
    CHECK(std::filesystem::is_symlink(bundle.File(0, suffix)));
    CHECK(Read(external) == "retained external bytes");
}

TEST_CASE("Metal baseline loading rejects symbolic package directories including a trailing separator", "[editor][baseline-assets]") {
    Bundle bundle;
    CompleteInventory(bundle);
    const auto alias = bundle.root / "package alias";
    std::filesystem::create_directory_symlink(bundle.root, alias);
    const auto root = std::filesystem::path{alias.string() + "/"};
    Tests::RequireActionableError(HasPackagedMetalBaselineLightingInputs(root), BaselineLightingAssetErrors::Unavailable);
    Tests::RequireActionableError(LoadPackagedMetalBaselineLighting(root), BaselineLightingAssetErrors::Unavailable);
    CHECK(std::filesystem::is_symlink(alias));
    RequireUnchanged(bundle);
}

TEST_CASE("Metal baseline inventory rejects nonregular leaves and directory components without replacement", "[editor][baseline-assets]") {
    Bundle bundle;
    SECTION("directory in place of payload") {
        REQUIRE(std::filesystem::create_directory(bundle.File(0, 0)));
        Tests::RequireActionableError(HasPackagedMetalBaselineLightingInputs(bundle.root), BaselineLightingAssetErrors::Unavailable);
        CHECK(std::filesystem::is_directory(bundle.File(0, 0)));
    }
    SECTION("ordinary file in place of shader directory") {
        REQUIRE(std::filesystem::remove(bundle.root / "shaders/editor"));
        Write(bundle.root / "shaders/editor");
        Tests::RequireActionableError(HasPackagedMetalBaselineLightingInputs(bundle.root), BaselineLightingAssetErrors::Unavailable);
        CHECK(Read(bundle.root / "shaders/editor") == InvalidBytes);
    }
    Tests::RequireActionableError(LoadPackagedMetalBaselineLighting(bundle.root), BaselineLightingAssetErrors::Unavailable);
}

TEST_CASE("Metal baseline loader rejects relative roots and exposes exactly three raster program borrows", "[editor][baseline-assets]") {
    Tests::RequireActionableError(HasPackagedMetalBaselineLightingInputs("relative baseline"), BaselineLightingAssetErrors::Unavailable);
    Tests::RequireActionableError(LoadPackagedMetalBaselineLighting("relative baseline"), BaselineLightingAssetErrors::Unavailable);
    EditorBaselineLightingAssets assets;
    const auto views = assets.ProgramViews();
    REQUIRE(views.size() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(views[i].id.value == i + 1);
        CHECK(views[i].manifest == &assets.manifests[i]);
        CHECK(views[i].compiled == &assets.programs[i]);
    }
    const auto target = EditorBaselineLightingMetalTarget();
    CHECK(target.backend == Render::ShaderTargetBackend::Metal);
    CHECK(target.payloadFormat == Render::ShaderPayloadFormat::MetalLibrary24);
    CHECK_FALSE(target.supportsCompute);
    CHECK_FALSE(target.supportsStorageResources);
}
