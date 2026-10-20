#pragma once
#include "Horo/Editor/PCGGraphDocumentService.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <fstream>
#include <functional>
#include <stdexcept>

namespace Horo::Editor::PCGDocumentTestSupport {
    template <typename T> T Id(std::uint64_t value) {
        return T::Create(value).Value();
    }

    inline Assets::AssetId Asset(unsigned char value = 1) {
        std::array<std::uint8_t, 16> bytes{};
        bytes[15] = value;
        return Assets::AssetId::FromBytes(bytes);
    }

    inline PCGDocumentCatalog Catalog(std::uint64_t revision = 1) {
        return {.revision = revision, .types = {{Id<PCG::NodeTypeId>(1), {1, 0}, {1, 0}}}};
    }

    inline PCG::PCGGraphSourceData Graph(std::uint64_t revision = 1, std::uint64_t seed = 11) {
        PCG::PCGGraphSourceData data;
        data.generation = {Id<PCG::GraphId>(1), Id<PCG::GraphRevision>(revision)};
        data.deterministicSeed = seed;
        data.nodes = {{Id<PCG::NodeId>(1),
                       Id<PCG::NodeTypeId>(1),
                       {1, 0},
                       {{Id<PCG::PinId>(1), PCG::PCGPinDirection::Input, PCG::PCGPinType::Scalar, PCG::PCGPinCardinality::Single,
                         PCG::PCGGraphValue{2.0}},
                        {Id<PCG::PinId>(2), PCG::PCGPinDirection::Output, PCG::PCGPinType::Scalar, PCG::PCGPinCardinality::Multiple, {}}},
                       {1, 2}},
                      {Id<PCG::NodeId>(2),
                       Id<PCG::NodeTypeId>(1),
                       {1, 0},
                       {{Id<PCG::PinId>(3), PCG::PCGPinDirection::Input, PCG::PCGPinType::Scalar, PCG::PCGPinCardinality::Single, {}}},
                       {3}}};
        data.edges = {{Id<PCG::EdgeId>(1), Id<PCG::NodeId>(1), Id<PCG::PinId>(2), Id<PCG::NodeId>(2), Id<PCG::PinId>(3)}};
        data.exposedInputs = {{Id<PCG::ExposedInputId>(1), "world.density", Id<PCG::NodeId>(1), Id<PCG::PinId>(1), 3.0}};
        return data;
    }

    inline Assets::AssetSourceCapture Capture(PCG::PCGGraphSourceData data = Graph()) {
        const auto catalog = Catalog();
        const PCG::PCGGraphSourceContext context{.unknownNodePolicy = PCG::PCGUnknownNodePolicy::PreserveInert,
                                                 .supportedNodeTypes = catalog.types};
        auto graph = PCG::PCGGraphAsset::Create(std::move(data), context);
        REQUIRE(graph.HasValue());
        auto encoded = PCG::SerializePCGGraphAsset(graph.Value());
        REQUIRE(encoded.HasValue());
        const auto bytes = std::as_bytes(std::span(encoded.Value()));
        return {{true, bytes.size(), ComputeSha256(bytes)}, {bytes.begin(), bytes.end()}};
    }

    inline DocumentIdentity Identity(std::uint64_t instance = 1, std::string_view path = "assets/graphs/图 test.hpcg") {
        return {{DocumentKind::PCGGraph, SourceDocumentId::Parse(path).Value()}, Id<DocumentInstanceId>(instance)};
    }

    inline PCGDocumentCommand Fence(const PCGDocumentSnapshot &snapshot) {
        return {.session = snapshot.session,
                .expectedRevision = snapshot.revision,
                .catalogRevision = snapshot.catalogRevision,
                .canEdit = true};
    }

    inline PCGDocumentCommand Edit(const PCGDocumentSnapshot &snapshot, std::uint64_t seed) {
        auto command = Fence(snapshot);
        command.operations = {SetPCGGraphPolicy{PCG::PCGGenerationMode::Offline, seed}};
        return command;
    }

