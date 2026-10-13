#include "Horo/Editor/PCGGraphDocumentService.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <limits>
#include <thread>

namespace Horo::Editor {
    namespace {
        constexpr std::size_t MaximumDocuments = 32;
        constexpr std::size_t MaximumSourceBytes = 8 * 1024 * 1024;
        using Reason = PCGDocumentCommandFailureReason;

        /** @brief Constructs a stable typed service rejection. */
        template <typename T> Result<T> Failure(Reason reason) {
            return Result<T>::Failure(MakeError(PCGDocumentError(reason)));
        }

        /** @brief Captures a live owner command header; no worker or presentation pointers are retained. */
        PCGDocumentCommand Fence(const PCGDocumentSnapshot &snapshot) {
            return {.session = snapshot.session,
                    .expectedRevision = snapshot.revision,
                    .catalogRevision = snapshot.catalogRevision,
                    .canEdit = true};
        }

        /** @brief Fences reentrant lifecycle changes while permitting source edits against immutable captures. */
        struct OperationGuard final {
            bool &active;

            explicit OperationGuard(bool &value) : active(value) {
                value = true;
            }

            OperationGuard(const OperationGuard &) = delete;
            OperationGuard &operator=(const OperationGuard &) = delete;
            OperationGuard(OperationGuard &&) = delete;
            OperationGuard &operator=(OperationGuard &&) = delete;

            ~OperationGuard() {
                active = false;
            }
        };

        /** @brief Projects owned catalog data into the sole graph codec. */
        PCG::PCGGraphSourceContext Context(const PCGDocumentCatalog &catalog) {
            return {.tier = catalog.tier,
                    .unknownNodePolicy = PCG::PCGUnknownNodePolicy::PreserveInert,
                    .limits = catalog.limits,
                    .supportedNodeTypes = catalog.types};
        }
    }  // namespace

    /** @brief Finite synchronous owner sessions; roots/history remain independent from panel placement. */
    struct PCGGraphDocumentService::State final {
        struct Entry final {
            Assets::AssetId asset;
            DocumentIdentity identity;
            std::filesystem::path path;
            PCGGraphDocument document;
            std::optional<DocumentRecoveryRecord> acceptedRecovery;
        };

        std::filesystem::path root;
        Assets::AssetId owner;
        PCGDocumentCatalog catalog;
        const ProjectMutationCoordinator *mutations{};
        DurableFileSystem *files{};
        std::vector<Entry> documents;
        std::uint64_t nextSession{1};
        std::thread::id thread{std::this_thread::get_id()};
        bool closed{};
        bool operationActive{};
        const PCG::IPCGGraphSourceMigrator *migrator{};

        /** @brief Reconciles one exact record without relaxing dirty-state or owner fences. */
        Result<void> ReconcileEntry(DocumentRecoveryService &recovery, const Entry &entry, bool preserveRecovery, bool discardDirty) const {
            const auto snapshot = entry.document.Snapshot().Value();
            auto record = recovery.Inspect(entry.asset, entry.identity.key);
            if (record.HasError())
                return Result<void>::Failure(record.ErrorValue());
            const bool requireCurrent = snapshot.dirty && preserveRecovery && !discardDirty;
            if (!record.Value())
                return requireCurrent ? Failure<void>(Reason::Stale) : Result<void>::Success();
            const auto &candidate = *record.Value();
            const bool currentSession =
                candidate.sessionOwner == snapshot.session.owner && candidate.sessionGeneration == snapshot.session.generation;
            if (requireCurrent &&
                (!currentSession || candidate.state != snapshot.state || candidate.acceptedSource != snapshot.acceptedSource))
                return Failure<void>(Reason::Stale);
            const bool provenSaved = !snapshot.dirty && candidate.state == snapshot.state;
            if (const bool acceptedCandidate = entry.acceptedRecovery && candidate == *entry.acceptedRecovery;
                provenSaved || (discardDirty && (currentSession || acceptedCandidate)))
                return recovery.Discard(candidate);
            return Result<void>::Success();
        }

