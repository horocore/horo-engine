#include "Horo/Packages/PackageArchive.h"
#include "PackageArchiveTestSupport.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <miniz.h>
#include <nlohmann/json.hpp>

namespace {
    using Json = nlohmann::json;
    using Horo::Packages::ValidatedPackageArchive;

    struct File {
        std::string name;
        std::string content;
    };

    std::vector<File> Files() {
        return {{"horo-package.toml", "schemaVersion = 1\n"}, {"assets/şölen.txt", "hello"}};
    }

    Json Inventory(const std::vector<File> &files) {
        Json entries = Json::array();
        for (const auto &file : files) {
            if (!file.name.ends_with('/')) {
                entries.push_back(Horo::Tests::Packages::FileInventoryEntry(file.name, file.content));
            }
        }
        return {{"schemaVersion", 1}, {"files", entries}};
    }

    std::vector<std::byte> Archive(std::vector<File> files, const std::optional<Json> &inventory = std::nullopt,
                                   const std::string_view localExtra = {}, const std::string_view centralExtra = {}) {
        files.push_back({"files.manifest.json", inventory.value_or(Inventory(files)).dump()});
        mz_zip_archive zip{};
        REQUIRE(mz_zip_writer_init_heap(&zip, 0, 0));
        for (const auto &file : files) {
            REQUIRE(mz_zip_writer_add_mem_ex_v2(&zip, file.name.c_str(), file.content.data(), file.content.size(), nullptr, 0,
                                                MZ_BEST_COMPRESSION, 0, 0, nullptr, localExtra.data(),
                                                static_cast<mz_uint>(localExtra.size()), centralExtra.data(),
                                                static_cast<mz_uint>(centralExtra.size())));
        }
        return Horo::Tests::Packages::FinalizeArchive(zip);
    }

    std::size_t CentralOffset(const std::vector<std::byte> &bytes, const mz_uint index) {
        mz_zip_archive zip{};
        REQUIRE(mz_zip_reader_init_mem(&zip, bytes.data(), bytes.size(), 0));
        mz_zip_archive_file_stat stat{};
        REQUIRE(mz_zip_reader_file_stat(&zip, index, &stat));
        const auto offset = zip.m_central_directory_file_ofs + stat.m_central_dir_ofs;
        REQUIRE(mz_zip_reader_end(&zip));
        return static_cast<std::size_t>(offset);
    }

    void Write32(std::vector<std::byte> &bytes, const std::size_t offset, const std::uint32_t value) {
        for (unsigned index = 0; index < 4; ++index) {
            bytes.at(offset + index) = static_cast<std::byte>((value >> (8 * index)) & 0xff);
        }
    }

    TEST_CASE("Package archive verifies complete contents and retains an immutable snapshot", "[packages][archive]") {
        auto files = Files();
        files.push_back({"assets/", ""});
        files.push_back({"empty.bin", ""});
        auto archive = Archive(files);
        const auto digest = Horo::ComputeSha256(archive);
        const auto result = ValidatedPackageArchive::Verify(archive);
        REQUIRE(result.HasValue());
        CHECK(result.Value().Manifest().Entries().size() == 3);
        CHECK(result.Value().Digest() == digest);
        constexpr std::string_view packageManifest = "schemaVersion = 1\n";
        CHECK(result.Value().PackageManifestDigest() == Horo::ComputeSha256(std::as_bytes(std::span{packageManifest})));
        CHECK(std::ranges::equal(result.Value().Bytes(), archive));
        std::ranges::fill(archive, std::byte{0});
        CHECK(Horo::ComputeSha256(result.Value().Bytes()) == digest);
    }

    TEST_CASE("Package archive rejects links and special file attributes", "[packages][archive]") {
        constexpr std::array<unsigned, 5> modes{0120777, 0010600, 0020600, 0104755, 0140600};
        for (const auto mode : modes) {
            auto archive = Archive(Files());
            Write32(archive, CentralOffset(archive, 0) + 38, mode << 16);
            CHECK(ValidatedPackageArchive::Verify(archive).HasError());
        }
        auto archive = Archive(Files());
        Write32(archive, CentralOffset(archive, 0) + 38, 0x400);
        CHECK(ValidatedPackageArchive::Verify(archive).HasError());
    }