    template <typename T> void ErrorIs(const Result<T> &result, const ErrorCodeDescriptor &code) {
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == code.code.Value());
        CHECK(result.ErrorValue().domain.Value() == code.domain.Value());
    }

    const ErrorCodeDescriptor Injected{ErrorDomainId{"test.pcg.document"}, ErrorCode{"test.pcg.document.failure"}, ErrorSeverity::Error,
                                       "Injected failure.", "Retry the fixture."};

    class Files final : public DurableFileSystem {
    public:
        NativeDurableFileSystem native;
        std::function<void()> duringWrite;
        std::function<void()> duringRemove;
        std::function<void(const std::filesystem::path &)> afterWrite;
        bool failWrite{}, failReplace{}, failAfterCommit{};
        std::function<void()> afterReplacement;
        std::function<void(bool &)> afterCreation;
        std::function<void()> beforeReplacement;
        std::function<void()> beforeLock;
        bool untrackedSuccess{};
        std::size_t writes{}, replacements{};

        Horo::Result<bool> CreateDirectoryPath(const std::filesystem::path &path, Horo::DirectoryCreationMode mode) override {
            return native.CreateDirectoryPath(path, mode);
        }

        Result<ExclusiveFileLock> TryAcquireExclusive(const std::filesystem::path &path, std::string_view owner) override {
            if (beforeLock)
                beforeLock();
            return native.TryAcquireExclusive(path, owner);
        }

        Result<std::uint64_t> AvailableBytes(const std::filesystem::path &path) const override {
            return native.AvailableBytes(path);
        }

        Result<void> WriteDurable(const std::filesystem::path &path, std::span<const std::byte> bytes) override {
            return native.WriteDurable(path, bytes);
        }

        Result<void> WritePrivateDurable(const std::filesystem::path &path, std::span<const std::byte> bytes, bool &created,
                                         std::filesystem::perms permissions = std::filesystem::perms::unknown) override {
            ++writes;
            if (duringWrite) {
                auto callback = std::move(duringWrite);
                duringWrite = {};
                callback();
            }
            auto written =
                failWrite ? Result<void>::Failure(MakeError(Injected)) : native.WritePrivateDurable(path, bytes, created, permissions);
            if (afterCreation)
                afterCreation(created);
            if (written.HasValue() && afterWrite) {
                auto callback = std::move(afterWrite);
                afterWrite = {};
                callback(path);
            }
            return written;
        }

        Result<void> CopyDurable(const std::filesystem::path &from, const std::filesystem::path &to) override {
            return native.CopyDurable(from, to);
        }

        Result<void> AtomicReplace(const std::filesystem::path &from, const std::filesystem::path &to) override {
            return native.AtomicReplace(from, to);
        }

        Result<void> AtomicReplaceTracked(const std::filesystem::path &from, const std::filesystem::path &to,
                                          AtomicFileReplacementReceipt &receipt) override {
            ++replacements;
            if (beforeReplacement)
                beforeReplacement();
            if (untrackedSuccess)
                return Result<void>::Success();
            if (failReplace)
                return Result<void>::Failure(MakeError(Injected));
            auto result = native.AtomicReplaceTracked(from, to, receipt);
            if (result.HasValue() && afterReplacement)
                afterReplacement();
            if (result.HasValue() && failAfterCommit)
                return Result<void>::Failure(MakeError(Injected));
            return result;
        }

        Result<void> RemoveDurable(const std::filesystem::path &path) override {
            if (duringRemove) {
                auto callback = std::move(duringRemove);
                duringRemove = {};
                callback();
            }
            return native.RemoveDurable(path);
        }

        Result<void> SyncDirectory(const std::filesystem::path &path) override {
            return native.SyncDirectory(path);
        }
    };

    class Project final {
    public:
        Project() : mutations(files) {
            const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
            root = std::filesystem::temp_directory_path() / ("horo-pcg-document-" + std::to_string(tick));
            std::filesystem::create_directories(root / "assets/graphs");
            root = std::filesystem::canonical(root);
            path = root / Identity().key.source.Value();
            Write(Capture());
        }

        ~Project() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        void Write(const Assets::AssetSourceCapture &capture) {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char *>(capture.bytes.data()), static_cast<std::streamsize>(capture.bytes.size()));
        }

        PCGGraphDocument Open(SemanticHistoryLimits limits = {}) {
            auto document = PCGGraphDocument::Open(Asset(), Identity(), {Asset(9), 1}, Capture(), Catalog(), limits);
            REQUIRE(document.HasValue());
            return std::move(document).Value();
        }

        Files files;
        ProjectMutationCoordinator mutations;
        std::filesystem::path root, path;
    };

    // Synthetic current-layout fixture only; this does not define a production legacy migration mapping.
    class SyntheticMigrator final : public PCG::IPCGGraphSourceMigrator {
    public:
        Result<std::vector<std::uint8_t>> Migrate(std::span<const std::uint8_t> bytes, PCG::PCGGraphSchemaVersion from,
                                                  PCG::PCGGraphSchemaVersion to, std::size_t maximum) const override {
            REQUIRE(from == PCG::PCGGraphSchemaVersion{1, 0});
            REQUIRE(to == PCG::PCGGraphSchemaVersion{1, 1});
            REQUIRE(bytes.size() <= maximum);
            std::vector<std::uint8_t> result(bytes.begin(), bytes.end());
            result[7] = 1;
            if (invalidOutput)
                result.push_back(0);
            return Result<std::vector<std::uint8_t>>::Success(std::move(result));
        }

        bool invalidOutput{};
    };
}  // namespace Horo::Editor::PCGDocumentTestSupport