        /** @brief Reconciles only current or explicitly accepted recovery while the shutdown operation guard is held. */
        Result<void> ReconcileRecovery(bool preserveRecovery, bool discardDirty) {
            if (!mutations || !files)
                return Result<void>::Success();
            DocumentRecoveryService recovery(root, *mutations, *files);
            for (const auto &entry : documents)
                if (const auto reconciled = ReconcileEntry(recovery, entry, preserveRecovery, discardDirty); reconciled.HasError())
                    return reconciled;
            return Result<void>::Success();
        }

        /** @brief Resolves only a live exact route in this owner incarnation. */
        Entry *Find(DocumentInstanceId instance) {
            const auto found = std::ranges::find(documents, instance, [](const Entry &entry) {
                return entry.identity.instance;
            });
            return found == documents.end() ? nullptr : std::to_address(found);
        }
    };

    /** @copydoc PCGGraphDocumentService::PCGGraphDocumentService */
    PCGGraphDocumentService::PCGGraphDocumentService(std::filesystem::path root, Assets::AssetId owner, PCGDocumentCatalog catalog,
                                                     const ProjectMutationCoordinator *mutations, DurableFileSystem *files,
                                                     const PCG::IPCGGraphSourceMigrator *migrator)
        : state_(std::make_unique<State>()) {
        state_->root = std::move(root);
        state_->owner = owner;
        state_->catalog = std::move(catalog);
        state_->mutations = mutations;
        state_->files = files;
        state_->migrator = migrator;
        state_->documents.reserve(MaximumDocuments);
    }

    PCGGraphDocumentService::~PCGGraphDocumentService() = default;

    /** @copydoc PCGGraphDocumentService::CheckAccess */
    Result<void> PCGGraphDocumentService::CheckAccess() const {
        if (state_->closed)
            return Failure<void>(Reason::Closed);
        if (state_->thread != std::this_thread::get_id())
            return Failure<void>(Reason::WrongThread);
        return Result<void>::Success();
    }

    /** @copydoc PCGGraphDocumentService::Open */
    Result<PCGDocumentSnapshot> PCGGraphDocumentService::Open(Assets::AssetId asset, DocumentIdentity identity,
                                                              const std::filesystem::path &path,
                                                              const PCG::IPCGGraphSourceMigrator *migrator) {
        using enum PCGDocumentCommandFailureReason;
        if (auto access = CheckAccess(); access.HasError())
            return Result<PCGDocumentSnapshot>::Failure(access.ErrorValue());
        if (state_->operationActive)
            return Failure<PCGDocumentSnapshot>(Busy);
        if (!asset.IsValid() || !identity.IsValid() || identity.key.kind != DocumentKind::PCGGraph ||
            path != state_->root / identity.key.source.Value())
            return Failure<PCGDocumentSnapshot>(Invalid);
        for (const auto &entry : state_->documents) {
            if (entry.asset == asset)
                return entry.document.Snapshot();
            if (entry.identity.instance == identity.instance || entry.identity.key == identity.key)
                return Failure<PCGDocumentSnapshot>(Conflict);
        }
        if (!state_->owner.IsValid())
            return Failure<PCGDocumentSnapshot>(Unauthorized);
        if (state_->documents.size() == MaximumDocuments)
            return Failure<PCGDocumentSnapshot>(Limit);
        if (state_->nextSession == std::numeric_limits<std::uint64_t>::max())
            return Failure<PCGDocumentSnapshot>(Exhausted);
        auto capture = Assets::CaptureAssetSource(state_->root, path, MaximumSourceBytes);
        if (capture.HasError())
            return Result<PCGDocumentSnapshot>::Failure(capture.ErrorValue());
        const DocumentSessionId session{state_->owner, state_->nextSession};
        auto document =
            PCGGraphDocument::Open(asset, identity, session, capture.Value(), state_->catalog, {}, migrator ? migrator : state_->migrator);
        if (document.HasError())
            return Result<PCGDocumentSnapshot>::Failure(document.ErrorValue());
        const auto graphId = document.Value().Snapshot().Value().acceptedGeneration.graph;
        for (const auto &entry : state_->documents)
            if (entry.document.Snapshot().Value().acceptedGeneration.graph == graphId)
                return Failure<PCGDocumentSnapshot>(Conflict);
        auto snapshot = document.Value().Snapshot();
        state_->documents.push_back({asset, std::move(identity), path, std::move(document).Value(), {}});
        ++state_->nextSession;
        return snapshot;
    }

