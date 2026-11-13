#include "AtomicShaderCookPackage.h"
#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"
#include "support/TypedIdentityTestSupport.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {
    using namespace Horo;
    using namespace Horo::Editor::Offline;
    namespace fs = std::filesystem;

    /** @brief Owns only a successfully created fresh test directory; existing paths are never removed to claim it. */
    struct OutputRoot {
        fs::path path;

        OutputRoot() {
            static std::atomic<unsigned> next{};
            path = fs::temp_directory_path() /
                   ("horo atomic cook " + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                    std::to_string(next++));
            REQUIRE(fs::create_directory(path));
        }

        ~OutputRoot() {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
    };

    /** @brief Writes one real finite filesystem marker under the owned test root. */
    void Write(const fs::path &path, std::string_view bytes) {
        std::ofstream stream(path, std::ios::binary);
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        stream.close();
        REQUIRE(stream.good());
    }

    /** @brief Reads exact marker bytes to prove preservation rather than only path existence. */
    std::string Read(const fs::path &path) {
        std::ifstream stream(path, std::ios::binary);
        REQUIRE(stream.good());
        return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    }

    /** @brief Builds a real source directory and unrelated sibling canaries for publication failure tests. */
    struct DirectoryFixture {
        OutputRoot root;
        fs::path source = root.path / "owned source";
        fs::path destination = root.path / "published output";
        fs::path foreign = root.path / "foreign candidate";

        DirectoryFixture() {
            REQUIRE(fs::create_directory(source));
            REQUIRE(fs::create_directory(foreign));
            Write(source / "payload.bin", "complete source");
            Write(foreign / "marker.bin", "foreign source");
            Write(root.path / "sibling.bin", "sibling bytes");
        }

        /** @brief Checks every source and foreign byte after a rejected rename. */
        void Preserved() const {
            CHECK(Read(source / "payload.bin") == "complete source");
            CHECK(Read(foreign / "marker.bin") == "foreign source");
            CHECK(Read(root.path / "sibling.bin") == "sibling bytes");
        }
    };

    /** @brief Mutable observation only; the validator has no package mutation authority. */
    struct ValidationCounter {
        mutable unsigned calls{};
    };

    /** @brief Records whether the complete-family validator was ever reached. */
    Result<void> ValidationCanary(const fs::path &, const void *context) {
        ++static_cast<const ValidationCounter *>(context)->calls;
        return Result<void>::Success();
    }

    /** @brief Deliberately uncompiled records distinguish early inventory rejection from later compiler-record validation.
     * @details This is a negative admission canary, never a fabricated compiled shader, authenticated host or native qualification.
     */
    struct InventoryFixture {
        OutputRoot root;
        Render::ShaderManifest manifest;
        Render::ShaderCompilationBatch uncompiled;
        std::array<AtomicShaderCookProgram, 1> programs{{{"shaders/program", &manifest, &uncompiled}}};
        std::array<std::uint8_t, 1> byte{{7}};
        ValidationCounter validated;

        /** @brief Requires inventory rejection before any staging write, compiled-record read or validator call. */
        void Reject(std::span<const AtomicShaderCookAuxiliaryFile> files) {
            const AtomicShaderCookPackage family{.programs = programs,
                                                 .privateCandidatePrefix = ".atomic-package-",
                                                 .provenanceFilename = "provenance.json",
                                                 .validateWritten = ValidationCanary,
                                                 .context = &validated,
                                                 .auxiliaryFiles = files};
            const auto outcome = PublishAtomicShaderCookPackage(root.path / "new output", family, {});
            Tests::RequireActionableError(outcome, Render::ShaderCompilerPipelineErrors::ScratchIoFailed);
            CHECK(validated.calls == 0);
            CHECK(fs::is_empty(root.path));
        }
    };
}  // namespace

