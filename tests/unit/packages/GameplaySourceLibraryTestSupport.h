#pragma once

#include "Horo/Packages/GameplaySourceLibrary.h"
#include "Horo/Packages/PackageRequest.h"
#include "PackageArchiveTestSupport.h"

#include <format>
#include <fstream>

namespace Horo::Tests::GameplayLibraries {
    using Horo::Packages::GameplaySourceLibrary;
    using Horo::Packages::GameplaySourceLibrarySet;
    using Horo::Packages::HoroPackageId;
    using Horo::Packages::HoroPackageSourceId;
    using Horo::Packages::PackageInstallService;
    using Horo::Packages::PackageLockArtifact;
    using Horo::Packages::PackagePath;
    using Horo::Packages::PackageResolutionPlan;
    using Horo::Packages::PackageRestoreGraph;
    using Horo::Packages::PackageVersion;
    using Horo::Packages::ValidatedPackageArchive;
    using Horo::Packages::ValidatedPackageLockfileV1;
    using Horo::Packages::ValidatedPackageRequest;
    using Horo::Packages::VerifiedPackageInstallRecord;

    inline std::filesystem::path Utf8Path(const std::string_view value) {
        return std::filesystem::path{std::u8string{value.begin(), value.end()}};
    }

    template <class T> T Parse(const std::string_view text) {
        auto result = T::Parse(text);
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    inline void Write(const std::filesystem::path &path, const std::string_view text) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream stream{path, std::ios::binary | std::ios::trunc};
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        REQUIRE(stream.good());
    }

    inline std::string Read(const std::filesystem::path &path) {
        std::ifstream stream{path, std::ios::binary};
        REQUIRE(stream.good());
        return {std::istreambuf_iterator<char>{stream}, {}};
    }

    /** @brief Real verified ZIP inputs include a hostile unused build script to detect accidental package CMake execution. */
    inline std::shared_ptr<const ValidatedPackageArchive> Archive(const std::size_t extraFiles = 0) {
        std::vector<std::pair<std::string, std::string>>
            files{{"horo-package.toml", "schemaVersion = 1\n"},
                  {"libraries/matematik_ü/Answer.cpp", "#include \"Answer.h\"\nint Example::Answer() { return 42; }\n"},
                  {"libraries/matematik_ü/Other.cpp", "int OtherLibraryFunction() { return 7; }\n"},
                  {"libraries/matematik_ü/Answer.h", "#pragma once\nnamespace Example { int Answer(); }\n"},
                  {"libraries/matematik_ü/CMakeLists.txt", "message(FATAL_ERROR \"Package build scripts must never execute\")\n"}};
        for (std::size_t index = 0; index < extraFiles; ++index)
            files.emplace_back(std::format("libraries/matematik_ü/Extra{}.h", index), "\n");
        nlohmann::json entries = nlohmann::json::array();
        for (const auto &[name, content] : files) {
            auto entry = Horo::Tests::Packages::FileInventoryEntry(name, content);
            if (name.starts_with("libraries/matematik_ü/"))
                entry["contributionRoot"] = "libraries/matematik_ü";
            entries.push_back(std::move(entry));
        }
        const auto inventory = nlohmann::json{{"schemaVersion", 1}, {"files", std::move(entries)}}.dump();
        mz_zip_archive zip{};
        REQUIRE(mz_zip_writer_init_heap(&zip, 0, 0));
        for (const auto &[name, content] : files)
            REQUIRE(mz_zip_writer_add_mem(&zip, name.c_str(), content.data(), content.size(), MZ_BEST_COMPRESSION));
        REQUIRE(mz_zip_writer_add_mem(&zip, "files.manifest.json", inventory.data(), inventory.size(), MZ_BEST_COMPRESSION));
        auto verified = ValidatedPackageArchive::Verify(Horo::Tests::Packages::FinalizeArchive(zip));
        REQUIRE(verified.HasValue());
        return std::make_shared<const ValidatedPackageArchive>(std::move(verified).Value());
    }

    /** @brief Owns actual portable intent, generated lock, immutable archive and a durably installed graph. */
    struct Fixture final {
        Horo::Tests::Packages::TemporaryDirectory temporary{"horo gameplay library spaces"};
        std::filesystem::path root{std::filesystem::canonical(temporary.Path())};
        NativeDurableFileSystem files;
        HoroPackageId id{Parse<HoroPackageId>("com.example.math")};
        PackagePath contribution{Parse<PackagePath>("libraries/matematik_ü")};
        std::shared_ptr<const ValidatedPackageArchive> archive;
        std::shared_ptr<const VerifiedPackageInstallRecord> installed;
        std::shared_ptr<PackageRestoreGraph> graph;
        std::string intent;
        std::string lock;

        explicit Fixture(const std::size_t extraFiles = 0, const std::string_view range = "1.0.0") : archive(Archive(extraFiles)) {
            intent = nlohmann::json{{"sources", {{"official", {{"kind", "public-registry"}, {"registry", "official"}}}}},
                                    {"dependencies", {{id.Value(), {{"source", "official"}, {"version", range}}}}}}
                         .dump();
            const auto request = ValidatedPackageRequest::Parse(intent);
            REQUIRE(request.HasValue());
            const auto version = Parse<PackageVersion>("1.0.0");
            const auto source = Parse<HoroPackageSourceId>("official");
            const PackageResolutionPlan plan{{{id, version, source, archive->Digest(), {}}}};
            const PackageLockArtifact
                evidence{id, version, source, archive->Digest(), archive->PackageManifestDigest(), archive->Manifest().Digest(), 1, {}, {}};
            const auto generated =
                ValidatedPackageLockfileV1::Generate(plan, std::span{&id, 1U}, request.Value().Digest(), std::span{&evidence, 1U});
            REQUIRE(generated.HasValue());
            lock = generated.Value().SerializeCanonical();
            graph =
                std::make_shared<PackageRestoreGraph>(PackageRestoreGraph{request.Value().Digest(),
                                                                          {"linux", "x64", "source-fixture"},
                                                                          {{generated.Value().Packages().front(), archive, false, {}}}});
            auto installer = PackageInstallService::Create(files, root);
            REQUIRE(installer.HasValue());
            auto authority = std::move(installer).Value();
            REQUIRE(authority.Install(graph, {}).HasValue());
            installed = authority.InstalledRecord();
            Write(root / ".horo/packages.json", intent);
            Write(root / ".horo/packages.lock", lock);
            Write(root / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.25)\nproject(LibraryFixture LANGUAGES CXX)\n");
        }

        [[nodiscard]] GameplaySourceLibrary Select() const {
            auto selected = GameplaySourceLibrary::Select(installed, id, contribution,
                                                          {Parse<PackagePath>("libraries/matematik_ü/Answer.cpp")}, archive->Digest());
            REQUIRE(selected.HasValue());
            return std::move(selected).Value();
        }
    };
}  // namespace Horo::Tests::GameplayLibraries