    /** @copydoc PCGGraphDocumentService::Snapshot */
    Result<PCGDocumentSnapshot> PCGGraphDocumentService::Snapshot(DocumentInstanceId instance) const {
        if (auto access = CheckAccess(); access.HasError())
            return Result<PCGDocumentSnapshot>::Failure(access.ErrorValue());
        const auto *entry = state_->Find(instance);
        return entry ? entry->document.Snapshot() : Failure<PCGDocumentSnapshot>(Reason::Stale);
    }

    /** @copydoc PCGGraphDocumentService::Snapshots */
    Result<std::vector<PCGDocumentSnapshot>> PCGGraphDocumentService::Snapshots() const {
        if (auto access = CheckAccess(); access.HasError())
            return Result<std::vector<PCGDocumentSnapshot>>::Failure(access.ErrorValue());
        std::vector<PCGDocumentSnapshot> snapshots;
        snapshots.reserve(state_->documents.size());
        for (const auto &entry : state_->documents) {
            auto snapshot = entry.document.Snapshot();
            if (snapshot.HasError())
                return Result<std::vector<PCGDocumentSnapshot>>::Failure(snapshot.ErrorValue());
            snapshots.push_back(std::move(snapshot).Value());
        }
        return Result<std::vector<PCGDocumentSnapshot>>::Success(std::move(snapshots));
    }

    /** @copydoc PCGGraphDocumentService::Execute */
    Result<void> PCGGraphDocumentService::Execute(DocumentInstanceId instance, const PCGDocumentCommand &command) {
        if (auto access = CheckAccess(); access.HasError())
            return access;
        auto *entry = state_->Find(instance);
        return entry ? entry->document.Execute(command) : Failure<void>(Reason::Stale);
    }

    /** @copydoc PCGGraphDocumentService::Replay */
    Result<void> PCGGraphDocumentService::Replay(DocumentInstanceId instance, const PCGDocumentCommand &command, bool redo) {
        if (auto access = CheckAccess(); access.HasError())
            return access;
        auto *entry = state_->Find(instance);
        return entry ? entry->document.Replay(command, redo) : Failure<void>(Reason::Stale);
    }

    /** @copydoc PCGGraphDocumentService::Save */
    Result<Assets::AssetSourcePublication> PCGGraphDocumentService::Save(DocumentInstanceId instance, const PCGDocumentCommand &command) {
        using enum PCGDocumentCommandFailureReason;
        if (auto access = CheckAccess(); access.HasError())
            return Result<Assets::AssetSourcePublication>::Failure(access.ErrorValue());
        if (state_->operationActive)
            return Failure<Assets::AssetSourcePublication>(Busy);
        OperationGuard guard(state_->operationActive);
        auto *entry = state_->Find(instance);
        if (!entry)
            return Failure<Assets::AssetSourcePublication>(Stale);
        if (!state_->mutations || !state_->files)
            return Failure<Assets::AssetSourcePublication>(Unauthorized);
        auto save = entry->document.CaptureSave(command);
        if (save.HasError())
            return Result<Assets::AssetSourcePublication>::Failure(save.ErrorValue());
        return entry->document.Save(save.Value(), state_->root, entry->path, *state_->mutations, *state_->files, command.cancellation);
    }

    /** @copydoc PCGGraphDocumentService::InspectExternal */
    Result<PCGDocumentSnapshot> PCGGraphDocumentService::InspectExternal(DocumentInstanceId instance) {
        if (auto access = CheckAccess(); access.HasError())
            return Result<PCGDocumentSnapshot>::Failure(access.ErrorValue());
        auto *entry = state_->Find(instance);
        if (!entry)
            return Failure<PCGDocumentSnapshot>(Reason::Stale);
        auto capture = Assets::CaptureAssetSource(state_->root, entry->path, MaximumSourceBytes);
        if (capture.HasError())
            return Result<PCGDocumentSnapshot>::Failure(capture.ErrorValue());
        if (auto observed = entry->document.ObserveExternal(capture.Value().identity); observed.HasError())
            return Result<PCGDocumentSnapshot>::Failure(observed.ErrorValue());
        return entry->document.Snapshot();
    }