TEST_CASE("Atomic shader directory publication succeeds with actual Unicode and space paths", "[editor][shader-cook][atomic]") {
    OutputRoot root;
    const auto source = root.path / fs::path{u8"kaynak ü 目录"};
    const auto destination = root.path / fs::path{u8"çıktı ğ 空間"};
    REQUIRE(fs::create_directory(source));
    REQUIRE(fs::create_directory(source / "nested folder"));
    Write(source / "nested folder" / fs::path{u8"renk 色.bin"}, "actual complete generation");
    const auto result = PublishShaderCookDirectory(source, destination);
    REQUIRE(result.HasValue());
    CHECK_FALSE(fs::exists(source));
    CHECK(Read(destination / "nested folder" / fs::path{u8"renk 色.bin"}) == "actual complete generation");
    CHECK(std::distance(fs::directory_iterator(root.path), fs::directory_iterator{}) == 1);
}

TEST_CASE("Atomic shader directory publication never replaces an existing empty destination", "[editor][shader-cook][atomic]") {
    DirectoryFixture fixture;
    REQUIRE(fs::create_directory(fixture.destination));
    Tests::RequireActionableError(PublishShaderCookDirectory(fixture.source, fixture.destination),
                                  Render::ShaderCompilerPipelineErrors::ScratchIoFailed);
    CHECK(fs::is_directory(fixture.destination));
    CHECK(fs::is_empty(fixture.destination));
    fixture.Preserved();
}

TEST_CASE("Atomic shader directory publication preserves an existing populated destination", "[editor][shader-cook][atomic]") {
    DirectoryFixture fixture;
    REQUIRE(fs::create_directory(fixture.destination));
    Write(fixture.destination / "original.bin", "original destination");
    Tests::RequireActionableError(PublishShaderCookDirectory(fixture.source, fixture.destination),
                                  Render::ShaderCompilerPipelineErrors::ScratchIoFailed);
    CHECK(Read(fixture.destination / "original.bin") == "original destination");
    CHECK_FALSE(fs::exists(fixture.destination / "payload.bin"));
    fixture.Preserved();
}

TEST_CASE("Atomic shader directory publication preserves live and dangling destination symlinks", "[editor][shader-cook][atomic]") {
    DirectoryFixture fixture;
    auto target = fixture.foreign;
    SECTION("live directory link") {}
    SECTION("dangling directory link") {
        target = fixture.root.path / "missing target";
    }
    std::error_code linkError;
    fs::create_directory_symlink(target, fixture.destination, linkError);
#if defined(_WIN32)
    if (linkError == std::error_code{ERROR_PRIVILEGE_NOT_HELD, std::system_category()})
        SKIP("Windows did not grant directory-symlink creation; this case requires a symlink-capable host.");
#endif
    INFO("Directory symlink fixture: " << linkError.message());
    REQUIRE_FALSE(linkError);
    Tests::RequireActionableError(PublishShaderCookDirectory(fixture.source, fixture.destination),
                                  Render::ShaderCompilerPipelineErrors::ScratchIoFailed);
    CHECK(fs::is_symlink(fs::symlink_status(fixture.destination)));
    CHECK(fs::read_symlink(fixture.destination) == target);
    fixture.Preserved();
}

TEST_CASE("Atomic shader directory publication preserves an existing destination file", "[editor][shader-cook][atomic]") {
    DirectoryFixture fixture;
    Write(fixture.destination, "protected file");
    Tests::RequireActionableError(PublishShaderCookDirectory(fixture.source, fixture.destination),
                                  Render::ShaderCompilerPipelineErrors::ScratchIoFailed);
    CHECK(Read(fixture.destination) == "protected file");
    fixture.Preserved();
}

TEST_CASE("Atomic shader directory publication leaves source intact when destination parent is absent", "[editor][shader-cook][atomic]") {
    DirectoryFixture fixture;
    const auto absent = fixture.root.path / "absent parent";
    Tests::RequireActionableError(PublishShaderCookDirectory(fixture.source, absent / "output"),
                                  Render::ShaderCompilerPipelineErrors::ScratchIoFailed);
    CHECK_FALSE(fs::exists(absent));
    fixture.Preserved();
}

TEST_CASE("Atomic shader directory publication reports absent source without creating output", "[editor][shader-cook][atomic]") {
    DirectoryFixture fixture;
    Tests::RequireActionableError(PublishShaderCookDirectory(fixture.root.path / "missing source", fixture.destination),
                                  Render::ShaderCompilerPipelineErrors::ScratchIoFailed);
    CHECK_FALSE(fs::exists(fixture.destination));
    fixture.Preserved();
}

