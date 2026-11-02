#include "EditorPlaySaveHostStorage.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <new>
#include <tuple>

namespace Horo::Application::Internal {
    namespace {
        using namespace Runtime;

        /** @brief Issues a fresh opaque UUID using the explicitly supplied native entropy owner, never a path or frame ordinal. */
        template <typename Identity> Result<Identity> RandomIdentity(Security::SecureRandomSource &entropy) {
            SaveIdentityDetail::Bytes bytes{};
            if (const auto random = entropy.Fill(std::as_writable_bytes(std::span{bytes})); random.HasError())
                return Result<Identity>::Failure(random.ErrorValue());
            bytes[6] = static_cast<std::uint8_t>((bytes[6] & 15U) | 64U);
            bytes[8] = static_cast<std::uint8_t>((bytes[8] & 63U) | 128U);
            return Identity::FromBytes(bytes);
        }

        /** @brief Only an exact private activation's immutable logical world may enter its sandbox. */
        bool SameWorld(const SaveArchiveHeader &header, const PlaySaveStorageConfig &config) noexcept {
            return std::tie(header.project, header.world, header.baseScene) == std::tie(config.project, config.world, config.baseScene);
        }

        /** @brief Describes one exact readable schema without allowing implicit migration or owner dropping. */
        SaveParticipantCompatibility Compatibility(const CanonicalStateParticipantDescriptor &descriptor) {
            return {descriptor.participant, {{descriptor.schemaVersion, descriptor.schemaVersion}, {}}, descriptor.required, {}};
        }

        /** @brief Derives the release policy solely from the actual frozen core and native gameplay declarations. */
        SaveSlotLifecyclePolicy Policy(const PlaySaveStorageConfig &config, const PlaySaveStorageSchema &schema) {
            SaveSlotLifecyclePolicy policy;
            policy.profile = SaveSlotLifecycleProfile::Development;
            policy.capabilities = (1U << static_cast<unsigned>(SaveSlotLifecycleKind::PublishSave)) |
                                  (1U << static_cast<unsigned>(SaveSlotLifecycleKind::Export)) |
                                  (1U << static_cast<unsigned>(SaveSlotLifecycleKind::Delete));
            policy.allowPermanentDelete = true;
            policy.signature = SaveSignaturePolicy::Disabled;
            const auto &user = std::get<UserProfileOwner>(config.sandbox.owner);
            policy.destination = {config.sandbox, user.user, user.profile};
            policy.maximumSlots = PlaySaveMaximumSlots;
            policy.archiveLimits = PlaySaveArchiveLimits();
            policy.compatibility.archiveVersions.direct = {ArchiveFormatVersion::Create(1).Value(),
                                                           ArchiveFormatVersion::Create(1).Value()};
            policy.compatibility.saveSchemaVersions.direct = {SaveSchemaVersion::Create(1).Value(), SaveSchemaVersion::Create(1).Value()};
            policy.compatibility.productVersions.direct = {ProductSaveCompatibilityVersion::Create(1).Value(),
                                                           ProductSaveCompatibilityVersion::Create(1).Value()};
            policy.compatibility.participants.push_back(Compatibility(PlaySaveSceneDescriptor()));
            if (schema.lifecycle)
                policy.compatibility.participants.push_back(Compatibility(*schema.lifecycle));
            for (const auto &declaration : schema.gameplay)
                policy.compatibility.participants.push_back(Compatibility(declaration.descriptor.participant));
            std::ranges::sort(policy.compatibility.participants, {}, &SaveParticipantCompatibility::participant);
            return policy;
        }

        /** @brief Admits root/namespace identity and required owners before creating the existing filesystem capability. */
        bool ValidConfig(const PlaySaveStorageConfig &config, const std::shared_ptr<const PlaySaveStorageSchema> &schema) noexcept {
            return config.root.IsValid() && config.sandbox.IsValid() && config.root.Product() == config.sandbox.product &&
                   std::holds_alternative<UserProfileOwner>(config.sandbox.owner) && config.project.IsValid() && config.world.IsValid() &&
                   config.baseScene.IsValid() && config.entropy && schema;
        }

        /** @brief Converts exact UI consent to the existing storage CAS without copying any native path. */
        SaveSlotLifecycleTarget Target(const SaveManagerCommand &selection) {
            return {{{selection.expectedSnapshot.namespaceId, selection.expectedSnapshot.bindingRevision}, selection.slot},
                    selection.expectedSnapshot.catalogRevision,
                    selection.expectedGeneration};
        }
    }  // namespace

