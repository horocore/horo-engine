#include "FileManagerNative.h"
#include "Horo/Platform/FileManager.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace Horo {
    namespace {
        /** @brief Owns an isolated fixture with native Unicode paths, without launching a desktop application. */
        struct FileManagerFixture {
            std::filesystem::path root =
                std::filesystem::temp_directory_path() /
                ("horo-file-manager-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            std::filesystem::path directory = root / std::filesystem::path{u8"Assets \u00fc # % & ' folder"};
            std::filesystem::path file = directory / std::filesystem::path{u8"\u0131 script #?&% ' $(name).lua"};

            FileManagerFixture() {
#if defined(_WIN32)
                // A question mark is URI syntax on POSIX but is not a legal Windows filename.
                file = directory / std::filesystem::path{u8"\u0131 script #&% ' $(name).lua"};
#endif
                std::filesystem::create_directories(directory);
                std::ofstream stream{file};
                stream << "return {}";
                REQUIRE(stream.good());
            }

            ~FileManagerFixture() {
                std::error_code ignored;
                std::filesystem::remove_all(root, ignored);
            }
        };
    }  // namespace

    TEST_CASE("File manager keeps native Unicode file and directory identities", "[platform][file-manager]") {
        FileManagerFixture fixture;
        const auto directory = FileManagerInternal::PrepareTarget(fixture.directory);
        REQUIRE(directory.HasValue());
        CHECK(directory.Value().directory);
        CHECK(directory.Value().path == fixture.directory);
        const auto file = FileManagerInternal::PrepareTarget(fixture.file);
        REQUIRE(file.HasValue());
        CHECK_FALSE(file.Value().directory);
        CHECK(file.Value().path == fixture.file);
#if defined(_WIN32)
        const auto forwardSlashes = FileManagerInternal::PrepareTarget(std::filesystem::path{fixture.directory.generic_u8string()});
        REQUIRE(forwardSlashes.HasValue());
        CHECK(forwardSlashes.Value().path.native().find(L'/') == std::wstring::npos);
#endif
    }

    TEST_CASE("File manager rejects invalid paths before desktop dispatch", "[platform][file-manager]") {
        FileManagerFixture fixture;
        auto truncated = fixture.file.native();
        truncated.push_back(std::filesystem::path::value_type{});
        truncated += std::filesystem::path{"extra"}.native();
        for (const auto &path : {std::filesystem::path{}, std::filesystem::path{"relative.lua"}, fixture.directory / "missing.lua",
                                 std::filesystem::path{truncated}}) {
            const auto result = RevealInFileManager(path);
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == FileManagerErrors::InvalidTarget.code.Value());
        }
    }

    TEST_CASE("File manager refuses linked targets", "[platform][file-manager]") {
        FileManagerFixture fixture;
        const auto link = fixture.directory / "linked.lua";
        std::error_code error;
        std::filesystem::create_symlink(fixture.file, link, error);
        if (error)
            SKIP("Creating a symlink requires host privileges: " << error.message());
        const auto result = RevealInFileManager(link);
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == FileManagerErrors::InvalidTarget.code.Value());
    }

    TEST_CASE("File manager URI encoding keeps path bytes out of URI syntax", "[platform][file-manager]") {
        const auto path = std::filesystem::path{u8"/tmp/\u00fc \u0131/#?%&'\";$(x)/file"};
        CHECK(FileManagerInternal::DirectoryUri(path) == "file:///tmp/%C3%BC%20%C4%B1/%23%3F%25%26%27%22%3B%24%28x%29/file");
        CHECK(FileManagerInternal::DirectoryUri(std::filesystem::path{"/"}) == "file:///");
        CHECK(FileManagerInternal::DirectoryUri(std::filesystem::path{"/tmp/a-z_A.Z~09"}) == "file:///tmp/a-z_A.Z~09");
    }

#if defined(HORO_FILE_MANAGER_HEADLESS)
    TEST_CASE("Headless file manager reports unavailable for a valid target", "[platform][file-manager]") {
        FileManagerFixture fixture;
        const auto result = RevealInFileManager(fixture.file);
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == FileManagerErrors::Unavailable.code.Value());
    }
#endif
}  // namespace Horo