TEST_CASE("Atomic package inventory rejects portable traversal absolute and alternate-separator paths before staging",
          "[editor][shader-cook][atomic][inventory]") {
    InventoryFixture fixture;
    for (const std::string_view path : {"../escape.bin", "aux/../escape.bin", "./aux.bin", "/absolute.bin", "aux//double.bin",
                                        "aux/./dot.bin", "aux\\backslash.bin", "C:/drive.bin"}) {
        const std::array files{AtomicShaderCookAuxiliaryFile{path, fixture.byte}};
        INFO(path);
        fixture.Reject(files);
    }
}

TEST_CASE("Atomic package inventory rejects reserved and canonical artifact collisions case insensitively",
          "[editor][shader-cook][atomic][inventory]") {
    InventoryFixture fixture;
    for (const std::string_view path : {"shader-host-catalog.json", "SHADER-HOST-CATALOG.SIGNATURE.JSON", "PrOvEnAnCe.JsOn",
                                        "shaders/program.horoshdr", "SHADERS/PROGRAM.REFLECTION.JSON", "shader-host-catalog.json/child"}) {
        const std::array files{AtomicShaderCookAuxiliaryFile{path, fixture.byte}};
        INFO(path);
        fixture.Reject(files);
    }
}

TEST_CASE("Atomic package inventory rejects auxiliary duplicate and parent-file collisions in either order",
          "[editor][shader-cook][atomic][inventory]") {
    InventoryFixture fixture;
    for (const auto &[a, b] : std::array{std::pair{"aux.bin", "AUX.BIN"}, std::pair{"aux", "aux/child.bin"},
                                         std::pair{"aux/child.bin", "aux"}, std::pair{"shaders", "safe.bin"}}) {
        const std::array files{AtomicShaderCookAuxiliaryFile{a, fixture.byte}, AtomicShaderCookAuxiliaryFile{b, fixture.byte}};
        fixture.Reject(files);
    }
}

TEST_CASE("Atomic package inventory rejects empty payloads and excessive auxiliary counts before staging",
          "[editor][shader-cook][atomic][inventory]") {
    InventoryFixture fixture;
    const std::array empty{AtomicShaderCookAuxiliaryFile{"aux.bin", {}}};
    fixture.Reject(empty);
    std::array<AtomicShaderCookAuxiliaryFile, 17> tooMany;
    for (auto &file : tooMany)
        file = {"aux.bin", fixture.byte};
    fixture.Reject(tooMany);
}

TEST_CASE("Atomic package inventory checks real individual and aggregate byte limits before staging",
          "[editor][shader-cook][atomic][inventory]") {
    InventoryFixture fixture;
    SECTION("one actual buffer exceeds the per-file limit") {
        const std::vector<std::uint8_t> data(32U * 1024U * 1024U + 1, 7);
        const std::array files{AtomicShaderCookAuxiliaryFile{"aux.bin", data}};
        fixture.Reject(files);
    }
    SECTION("three actual borrowed views exceed aggregate limit without tripling allocated storage") {
        const std::vector<std::uint8_t> data(32U * 1024U * 1024U, 7);
        const std::array files{AtomicShaderCookAuxiliaryFile{"a.bin", data}, AtomicShaderCookAuxiliaryFile{"b.bin", data},
                               AtomicShaderCookAuxiliaryFile{"c.bin", data}};
        fixture.Reject(files);
    }
}

TEST_CASE("Atomic package output reserves private candidate names without changing siblings", "[editor][shader-cook][atomic][inventory]") {
    OutputRoot root;
    for (const auto name : {".atomic-package-0", ".ATOMIC-PACKAGE-0", ".AtOmIc-PaCkAgE-other"}) {
        Tests::RequireActionableError(ValidateAtomicShaderCookDestination(root.path / name, ".atomic-package-"),
                                      Render::ShaderCompilerPipelineErrors::ScratchIoFailed);
        CHECK(fs::is_empty(root.path));
    }
}
