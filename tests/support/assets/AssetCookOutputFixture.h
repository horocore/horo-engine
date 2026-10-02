#pragma once

#include "Horo/Assets/AssetCookOutput.h"
#include "Horo/Assets/AssetCookTransaction.h"
#include "Horo/Foundation/Sha256.h"
#include "NativePublicationFiles.h"
#include "assets/AssetCookTestValues.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Horo::Assets::OutputTestSupport {
    using CookTestValues::Id;
    using CookTestValues::OwnedCookTestDirectory;
    using CookTestValues::Target;
    using CookTestValues::Type;

    inline Sha256Digest DigestOf(std::span<const std::uint8_t> bytes) {
        return ComputeSha256(std::as_bytes(bytes));
    }

    inline std::vector<std::uint8_t> MakePayload(const AssetId &id, std::size_t size, std::uint8_t fill = 0x42) {
        const std::vector bytes(size, fill);
        auto encoded = EncodeCookedArtifact(AssetCookArtifact{.id = id,
                                                              .type = Type("core.mesh"),
                                                              .target = Target("headless-null"),
                                                              .payloadDigest = DigestOf(bytes),
                                                              .payload = bytes});
        REQUIRE(encoded.HasValue());
        return std::move(encoded).Value();
    }

    struct TempDir final : OwnedCookTestDirectory {
        TempDir() : OwnedCookTestDirectory("horo_output_test", true) {}
    };

    enum class PublicationFault {
        None,
        ArtifactWrite,
        ManifestWrite,
        PointerWrite,
        GenerationRename,
        GenerationFlush,
        RootFlush,
        PointerRename,
        PointerSync,
        PointerException,
        BaselineSync
    };

    struct PublicationFiles final : Horo::TestSupport::NativePublicationFiles {
        PublicationFault fault{};
        std::filesystem::path root;

        Result<void> Failure() const {
            return Result<void>::Failure(Error{.code = ErrorCode{"test.publication_io"}, .message = "Injected publication I/O failure."});
        }

        Result<void> WriteDurable(const std::filesystem::path &path, std::span<const std::byte> bytes) override {
            if ((fault == PublicationFault::ArtifactWrite && path.extension() == ".cooked") ||
                (fault == PublicationFault::ManifestWrite && path.filename() == "manifest.json") ||
                (fault == PublicationFault::PointerWrite && path.filename() == "current.json"))
                return Failure();
            return native.WriteDurable(path, bytes);
        }

        Result<void> AtomicReplace(const std::filesystem::path &prepared, const std::filesystem::path &destination) override {
            const bool pointer = destination.filename() == "current.json";
            if ((pointer && fault == PublicationFault::PointerRename) || (!pointer && fault == PublicationFault::GenerationRename))
                return Failure();
            auto result = native.AtomicReplace(prepared, destination);
            if (result.HasError())
                return result;
            if (pointer && fault == PublicationFault::PointerException)
                throw 42;
            if (pointer && fault == PublicationFault::PointerSync)
                return Failure();
            return result;
        }

        Result<void> AtomicReplaceTracked(const std::filesystem::path &prepared, const std::filesystem::path &destination,
                                          AtomicFileReplacementReceipt &receipt) override {
            const bool baseline = prepared.filename() == "unpublished.current.json";
            if (!baseline && fault == PublicationFault::PointerRename)
                return Failure();
            auto result = native.AtomicReplaceTracked(prepared, destination, receipt);
            if (result.HasError())
                return result;
            if (baseline)
                return fault == PublicationFault::BaselineSync ? Failure() : std::move(result);
            if (fault == PublicationFault::PointerException)
                throw 42;
            return fault == PublicationFault::PointerSync ? Failure() : std::move(result);
        }

        Result<void> SyncDirectory(const std::filesystem::path &path) override {
            if ((fault == PublicationFault::GenerationFlush && path.filename() == "generation") ||
                (fault == PublicationFault::RootFlush && path == root))
                return Failure();
            return native.SyncDirectory(path);
        }
    };

    inline AssetCookManifestEntry Entry(const AssetId &id, const std::vector<std::uint8_t> &bytes) {
        return {.assetId = id, .assetType = Type("core.mesh"), .artifactFile = id.ToString() + ".cooked", .artifactHash = DigestOf(bytes)};
    }

    inline AssetCookPublicationPolicy Policy(PublicationFiles &files, const std::string_view token) {
        return {.files = &files, .operationId = std::string(token)};
    }

    inline std::string ReadText(const std::filesystem::path &path) {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    }

    inline Result<AssetCookGeneration> PublishFixture(const std::filesystem::path &root, const AssetCookTargetId &target,
                                                      std::span<const AssetCookManifestEntry> entries,
                                                      std::span<const std::vector<std::uint8_t>> payloads,
                                                      const AssetCookLimits &limits = {}) {
        NativeDurableFileSystem files;
        auto lock = files.TryAcquireExclusive(root / ".cook-writer.lock", "publication fixture");
        REQUIRE(lock.HasValue());
        AssetCookPublicationPolicy policy{.files = &files};
        policy.newOperationId = [] {
            static std::uint64_t next{};
            const auto value = ++next;
            std::array<std::uint8_t, 16> bytes{};
            for (std::size_t index = 0; index < sizeof(value); ++index)
                bytes[index] = static_cast<std::uint8_t>(value >> (index * 8U));
            return Result<AssetId>::Success(AssetId::FromBytes(bytes));
        };
        policy.writerLease = &lock.Value();
        return Horo::Assets::PublishCookGeneration(root, target, entries, payloads, limits, policy);
    }

}  // namespace Horo::Assets::OutputTestSupport
