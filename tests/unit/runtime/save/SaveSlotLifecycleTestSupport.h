#pragma once

#include "Horo/Runtime/Save/SaveSlotLifecycle.h"
#include "SaveArchiveReaderTestHelpers.h"
#include "SaveFilesystemTestSupport.h"

#include <catch2/generators/catch_generators.hpp>
#include <cerrno>
#include <fstream>
#include <functional>
#include <map>
#include <new>

namespace Horo::Runtime::SaveSlotLifecycleTest {
    using namespace Horo;
    using namespace Horo::Runtime::Test;
    using namespace Horo::Runtime::ArchiveReaderTest;
    using namespace Horo::Runtime::SaveFilesystemTest;

    inline SaveNamespaceId Namespace() {
        return {Id<ProductStorageId>(3), Id<EnvironmentStorageId>(4), UserProfileOwner{Id<LocalUserStorageId>(5), Id<GameProfileId>(6)}};
    }

    inline ValidatedSaveArchive ReadOwned(std::vector<std::byte> bytes) {
        auto archive = SaveArchiveReader{}.Read(std::make_shared<const std::vector<std::byte>>(std::move(bytes)));
        REQUIRE(archive.HasValue());
        return std::move(archive).Value();
    }

    inline std::vector<std::byte> RewriteHeader(const ValidatedSaveArchive &archive, const SaveArchiveHeader &header) {
        std::vector<PreservedSaveChunk> chunks;
        for (const auto &entry : archive.Directory().Entries()) {
            const auto stored = archive.Payload().subspan(entry.offset, entry.storedByteLength);
            chunks.push_back({entry, {stored.begin(), stored.end()}});
        }
        auto written = SaveArchiveContainerWriter::Write(header, archive.Manifest(), chunks, archive.Preamble().archiveFormatVersion);
        REQUIRE(written.HasValue());
        return {written.Value().Bytes().begin(), written.Value().Bytes().end()};
    }

    class Lease final : public ISaveSlotOperationLease {
    public:
        explicit Lease(std::size_t &count) : count_(&count) {
            ++*count_;
        }

        ~Lease() override {
            --*count_;
        }

    private:
        std::size_t *count_;
    };

    /** @brief Host authority fixture; semantics/signing/recycle faults occur before any live runtime mutation. */
    class Host final : public ISaveSlotLifecycleHost {
    public:
        Result<std::unique_ptr<ISaveSlotOperationLease>> AcquireBinding(const SaveNamespaceAccessRequest &access) override {
            if (auto valid = ValidateSaveNamespaceAccess(access, binding); valid.HasError())
                return Result<std::unique_ptr<ISaveSlotOperationLease>>::Failure(valid.ErrorValue());
            return Result<std::unique_ptr<ISaveSlotOperationLease>>::Success(std::make_unique<Lease>(leases));
        }

        Result<SlotGenerationId> AllocateGeneration() override {
            if (allocationFailure)
                return Result<SlotGenerationId>::Failure(MakeError(SaveErrors::StorageAllocationFailed));
            return Result<SlotGenerationId>::Success(Id<SlotGenerationId>(nextGeneration++));
        }