    TEST_CASE("Package archive rejects link and alternate-path extra fields in both headers", "[packages][archive]") {
        const std::array<std::string_view, 4> extras{std::string_view{"\x0d\x00\x00\x00", 4}, std::string_view{"\x6e\x75\x00\x00", 4},
                                                     std::string_view{"\x75\x70\x00\x00", 4}, std::string_view{"\x0a\x00\x00\x00", 4}};
        for (const auto extra : extras) {
            CHECK(ValidatedPackageArchive::Verify(Archive(Files(), std::nullopt, extra)).HasError());
            CHECK(ValidatedPackageArchive::Verify(Archive(Files(), std::nullopt, {}, extra)).HasError());
        }
        const std::string_view timestamp{"\x55\x54\x01\x00\x00", 5};
        CHECK(ValidatedPackageArchive::Verify(Archive(Files(), std::nullopt, timestamp, timestamp)).HasValue());
    }

    TEST_CASE("Package archive verifies long full names and compressed payload CRC", "[packages][archive]") {
        auto files = Files();
        files.push_back({std::string(250, 'a') + "/" + std::string(250, 'b') + "/" + std::string(250, 'c'), "content"});
        CHECK(ValidatedPackageArchive::Verify(Archive(files)).HasValue());
        auto archive = Archive(Files());
        Write32(archive, CentralOffset(archive, 0) + 16, 0);
        CHECK(ValidatedPackageArchive::Verify(archive).HasError());
    }

    TEST_CASE("Package archive rejects duplicate paths and prefix aliases", "[packages][archive]") {
        const std::array aliases{"horo-package.toml", "HORO-PACKAGE.TOML", "ASSETS/other", "assets"};
        for (const auto *alias : aliases) {
            auto files = Files();
            const auto inventory = Inventory(files);
            files.push_back({alias, "rogue"});
            CHECK(ValidatedPackageArchive::Verify(Archive(files, inventory)).HasError());
        }
        auto files = Files();
        files.push_back({"a/../outside", "rogue"});
        CHECK(ValidatedPackageArchive::Verify(Archive(files)).HasError());
    }

    TEST_CASE("Package archive requires exact hashes sizes modes and complete inventory", "[packages][archive]") {
        using Mutate = void (*)(Json &);
        const std::array<Mutate, 5> mutations{
            +[](Json &inventory) {
            inventory["files"][0]["sha256"] = Horo::FormatSha256(Horo::ComputeSha256({}));
        },
            +[](Json &inventory) {
            inventory["files"][0]["size"] = 1;
        },
            +[](Json &inventory) {
            inventory["files"][0]["executable"] = true;
        },
            +[](Json &inventory) {
            inventory["files"].erase(0);
        },
            +[](Json &inventory) {
            inventory["files"][0]["path"] = "missing";
        },
        };
        for (const auto mutate : mutations) {
            auto inventory = Inventory(Files());
            mutate(inventory);
            const auto result = ValidatedPackageArchive::Verify(Archive(Files(), inventory));
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == "packages.archive.inventory_mismatch");
        }
        auto files = Files();
        files.erase(files.begin());
        CHECK(ValidatedPackageArchive::Verify(Archive(files)).HasError());
        CHECK(ValidatedPackageArchive::Verify(Archive(Files(), Json::array())).HasError());
    }

    TEST_CASE("Package archive bounds compressed expanded and manifest sizes before decompression", "[packages][archive]") {
        using Horo::Packages::PackageValidationLimits;
        const auto archive = Archive(Files());
        const std::array limits{
            PackageValidationLimits{.archiveBytes = archive.size() - 1},
            PackageValidationLimits{.manifestBytes = 1},
            PackageValidationLimits{.fileBytes = 1},
            PackageValidationLimits{.expandedBytes = 1},
            PackageValidationLimits{.entries = 2},
        };
        for (const auto &limit : limits) {
            const auto result = ValidatedPackageArchive::Verify(archive, limit);
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == "packages.validation.limit");
        }
        auto bomb = Files();
        bomb.push_back({"bomb", std::string(100'000, 'x')});
        CHECK(ValidatedPackageArchive::Verify(Archive(bomb), {.expandedBytes = 10'000}).HasError());
    }

    TEST_CASE("Package archive rejects malformed local headers including empty entries", "[packages][archive]") {
        CHECK(ValidatedPackageArchive::Verify({}).HasError());
        auto files = Files();
        files[0].content.clear();
        const auto original = Archive(files);
        for (const std::size_t offset : {std::size_t{0}, std::size_t{6}, std::size_t{8}, std::size_t{26}, std::size_t{30}}) {
            auto corrupted = original;
            corrupted[offset] ^= std::byte{1};
            CHECK(ValidatedPackageArchive::Verify(corrupted).HasError());
        }
        auto truncated = original;
        truncated.resize(20);
        CHECK(ValidatedPackageArchive::Verify(truncated).HasError());
    }
}  // namespace