    struct PlaySaveStorage::State final : Runtime::ISaveSlotLifecycleHost, std::enable_shared_from_this<State> {
        /** @brief Owns detached immutable policy before opening any contained namespace directory. */
        State(PlaySaveStorageConfig config, std::shared_ptr<const PlaySaveStorageSchema> schema)
            : config(std::move(config)), schema(std::move(schema)) {}

        /** @brief Each ordinary lifecycle operation pins this exact host through completion without extending new admission. */
        struct Lease final : Runtime::ISaveSlotOperationLease {
            explicit Lease(std::shared_ptr<State> owner) : owner(std::move(owner)) {}

            std::shared_ptr<State> owner;
        };

        /** @copydoc Runtime::ISaveSlotLifecycleHost::AcquireBinding */
        Result<std::unique_ptr<Runtime::ISaveSlotOperationLease>> AcquireBinding(
            const Runtime::SaveNamespaceAccessRequest &access) override {
            using Outcome = Result<std::unique_ptr<Runtime::ISaveSlotOperationLease>>;
            if (closed.load(std::memory_order_acquire))
                return Outcome::Failure(MakeError(Runtime::SaveErrors::NamespaceUnavailable));
            const Runtime::SaveNamespaceBindingSnapshot binding{config.sandbox, Runtime::SaveNamespaceBindingState::Available, 1};
            if (const auto valid = Runtime::ValidateSaveNamespaceAccess(access, binding); valid.HasError())
                return Outcome::Failure(valid.ErrorValue());
            auto lease = std::make_unique<Lease>(shared_from_this());
            if (closed.load(std::memory_order_acquire))
                return Outcome::Failure(MakeError(Runtime::SaveErrors::NamespaceUnavailable));
            return Outcome::Success(std::move(lease));
        }

        /** @copydoc Runtime::ISaveSlotLifecycleHost::AllocateGeneration */
        Result<Runtime::SlotGenerationId> AllocateGeneration() override {
            std::lock_guard lock(entropyMutex);
            return RandomIdentity<Runtime::SlotGenerationId>(*config.entropy);
        }

        /** @copydoc Runtime::ISaveSlotLifecycleHost::ValidateSemantics */
        Result<void> ValidateSemantics(const Runtime::ValidatedSaveArchive &archive) override {
            if (!SameWorld(archive.Header(), config))
                return Result<void>::Failure(MakeError(Runtime::SaveErrors::RestoreContextInvalid));
            if (const auto logical = ValidatePlaySaveArchiveState(archive); logical.HasError())
                return logical;
            const auto bytes = archive.SelectChunk(PlaySaveSceneRecord());
            if (bytes.HasError())
                return Result<void>::Failure(bytes.ErrorValue());
            if (!bytes.Value())
                return Result<void>::Failure(MakeError(Runtime::SaveErrors::CaptureIncomplete));
            const auto core = DecodePlaySaveSceneStructure(*bytes.Value(), schema->baseline, schema->identities);
            return core.HasError() ? Result<void>::Failure(core.ErrorValue()) : Result<void>::Success();
        }

        /** @copydoc Runtime::ISaveSlotLifecycleHost::Verifier */
        Runtime::SaveArchiveSignatureProvider *Verifier() noexcept override {
            return nullptr;
        }

        /** @copydoc Runtime::ISaveSlotLifecycleHost::SignDestination */
        Result<std::vector<std::byte>> SignDestination(std::vector<std::byte>, const Runtime::SaveSlotArchiveScope &) override {
            return Result<std::vector<std::byte>>::Failure(MakeError(Runtime::SaveErrors::ProtectionUnavailable));
        }

        /** @copydoc Runtime::ISaveSlotLifecycleHost::SupportsRecycle */
        bool SupportsRecycle() const noexcept override {
            return false;
        }

        /** @copydoc Runtime::ISaveSlotLifecycleHost::Recycle */
        Result<void> Recycle(const Runtime::SaveSlotCatalogEntry &, Runtime::ImmutableSaveArchive) noexcept override {
            return Result<void>::Failure(unsupported);
        }

        PlaySaveStorageConfig config;
        std::shared_ptr<const PlaySaveStorageSchema> schema;
        std::atomic_bool closed{}; /**< Cross-thread close denies new binding leases; namespace never mutates or rebinds. */
        std::mutex entropyMutex;   /**< Serializes entropy provider calls across independent worker archive/slot generation requests. */
        const Error unsupported{MakeError(Runtime::SaveErrors::StorageCapabilityUnsupported)};
        std::optional<Runtime::SaveSlotLifecycle> lifecycle; /**< Declared last: closes storage before host policy/source leases. */
    };

