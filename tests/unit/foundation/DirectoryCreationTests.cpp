#include "Horo/Foundation/Platform.h"
#include "OwnedTestDirectory.h"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>

TEST_CASE("Native directory authority distinguishes parent creation from a single owned component", "[platform][filesystem]") {
    using enum Horo::DirectoryCreationMode;
    Horo::Tests::OwnedTestDirectory directory{"directory authority"};
    Horo::NativeDurableFileSystem files;
    const auto target = directory.Path() / "missing parent" / std::filesystem::path{u8"output ü"};
    CHECK(files.CreateDirectoryPath(target, Single).HasError());
    CHECK_FALSE(std::filesystem::exists(target.parent_path()));
    const auto created = files.CreateDirectoryPath(target, Parents);
    REQUIRE(created.HasValue());
    CHECK(created.Value());
    REQUIRE(std::filesystem::is_directory(target));
    for (const auto mode : {Single, Parents}) {
        const auto existing = files.CreateDirectoryPath(target, mode);
        REQUIRE(existing.HasValue());
        CHECK_FALSE(existing.Value());
    }
}

TEST_CASE("Native directory authority rejects file obstructions and invalid creation policies", "[platform][filesystem]") {
    using enum Horo::DirectoryCreationMode;
    Horo::Tests::OwnedTestDirectory directory{"directory authority obstruction"};
    Horo::NativeDurableFileSystem files;
    const auto target = directory.Path() / "protected file";
    REQUIRE(files.WriteDurable(target, {}).HasValue());
    for (const auto mode : {Single, Parents}) {
        CHECK(files.CreateDirectoryPath(target, mode).HasError());
        CHECK(std::filesystem::is_regular_file(target));
        CHECK(files.CreateDirectoryPath(target / "child", mode).HasError());
    }
    const auto invalid = directory.Path() / "must not be created";
    CHECK(files.CreateDirectoryPath(invalid, static_cast<Horo::DirectoryCreationMode>(255)).HasError());
    CHECK_FALSE(std::filesystem::exists(invalid));
}
