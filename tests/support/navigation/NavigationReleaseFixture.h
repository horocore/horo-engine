#pragma once

#include "Horo/Application/NavigationContentIntegration.h"
#include "Horo/Release/ReleaseErrors.h"
#include "Horo/Release/UpdateZipPackageProducer.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <miniz.h>

namespace Horo::Application::ContentTestSupport {
    /** @brief Preserve the actual typed cause chain in failed qualification output; no production error is translated. */
    [[nodiscard]] std::string ErrorText(const Error &error) {
        std::string text;
        for (const auto *current = &error; current; current = current->cause.Get()) {
            if (!text.empty())
                text += " <- ";
            text += current->domain.Value();
            text += "/";
            text += current->code.Value();
            text += ": ";
            text += current->message;
        }
        return text;
    }

    /** @brief Own only a new isolated test directory; paths exercise spaces and non-ASCII without host-global mutation. */
    class Directory final {
    public:
        Directory() {
            root = std::filesystem::temp_directory_path() /
                   ("horo navigation package ü " + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            REQUIRE(std::filesystem::create_directory(root));
            root = std::filesystem::canonical(root);
        }

        Directory(const Directory &) = delete;
        Directory &operator=(const Directory &) = delete;

        ~Directory() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        std::filesystem::path root;
    };

    inline void Write(const std::filesystem::path &path, const std::span<const std::uint8_t> bytes) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output{path, std::ios::binary};
        REQUIRE(output.is_open());
        output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        REQUIRE(output.good());
        output.close();
        REQUIRE(!output.fail());
    }

    /** @brief Existing chunk authority; no synthetic identities are assigned to generated tile partitions. */
    [[nodiscard]] inline Assets::AssetChunkPlan Plan(const Assets::AssetId asset, const Release::DistributionProductKind product) {
        const auto base = Assets::AssetChunkId::Parse("base");
        const auto server = Assets::AssetChunkId::Parse("server");
        REQUIRE(base.HasValue());
        REQUIRE(server.HasValue());
        std::vector<Assets::AssetChunkDefinition> chunks;
        if (product == Release::DistributionProductKind::GameDedicatedServer) {
            chunks.push_back({server.Value(), Assets::AssetChunkKind::DedicatedServer, {asset}, {}});
        } else {
            chunks.push_back({base.Value(), Assets::AssetChunkKind::Base, {asset}, {}});
        }
        auto plan = Assets::AssetChunkPlan::Create(chunks);
        REQUIRE(plan.HasValue());
        return std::move(plan).Value();
    }

    /** @brief Publish existing promoted producer bytes through the actual durable generic authority, without recooking. */
    [[nodiscard]] inline Assets::AssetCookGeneration Publish(const std::filesystem::path &root, const AssetCookTargetId &target,
                                                             const Assets::AssetId id, std::vector<std::uint8_t> bytes) {
        REQUIRE(std::filesystem::create_directories(root));
        NativeDurableFileSystem files;
        auto writer = files.TryAcquireExclusive(root / ".cook-writer.lock", "navigation package corpus");
        REQUIRE(writer.HasValue());
        const auto type = Assets::AssetTypeId::Parse(Assets::NavMeshAssetTypeName);
        REQUIRE(type.HasValue());
        const std::array entries{
            Assets::AssetCookManifestEntry{id, type.Value(), id.ToString() + ".cooked", ComputeSha256(std::as_bytes(std::span{bytes}))}};
        const std::array payloads{std::move(bytes)};
        const Assets::AssetCookPublicationPolicy policy{.files = &files,
                                                        .operationId = "10000000-0000-0000-0000-000000000001",
                                                        .writerLease = &writer.Value()};
        auto published = Assets::PublishCookGeneration(root, target, entries, payloads, {}, policy);
        REQUIRE(published.HasValue());
        REQUIRE_FALSE(published.Value().durabilityError.has_value());
        return std::move(published).Value();
    }

    [[nodiscard]] inline Release::DistributionPackageSelection Selection(const Release::DistributionProductKind product) {
        Release::DistributionArtifactIdentity identity;
        identity.product = {product, {}};
        identity.version = Release::GameProductVersion{{1, 2, 3, {}, {}}};
        identity.platform = Release::DistributionPlatform::Windows;
        identity.architecture = Release::DistributionArchitecture::X64;
        identity.build = {"nav_build_42"};
        identity.package = {"nav_package_42"};
        identity.installation = Release::DistributionInstallationId{"nav_game_42"};
        auto selection = Release::ValidateDistributionPackageSelection(identity, Release::DistributionPackageFormat::ZipArchive);
        REQUIRE(selection.HasValue());
        return std::move(selection).Value();
    }