    /** @copydoc PCGGraphDocumentService::Reload */
    Result<void> PCGGraphDocumentService::Reload(DocumentInstanceId instance, const PCGDocumentCommand &command, bool discardDirty,
                                                 const PCG::IPCGGraphSourceMigrator *migrator) {
        if (auto access = CheckAccess(); access.HasError())
            return access;
        if (state_->operationActive)
            return Failure<void>(Reason::Busy);
        auto *entry = state_->Find(instance);
        if (!entry)
            return Failure<void>(Reason::Stale);
        auto capture = Assets::CaptureAssetSource(state_->root, entry->path, MaximumSourceBytes, command.cancellation);
        if (capture.HasError())
            return Result<void>::Failure(capture.ErrorValue());
        return entry->document.Reload(command, capture.Value(), discardDirty, migrator ? migrator : state_->migrator);
    }

    /** @copydoc PCGGraphDocumentService::Autosave */
    Result<void> PCGGraphDocumentService::Autosave(DocumentInstanceId instance, const PCGDocumentCommand &command) {
        if (auto access = CheckAccess(); access.HasError())
            return access;
        if (state_->operationActive)
            return Failure<void>(Reason::Busy);
        OperationGuard guard(state_->operationActive);
        auto *entry = state_->Find(instance);
        if (!entry)
            return Failure<void>(Reason::Stale);
        if (!state_->mutations || !state_->files)
            return Failure<void>(Reason::Unauthorized);
        auto snapshot = entry->document.Snapshot();
        if (snapshot.HasError())
            return Result<void>::Failure(snapshot.ErrorValue());
        if (!snapshot.Value().dirty)
            return Result<void>::Success();
        // Recovery must preserve dirty conflicted intent as well; it never grants canonical source publication.
        auto encoded = PCG::SerializePCGGraphAsset(*snapshot.Value().source, MaximumSourceBytes);
        if (encoded.HasError())
            return Result<void>::Failure(encoded.ErrorValue());
        if (auto admitted = entry->document.Admit(command); admitted.HasError())
            return admitted;
        const auto &captured = snapshot.Value();
        const auto timestamp =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        DocumentRecoveryRecord record{captured.asset,
                                      captured.identity.key,
                                      captured.session.owner,
                                      captured.session.generation,
                                      captured.revision,
                                      static_cast<std::uint64_t>(timestamp),
                                      captured.acceptedSource,
                                      captured.state,
                                      {}};
        const auto bytes = std::as_bytes(std::span(encoded.Value()));
        record.payload.assign(bytes.begin(), bytes.end());
        DocumentRecoveryService recovery(state_->root, *state_->mutations, *state_->files);
        auto written = entry->acceptedRecovery ? recovery.ReplaceAccepted(record, *entry->acceptedRecovery, command.cancellation)
                                               : recovery.Write(record, command.cancellation);
        if (written.HasValue())
            entry->acceptedRecovery.reset();
        return written;
    }