    /** @copydoc PlaySaveStorage::Open */
    Result<std::shared_ptr<PlaySaveStorage>> PlaySaveStorage::Open(PlaySaveStorageConfig config,
                                                                   std::shared_ptr<const PlaySaveStorageSchema> schema) {
        using Outcome = Result<std::shared_ptr<PlaySaveStorage>>;
        if (!ValidConfig(config, schema))
            return Outcome::Failure(MakeError(Runtime::SaveErrors::CompositionUnsupported));
        try {
            auto state = std::make_shared<State>(std::move(config), std::move(schema));
            auto lifecycle = Runtime::SaveSlotLifecycle::Open(state->config.root, Policy(state->config, *state->schema), *state);
            if (lifecycle.HasError())
                return Outcome::Failure(lifecycle.ErrorValue());
            state->lifecycle.emplace(std::move(lifecycle).Value());
            return Outcome::Success(std::make_shared<PlaySaveStorage>(std::move(state)));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(Runtime::SaveErrors::StorageAllocationFailed));
        }
    }

    /** @copydoc PlaySaveStorage::PlaySaveStorage */
    PlaySaveStorage::PlaySaveStorage(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

    /** @copydoc PlaySaveStorage::Namespace */
    const Runtime::SaveNamespaceId &PlaySaveStorage::Namespace() const noexcept {
        return state_->config.sandbox;
    }

    /** @copydoc PlaySaveStorage::List */
    Result<Runtime::SaveSlotIndex> PlaySaveStorage::List() const {
        return state_->lifecycle->List({state_->config.sandbox, 1});
    }

    /** @copydoc PlaySaveStorage::NewSlot */
    Result<Runtime::SaveSlotLifecycleTarget> PlaySaveStorage::NewSlot() {
        using Outcome = Result<Runtime::SaveSlotLifecycleTarget>;
        const auto index = List();
        if (index.HasError())
            return Outcome::Failure(index.ErrorValue());
        if (index.Value().entries.size() >= PlaySaveMaximumSlots)
            return Outcome::Failure(MakeError(Runtime::SaveErrors::CaptureBudgetExceeded));
        std::lock_guard lock(state_->entropyMutex);
        const auto slot = RandomIdentity<Runtime::SaveGameSlotId>(*state_->config.entropy);
        if (slot.HasError())
            return Outcome::Failure(slot.ErrorValue());
        if (std::ranges::any_of(index.Value().entries, [&](const auto &entry) {
            return entry.publication.slot == slot.Value();
        }))
            return Outcome::Failure(MakeError(Runtime::SaveErrors::IdentityInvalid));
        return Outcome::Success({{{state_->config.sandbox, 1}, slot.Value()}, index.Value().revision, {}});
    }

    /** @copydoc PlaySaveStorage::Header */
    Result<Runtime::SaveArchiveHeader> PlaySaveStorage::Header(const Runtime::SaveSlotLifecycleTarget &target,
                                                               std::uint64_t unixMilliseconds, std::uint64_t playNanoseconds) {
        using Outcome = Result<Runtime::SaveArchiveHeader>;
        if (target.address.namespaceAccess.expected != state_->config.sandbox || target.address.namespaceAccess.expectedRevision != 1)
            return Outcome::Failure(MakeError(Runtime::SaveErrors::NamespaceUnavailable));
        const auto generation = state_->AllocateGeneration();
        if (generation.HasError())
            return Outcome::Failure(generation.ErrorValue());
        const auto &user = std::get<Runtime::UserProfileOwner>(state_->config.sandbox.owner);
        return Outcome::Success({.slot = target.address.slot,
                                 .slotGeneration = generation.Value(),
                                 .parentGeneration = target.generation,
                                 .productCompatibility = Runtime::ProductSaveCompatibilityVersion::Create(1).Value(),
                                 .product = state_->config.sandbox.product,
                                 .environment = state_->config.sandbox.environment,
                                 .user = user.user,
                                 .profile = user.profile,
                                 .project = state_->config.project,
                                 .world = state_->config.world,
                                 .baseScene = state_->config.baseScene,
                                 .capturedAtUnixMilliseconds = unixMilliseconds,
                                 .playTimeNanoseconds = playNanoseconds,
                                 .engineVersion = state_->config.engineVersion,
                                 .projectBuildId = state_->config.projectBuildId});
    }

    /** @copydoc PlaySaveStorage::Read */
    Result<Runtime::ValidatedSaveArchive> PlaySaveStorage::Read(const Runtime::SaveManagerCommand &selection,
                                                                const CancellationToken &cancellation) {
        using Outcome = Result<Runtime::ValidatedSaveArchive>;
        const auto result =
            state_->lifecycle->Execute({.kind = Runtime::SaveSlotLifecycleKind::Export, .source = Target(selection)}, cancellation);
        if (result.HasError())
            return Outcome::Failure(result.ErrorValue());
        auto archive = Runtime::SaveArchiveReader{PlaySaveArchiveLimits()}.Read(result.Value().exported.bytes);
        if (archive.HasError())
            return Outcome::Failure(archive.ErrorValue());
        if (const auto semantics = state_->ValidateSemantics(archive.Value()); semantics.HasError())
            return Outcome::Failure(semantics.ErrorValue());
        return archive;
    }

    /** @copydoc PlaySaveStorage::Commit */
    Result<Runtime::SaveSlotLifecycleResult> PlaySaveStorage::Commit(const Runtime::SaveSlotLifecycleTarget &target,
                                                                     const Runtime::FinalizedSaveArchive &archive,
                                                                     Runtime::SaveSlotDisplayMetadata display) {
        using Outcome = Result<Runtime::SaveSlotLifecycleResult>;
        const auto admitted = Runtime::SaveArchiveReader{PlaySaveArchiveLimits()}.Read(archive.Archive().bytes);
        if (admitted.HasError())
            return Outcome::Failure(admitted.ErrorValue());
        const auto &header = admitted.Value().Header();
        Runtime::SaveSlotPublicationMetadata metadata{.slot = header.slot,
                                                      .generation = header.slotGeneration,
                                                      .savedAtUnixMilliseconds = header.capturedAtUnixMilliseconds,
                                                      .playTimeNanoseconds = header.playTimeNanoseconds,
                                                      .baseScene = header.baseScene,
                                                      .productCompatibility = header.productCompatibility,
                                                      .saveSchema = admitted.Value().Manifest().saveSchemaVersion,
                                                      .projectBuildId = header.projectBuildId,
                                                      .canonicalState = archive.Summary().canonicalState,
                                                      .archiveContent = archive.Summary().integrity.archiveContent};
        return state_->lifecycle->CommitSave(target, {{std::move(metadata), std::move(display)}, archive.Archive()},
                                             header.capturedAtUnixMilliseconds);
    }

    /** @copydoc PlaySaveStorage::Delete */
    Result<Runtime::SaveSlotLifecycleResult> PlaySaveStorage::Delete(const Runtime::SaveManagerCommand &selection,
                                                                     const CancellationToken &cancellation) {
        return state_->lifecycle->Execute({.kind = Runtime::SaveSlotLifecycleKind::Delete,
                                           .source = Target(selection),
                                           .deleteMode = Runtime::SaveSlotDeleteMode::Permanent},
                                          cancellation);
    }

    /** @copydoc PlaySaveStorage::Cleanup */
    Result<void> PlaySaveStorage::Cleanup() {
        auto selected = List();
        if (selected.HasError())
            return Result<void>::Failure(selected.ErrorValue());
        auto revision = selected.Value().revision;
        for (const auto &entry : selected.Value().entries) {
            const Runtime::SaveSlotLifecycleTarget target{{{state_->config.sandbox, 1}, entry.publication.slot},
                                                          revision,
                                                          entry.publication.generation};
            const auto removed = state_->lifecycle->Execute(
                {.kind = Runtime::SaveSlotLifecycleKind::Delete, .source = target, .deleteMode = Runtime::SaveSlotDeleteMode::Permanent});
            if (removed.HasError())
                return Result<void>::Failure(removed.ErrorValue());
            revision = removed.Value().catalogRevision;
        }
        const auto reconciled = state_->lifecycle->Reconcile({state_->config.sandbox, 1});
        if (reconciled.HasError())
            return Result<void>::Failure(reconciled.ErrorValue());
        if (reconciled.Value())
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::OperationInProgress));
        return Result<void>::Success();
    }

    /** @copydoc PlaySaveStorage::CloseAdmission */
    void PlaySaveStorage::CloseAdmission() noexcept {
        state_->closed.store(true, std::memory_order_release);
    }
}  // namespace Horo::Application::Internal