    /** @brief Read actual ZIP content through miniz, not through a memory-provider substitute. */
    [[nodiscard]] inline std::vector<std::uint8_t> ExtractArchive(const std::filesystem::path &zip) {
        mz_zip_archive reader{};
        const auto path = zip.string();
        REQUIRE(mz_zip_reader_init_file(&reader, path.c_str(), 0));

        struct Close final {
            mz_zip_archive &reader;

            ~Close() {
                mz_zip_reader_end(&reader);
            }
        } close{reader};

        const int index = mz_zip_reader_locate_file(&reader, "assets.horo", nullptr, 0);
        REQUIRE(index >= 0);
        mz_zip_archive_file_stat stat{};
        REQUIRE(mz_zip_reader_file_stat(&reader, static_cast<mz_uint>(index), &stat));
        REQUIRE(stat.m_uncomp_size > 0);
        REQUIRE(stat.m_uncomp_size <= 8U * 1024U * 1024U);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(stat.m_uncomp_size));
        REQUIRE(mz_zip_reader_extract_to_mem(&reader, static_cast<mz_uint>(index), bytes.data(), bytes.size(), 0));
        return bytes;
    }

    struct Package final {
        std::filesystem::path stage;
        Release::ReleaseArtifactManifest manifest;
        std::vector<std::uint8_t> archive;
    };

    /** @brief Compose the real generic producer and canonical final inventory around explicit fixture executable bytes. */
    [[nodiscard]] inline Package Produce(const std::filesystem::path &root, const PreparedNavigationReleaseContent &content,
                                         const Release::DistributionProductKind product) {
        const auto stage = root / "stage";
        const std::array<std::uint8_t, 4> fixtureEntrypoint{'t', 'e', 's', 't'};
        Write(stage / "bin/game", fixtureEntrypoint);
        Write(stage / "assets.horo", content.archive);
        std::vector<Release::ReleaseArtifactRecord> records{{"assets.horo", Release::ReleaseArtifactRole::AssetArchive,
                                                             content.archive.size(),
                                                             ComputeSha256(std::as_bytes(std::span{content.archive}))},
                                                            {"bin/game", Release::ReleaseArtifactRole::Binary, fixtureEntrypoint.size(),
                                                             ComputeSha256(std::as_bytes(std::span{fixtureEntrypoint}))}};
        auto inventory = Release::ReleasePreSignInventory::Create({42}, records);
        REQUIRE(inventory.HasValue());
        Release::UpdateZipPackageProducer producer{
            {.maximumEntries = 16, .maximumFileBytes = 4U * 1024U * 1024U, .maximumExpandedBytes = 8U * 1024U * 1024U}};
        Release::IReleasePackageProducer *producers[]{&producer};
        const Release::ReleasePackageRequest request{Selection(product), inventory.Value(), stage,
                                                     root / "output",    "bin/game",        {"bin/game"}};
        REQUIRE(std::filesystem::create_directories(request.privateOutputRoot));
        const auto produced = Release::ProduceReleasePackage(request, producers);
        INFO((produced.HasError() ? ErrorText(produced.ErrorValue()) : std::string{}));
        REQUIRE(produced.HasValue());
        REQUIRE(produced.Value().files.size() == 1);
        const auto &package = produced.Value().files.front();
        const auto zip = request.privateOutputRoot / package.path;
        auto extracted = ExtractArchive(zip);
        REQUIRE(extracted == content.archive);
        const auto packagedPath = "packages/" + package.path;
        std::filesystem::create_directories(stage / "packages");
        REQUIRE(std::filesystem::copy_file(zip, stage / packagedPath));
        records.push_back({packagedPath, Release::ReleaseArtifactRole::NativePackage, package.size, package.digest});
        Release::ReleaseArtifactManifestData data;
        data.candidate = {42};
        data.product = {product, {}};
        data.version = Release::GameProductVersion{{1, 2, 3, {}, {}}};
        data.sourceRevision = {"navigation_fixture_source"};
        data.platform = Release::DistributionPlatform::Windows;
        data.architecture = Release::DistributionArchitecture::X64;
        data.configuration = Release::ReleaseBuildConfiguration::Shipping;
        data.build = {"nav_build_42"};
        data.toolchainId = "test_host_policy";
        data.artifacts = std::move(records);
        data.extensions = {content.extension};
        auto manifest = Release::ReleaseArtifactManifest::Create(std::move(data));
        REQUIRE(manifest.HasValue());
        const auto &json = manifest.Value().CanonicalJson();
        Write(stage / "manifest.json", {reinterpret_cast<const std::uint8_t *>(json.data()), json.size()});
        return {stage, std::move(manifest).Value(), std::move(extracted)};
    }

    /** @brief Run the real generic verification pipeline including required navigation smoke; unsigned is policy-admitted. */
    [[nodiscard]] inline Release::VerifiedReleaseCandidate Verify(Package &package, const AssetCookTargetId &target,
                                                                  const Release::DistributionProductKind product) {
        NavigationReleaseContentSmokeProbe smoke{"assets.horo", target, product};
        Release::IReleaseCandidateSmokeProbe *probes[]{&smoke};
        const std::array kinds{Release::ReleaseCandidateSmokeKind::RuntimeAssets};
        auto verified = Release::VerifyReleaseCandidate(package.stage, package.manifest, kinds, nullptr, probes);
        REQUIRE(verified.HasValue());
        REQUIRE(verified.Value().ManifestDigest() == package.manifest.Digest());
        return std::move(verified).Value();
    }
}  // namespace Horo::Application::ContentTestSupport