    /** @copydoc PCGGraphDocumentService::InspectRecovery */
    Result<std::optional<PCGRecoveryCandidate>> PCGGraphDocumentService::InspectRecovery(
        DocumentInstanceId instance, const PCG::IPCGGraphSourceMigrator *migrator) const {
        if (auto access = CheckAccess(); access.HasError())
            return Result<std::optional<PCGRecoveryCandidate>>::Failure(access.ErrorValue());
        const auto *entry = state_->Find(instance);
        if (!entry)
            return Failure<std::optional<PCGRecoveryCandidate>>(Reason::Stale);
        if (!state_->mutations || !state_->files)
            return Failure<std::optional<PCGRecoveryCandidate>>(Reason::Unauthorized);
        DocumentRecoveryService recovery(state_->root, *state_->mutations, *state_->files);
        auto record = recovery.Inspect(entry->asset, entry->identity.key);
        if (record.HasError())
            return Result<std::optional<PCGRecoveryCandidate>>::Failure(record.ErrorValue());
        if (!record.Value())
            return Result<std::optional<PCGRecoveryCandidate>>::Success(std::nullopt);
        const auto &bytes = record.Value()->payload;
        auto parsed = PCG::DeserializePCGGraphAsset({std::bit_cast<const std::uint8_t *>(bytes.data()), bytes.size()},
                                                    Context(state_->catalog), migrator ? migrator : state_->migrator);
        if (parsed.HasError())
            return Result<std::optional<PCGRecoveryCandidate>>::Failure(parsed.ErrorValue());
        if (parsed.Value().Data().generation.graph != entry->document.Snapshot().Value().acceptedGeneration.graph)
            return Failure<std::optional<PCGRecoveryCandidate>>(Reason::Conflict);
        return Result<std::optional<PCGRecoveryCandidate>>::Success(
            PCGRecoveryCandidate{std::move(record).Value().value(), std::make_shared<const PCG::PCGGraphAsset>(std::move(parsed).Value())});
    }

    /** @copydoc PCGGraphDocumentService::AcceptRecovery */
    Result<void> PCGGraphDocumentService::AcceptRecovery(DocumentInstanceId instance, const PCGDocumentCommand &command,
                                                         const PCGRecoveryCandidate &candidate) {
        using enum PCGDocumentCommandFailureReason;
        if (auto access = CheckAccess(); access.HasError())
            return access;
        if (state_->operationActive)
            return Failure<void>(Busy);
        auto *entry = state_->Find(instance);
        if (!entry)
            return Failure<void>(Stale);
        if (!candidate.source || candidate.record.asset != entry->asset || candidate.record.document != entry->identity.key)
            return Failure<void>(Invalid);
        if (!state_->mutations || !state_->files)
            return Failure<void>(Unauthorized);
        auto encoded = PCG::SerializePCGGraphAsset(*candidate.source, MaximumSourceBytes);
        if (encoded.HasError())
            return Result<void>::Failure(encoded.ErrorValue());
        if (!std::ranges::equal(std::as_bytes(std::span(encoded.Value())), candidate.record.payload))
            return Failure<void>(Invalid);
        DocumentRecoveryService recovery(state_->root, *state_->mutations, *state_->files);
        const auto currentRecord = recovery.Inspect(entry->asset, entry->identity.key);
        if (currentRecord.HasError())
            return Result<void>::Failure(currentRecord.ErrorValue());
        if (!currentRecord.Value() || *currentRecord.Value() != candidate.record)
            return Failure<void>(Conflict);
        auto capture = Assets::CaptureAssetSource(state_->root, entry->path, MaximumSourceBytes, command.cancellation);
        if (capture.HasError())
            return Result<void>::Failure(capture.ErrorValue());
        if (capture.Value().identity != candidate.record.acceptedSource)
            return Failure<void>(Conflict);
        std::optional<DocumentRecoveryRecord> accepted{candidate.record};
        auto restored = entry->document.AcceptRecovery(command, *candidate.source, candidate.record.acceptedSource);
        if (restored.HasValue())
            entry->acceptedRecovery.swap(accepted);
        return restored;
    }

    /** @copydoc PCGGraphDocumentService::Close */
    Result<void> PCGGraphDocumentService::Close(DocumentInstanceId instance, bool discardDirty) {
        if (auto access = CheckAccess(); access.HasError())
            return access;
        if (state_->operationActive)
            return Failure<void>(Reason::Busy);
        OperationGuard guard(state_->operationActive);
        auto *entry = state_->Find(instance);
        if (!entry)
            return Failure<void>(Reason::Stale);
        const auto snapshot = entry->document.Snapshot();
        if (snapshot.HasError())
            return Result<void>::Failure(snapshot.ErrorValue());
        if (snapshot.Value().dirty && !discardDirty)
            return Failure<void>(Reason::Dirty);
        if (discardDirty && state_->mutations && state_->files) {
            DocumentRecoveryService recovery(state_->root, *state_->mutations, *state_->files);
            auto record = recovery.Inspect(entry->asset, entry->identity.key);
            if (record.HasError())
                return Result<void>::Failure(record.ErrorValue());
            if (record.Value() && ((record.Value()->sessionOwner == snapshot.Value().session.owner &&
                                    record.Value()->sessionGeneration == snapshot.Value().session.generation) ||
                                   (entry->acceptedRecovery && *record.Value() == *entry->acceptedRecovery))) {
                if (auto discarded = recovery.Discard(*record.Value()); discarded.HasError())
                    return discarded;
            }
        }
        if (entry->document.Snapshot().Value().revision != snapshot.Value().revision)
            return Failure<void>(Reason::Stale);
        if (auto closed = entry->document.Close(discardDirty); closed.HasError())
            return closed;
        std::erase_if(state_->documents, [instance](const State::Entry &value) {
            return value.identity.instance == instance;
        });
        return Result<void>::Success();
    }