        Result<void> ValidateSemantics(const ValidatedSaveArchive &archive) override {
            ++semanticCalls;
            CHECK(leases == 1);
            if (onSemantics)
                onSemantics();
            if (semanticFailure)
                return Result<void>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid));
            for (const auto &participant : archive.Manifest().participants) {
                if (!participant.required)
                    continue;
                for (const auto record : participant.chunks) {
                    auto chunk = archive.SelectChunk(record);
                    if (chunk.HasError())
                        return Result<void>::Failure(chunk.ErrorValue());
                    if (!chunk.Value())
                        return Result<void>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid));
                }
            }
            return Result<void>::Success();
        }

        SaveArchiveSignatureProvider *Verifier() noexcept override {
            return verifier;
        }

        Result<std::vector<std::byte>> SignDestination(std::vector<std::byte> bytes, const SaveSlotArchiveScope &) override {
            ++signCalls;
            if (signer)
                return signer(std::move(bytes));
            return Result<std::vector<std::byte>>::Failure(MakeError(SaveErrors::ProtectionUnavailable));
        }

        bool SupportsRecycle() const noexcept override {
            return recycleSupported;
        }

        Result<void> Recycle(const SaveSlotCatalogEntry &entry, ImmutableSaveArchive archive) override {
            ++recycleCalls;
            if (onRecycle)
                onRecycle();
            if (recycleFailure)
                return Result<void>::Failure(MakeError(SaveErrors::StoragePermanentIo));
            recycled.insert_or_assign(entry.publication.generation, *archive.bytes);
            return Result<void>::Success();
        }

        SaveNamespaceBindingSnapshot binding{Namespace(), SaveNamespaceBindingState::Available, 7};
        std::size_t leases{};
        std::uint8_t nextGeneration{100};
        bool allocationFailure{};
        bool semanticFailure{};
        bool recycleSupported{};
        bool recycleFailure{};
        std::size_t semanticCalls{};
        std::size_t signCalls{};
        std::size_t recycleCalls{};
        SaveArchiveSignatureProvider *verifier{};
        std::function<void()> onSemantics;
        std::function<void()> onRecycle;
        std::function<Result<std::vector<std::byte>>(std::vector<std::byte>)> signer;
        std::map<SlotGenerationId, std::vector<std::byte>> recycled;
    };

    /** @brief Exact native stage hook; injected failures still exercise real temporary/file/selection operations. */
    class Fault final : public ISaveSlotLifecycleIoObserver {
    public:
        Result<void> Before(const SaveSlotLifecycleIoStage stage, const SaveSlotLifecycleFileKind kind) override {
            if (action)
                action(stage, kind);
            if (enabled && stage == failStage && kind == failKind) {
                enabled = false;
                return Result<void>::Failure(MakeError(*error));
            }
            return Result<void>::Success();
        }

        bool enabled{};
        SaveSlotLifecycleIoStage failStage{SaveSlotLifecycleIoStage::Write};
        SaveSlotLifecycleFileKind failKind{SaveSlotLifecycleFileKind::Generation};
        const ErrorCodeDescriptor *error{&SaveErrors::StorageDiskFull};
        std::function<void(SaveSlotLifecycleIoStage, SaveSlotLifecycleFileKind)> action;
    };

    inline SaveSlotLifecyclePolicy Policy() {
        auto policy = UnknownPolicy();
        policy.archiveVersions.direct.maximum = V<ArchiveFormatVersion>(2);
        policy.saveSchemaVersions.direct.maximum = V<SaveSchemaVersion>(2);
        const auto name = Namespace();
        const SaveSlotArchiveScope scope{name, Id<LocalUserStorageId>(5), Id<GameProfileId>(6)};
        return {.profile = SaveSlotLifecycleProfile::Development,
                .capabilities = static_cast<std::uint16_t>((1U << static_cast<std::uint8_t>(SaveSlotLifecycleKind::Count)) - 1U),
                .allowPermanentDelete = true,
                .allowPlatformRecycle = true,
                .signature = SaveSignaturePolicy::Disabled,
                .destination = scope,
                .importSources = {scope},
                .compatibility = std::move(policy)};
    }

    /** @brief Gives physical probes the same long-path reach as native relative storage handles. */
    inline std::filesystem::path NativeProbePath(const std::filesystem::path &path) {
#ifdef _WIN32
        // Native storage opens relative handles; physical test probes must address the same
        // long namespace path without the ordinary Win32 MAX_PATH limit.
        const auto &native = path.native();
        if (native.starts_with(L"\\\\?\\"))
            return path;
        if (native.starts_with(L"\\\\"))
            return std::filesystem::path{L"\\\\?\\UNC\\" + native.substr(2)};
        return std::filesystem::path{L"\\\\?\\" + native};
#else
        return path;
#endif
    }

    inline std::vector<std::byte> DiskBytes(const std::filesystem::path &path) {
        std::ifstream file(NativeProbePath(path), std::ios::binary);
        const int openError = errno;
        std::error_code inspection;
        const bool exists = std::filesystem::exists(NativeProbePath(path), inspection);
        INFO("Physical read path units=" << path.native().size() << ", exists=" << exists << ", inspection=" << inspection.value()
                                         << ", errno=" << openError);
        REQUIRE(file.good());
        const std::vector<char> chars{std::istreambuf_iterator<char>{file}, {}};
        std::vector<std::byte> bytes;
        for (const auto value : chars)
            bytes.push_back(static_cast<std::byte>(value));
        return bytes;
    }

    inline void WriteBytes(const std::filesystem::path &path, const std::span<const std::byte> bytes) {
        std::ofstream file(NativeProbePath(path), std::ios::binary | std::ios::trunc);
        INFO("Physical corruption path units=" << path.native().size() << ", errno=" << errno);
        REQUIRE(file.good());
        file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        REQUIRE(file.good());
    }

    struct Fixture final {
        TemporaryDirectory temporary;
        FixedEnvironment environment;
        Host host;
        Fault fault;
        SaveSlotLifecyclePolicy policy{Policy()};
        ProductSaveRoot root =
            Resolve({.product = Namespace().product, .platform = SaveRootPlatform::Test, .testStateRoot = temporary.Path() / "approved ü"},
                    environment);
        mutable std::optional<SaveSlotLifecycle> owner;

        Fixture() {
            Open();
        }

        void Open() {
            auto opened = SaveSlotLifecycle::Open(root, policy, host, &fault);
            REQUIRE(opened.HasValue());
            owner.emplace(std::move(opened).Value());
        }

        void Reopen() {
            owner.reset();
            Open();
        }

        void InjectFailure(const SaveSlotLifecycleIoStage stage, const SaveSlotLifecycleFileKind kind) {
            fault.enabled = true;
            fault.failStage = stage;
            fault.failKind = kind;
        }

        SaveNamespaceAccessRequest Access() const {
            return {policy.destination.name, host.binding.revision};
        }

        std::filesystem::path Slots() const {
            const auto path = root.CanonicalPath() / Namespace().environment.ToString() / "profile" /
                              (Id<LocalUserStorageId>(5).ToString() + "_" + Id<GameProfileId>(6).ToString()) / "slots";
            return NativeProbePath(path);
        }

        std::filesystem::path Generation(const SlotGenerationId generation) const {
            return Slots() / (".generation." + generation.ToString() + ".horosave");
        }

        SaveSlotIndex Index() const {
            auto list = owner->List(Access());
            REQUIRE(list.HasValue());
            return std::move(list).Value();
        }

        SaveSlotLifecycleTarget Target(const std::uint8_t slot, const bool deleted = false) const {
            auto index = deleted ? owner->ListDeleted(Access()) : owner->List(Access());
            REQUIRE(index.HasValue());
            SaveSlotLifecycleTarget target{{Access(), Id<SaveGameSlotId>(slot)}, index.Value().revision, {}};
            for (const auto &entry : index.Value().entries) {
                if (entry.publication.slot == target.address.slot)
                    target.generation = entry.publication.generation;
            }
            return target;
        }

        SaveSlotLifecycleResult Import(const std::uint8_t slot, std::vector<std::byte> bytes = MakeArchive().bytes,
                                       const std::size_t sourceScope = 0) {
            auto result = owner->Execute({.kind = SaveSlotLifecycleKind::Import,
                                          .source = Target(slot),
                                          .imported = {std::make_shared<const std::vector<std::byte>>(std::move(bytes))},
                                          .importSource = sourceScope,
                                          .display = {"Slot ü"}});
            const auto diagnostic =
                result.HasError() ? result.ErrorValue().code.Value() + ": " + result.ErrorValue().message : std::string{"Import succeeded"};
            INFO(diagnostic);
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        SaveSlotLifecycleRequest Copy(const std::uint8_t source, const std::uint8_t destination) const {
            return {.kind = SaveSlotLifecycleKind::Copy, .source = Target(source), .destination = Target(destination), .display = {"Copy"}};
        }

        SaveSlotLifecycleRequest Export(const std::uint8_t slot) const {
            return {.kind = SaveSlotLifecycleKind::Export, .source = Target(slot)};
        }

        std::vector<std::byte> ExportBytes(const std::uint8_t slot) const {
            auto result = owner->Execute(Export(slot));
            REQUIRE(result.HasValue());
            return *result.Value().exported.bytes;
        }
    };
}  // namespace Horo::Runtime::SaveSlotLifecycleTest