    /** @copydoc PCGGraphDocumentService::FlushRecovery */
    Result<void> PCGGraphDocumentService::FlushRecovery() {
        if (auto access = CheckAccess(); access.HasError())
            return access;
        std::optional<Error> firstError;
        for (const auto &entry : state_->documents) {
            auto snapshot = entry.document.Snapshot();
            if (snapshot.HasError()) {
                if (!firstError)
                    firstError = snapshot.ErrorValue();
                continue;
            }
            auto saved = Autosave(entry.identity.instance, Fence(snapshot.Value()));
            if (saved.HasError() && !firstError)
                firstError = saved.ErrorValue();
        }
        return firstError ? Result<void>::Failure(*firstError) : Result<void>::Success();
    }

    /** @copydoc PCGGraphDocumentService::Shutdown */
    Result<void> PCGGraphDocumentService::Shutdown(bool preserveRecovery, bool discardDirty) {
        if (state_->closed)
            return Result<void>::Success();
        if (auto access = CheckAccess(); access.HasError())
            return access;
        if (state_->operationActive)
            return Failure<void>(Reason::Busy);
        if (preserveRecovery) {
            if (auto recovery = FlushRecovery(); recovery.HasError())
                return recovery;
        }
        OperationGuard guard(state_->operationActive);
        auto captured = Snapshots();
        if (captured.HasError())
            return Result<void>::Failure(captured.ErrorValue());
        for (const auto &entry : state_->documents) {
            auto snapshot = entry.document.Snapshot();
            if (snapshot.HasError())
                return Result<void>::Failure(snapshot.ErrorValue());
            if (snapshot.Value().dirty && !preserveRecovery && !discardDirty)
                return Failure<void>(Reason::Dirty);
        }
        if (auto reconciled = state_->ReconcileRecovery(preserveRecovery, discardDirty); reconciled.HasError())
            return reconciled;
        for (const auto &snapshot : captured.Value()) {
            const auto current = Snapshot(snapshot.identity.instance);
            if (current.HasError())
                return Result<void>::Failure(current.ErrorValue());
            if (current.Value().revision != snapshot.revision)
                return Failure<void>(Reason::Stale);
        }
        state_->closed = true;
        state_->documents.clear();
        return Result<void>::Success();
    }

    /** @copydoc PCGGraphDocumentService::ReplaceCatalog */
    Result<void> PCGGraphDocumentService::ReplaceCatalog(PCGDocumentCatalog catalog) {
        if (auto access = CheckAccess(); access.HasError())
            return access;
        if (state_->operationActive)
            return Failure<void>(Reason::Busy);
        if (catalog.revision <= state_->catalog.revision)
            return Failure<void>(Reason::Stale);
        if (auto validated = PCGGraphDocument::ValidateCatalog(catalog); validated.HasError())
            return validated;
        std::vector<PCGGraphDocument::CatalogReplacement> replacements;
        replacements.reserve(state_->documents.size());
        for (const auto &entry : state_->documents) {
            auto replacement = entry.document.PrepareCatalog(catalog);
            if (replacement.HasError())
                return Result<void>::Failure(replacement.ErrorValue());
            replacements.push_back(std::move(replacement).Value());
        }
        for (std::size_t index = 0; index < replacements.size(); ++index)
            state_->documents[index].document.CommitCatalog(replacements[index]);
        std::swap(state_->catalog, catalog);
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
