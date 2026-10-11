#include "Horo/Editor/PCGGraphDocument.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <thread>

namespace Horo::Editor {
    namespace {
        using Reason = PCGDocumentCommandFailureReason;

        /** @brief Holds reentrant publication exclusion until the complete typed save boundary returns. */
        struct PublicationGuard final {
            bool &publishing;

            explicit PublicationGuard(bool &value) : publishing(value) {
                value = true;
            }

            PublicationGuard(const PublicationGuard &) = delete;
            PublicationGuard &operator=(const PublicationGuard &) = delete;
            PublicationGuard(PublicationGuard &&) = delete;
            PublicationGuard &operator=(PublicationGuard &&) = delete;

            ~PublicationGuard() {
                publishing = false;
            }
        };

        /** @brief Projects one owned catalog into the existing canonical codec without ambient discovery. */
        PCG::PCGGraphSourceContext Context(const PCGDocumentCatalog &catalog) {
            return {.tier = catalog.tier,
                    .unknownNodePolicy = PCG::PCGUnknownNodePolicy::PreserveInert,
                    .limits = catalog.limits,
                    .supportedNodeTypes = catalog.types};
        }

        /** @brief Reinterprets owned format bytes without allocating or changing their representation. */
        std::span<const std::uint8_t> SourceBytes(const Assets::AssetSourceCapture &capture) {
            return {std::bit_cast<const std::uint8_t *>(capture.bytes.data()), capture.bytes.size()};
        }

        /** @brief Hashes canonical authored semantics independently from durable publication revision. */
        Result<Sha256Digest> StateIdentity(const PCG::PCGGraphAsset &asset, const PCGDocumentCatalog &catalog) {
            auto data = asset.Data();
            data.generation.revision = PCG::GraphRevision::Create(1).Value();
            auto normalized = PCG::PCGGraphAsset::Create(std::move(data), Context(catalog));
            if (normalized.HasError())
                return Result<Sha256Digest>::Failure(normalized.ErrorValue());
            auto encoded = PCG::SerializePCGGraphAsset(normalized.Value());
            if (encoded.HasError())
                return Result<Sha256Digest>::Failure(encoded.ErrorValue());
            return Result<Sha256Digest>::Success(ComputeSha256(std::as_bytes(std::span(encoded.Value()))));
        }

        /** @brief Validates the bounded owned projection before copying untrusted catalogs. */
        bool ValidCatalog(const PCGDocumentCatalog &catalog) {
            return catalog.revision != 0 && catalog.types.size() <= PCG::PCGGraphSourceHardLimits::NodeTypes;
        }

        /** @brief Exact semantic values keyed by stable authored identity. */
        template <typename T> struct Change final {
            std::optional<T> before;
            std::optional<T> after;
        };

        /** @brief Exact source-only delta; no complete graph, plans, evaluators or runtime objects. */
        struct GraphDelta final {
            std::vector<Change<PCG::PCGGraphNode>> nodes;
            std::vector<Change<PCG::PCGGraphEdge>> edges;
            std::vector<Change<PCG::PCGExposedInput>> inputs;
            std::optional<std::pair<SetPCGGraphPolicy, SetPCGGraphPolicy>> policy;
            Sha256Digest beforeState;
            Sha256Digest afterState;
            std::size_t bytes{sizeof(GraphDelta)};
        };

        /** @brief Accounts all domain-owned variable storage in retained node deltas. */
        std::size_t ExtraBytes(const PCG::PCGGraphNode &value) {
            return value.pins.capacity() * sizeof(PCG::PCGGraphPin) + value.payload.capacity();
        }

        /** @brief Edges contain only fixed-width inline semantic values. */
        std::size_t ExtraBytes(const PCG::PCGGraphEdge &) {
            return 0;
        }

        /** @brief Accounts retained exposed binding keys. */
        std::size_t ExtraBytes(const PCG::PCGExposedInput &value) {
            return value.key.capacity();
        }

        /** @brief Captures only changed stable elements and charges their complete owned representation. */
        template <typename T>
        std::vector<Change<T>> Changes(const std::vector<T> &before, const std::vector<T> &after, std::size_t &bytes) {
            std::vector<Change<T>> changes;
            for (const auto &value : before) {
                const auto found = std::ranges::find(after, value.id, &T::id);
                if (found != after.end() && *found == value)
                    continue;
                Change<T> change{value, {}};
                if (found != after.end())
                    change.after = *found;
                changes.push_back(std::move(change));
            }
            for (const auto &value : after)
                if (std::ranges::find(before, value.id, &T::id) == before.end())
                    changes.push_back({{}, value});
            bytes += changes.capacity() * sizeof(Change<T>);
            for (const auto &change : changes) {
                if (change.before)
                    bytes += ExtraBytes(*change.before);
                if (change.after)
                    bytes += ExtraBytes(*change.after);
            }
            return changes;
        }

        /** @brief Replaces one semantic element in detached storage. */
        template <typename T> void Put(std::vector<T> &values, const T &value) {
            const auto found = std::ranges::find(values, value.id, &T::id);
            if (found == values.end())
                values.push_back(value);
            else
                *found = value;
        }

        /** @brief Removes an exact semantic identity; absent removal is invalid intent. */
        template <typename T, typename Id> bool Remove(std::vector<T> &values, Id id) {
            return std::erase_if(values, [id](const T &value) {
                return value.id == id;
            }) != 0;
        }

        /** @brief Applies retained before/after values to detached source without re-evaluating nodes. */
        template <typename T> void ApplyChanges(std::vector<T> &values, const std::vector<Change<T>> &changes, bool redo) {
            for (const auto &change : changes) {
                const auto &value = redo ? change.after : change.before;
                if (value)
                    Put(values, *value);
                else
                    static_cast<void>(Remove(values, change.before ? change.before->id : change.after->id));
            }
        }

        /** @brief Checks owned operation size before copying into a candidate. */
        bool BoundedOperation(const PCGDocumentOperation &operation) {
            if (const auto *node = std::get_if<PutPCGNode>(&operation))
                return node->value.pins.size() <= PCG::PCGGraphSourceHardLimits::PinsPerNode &&
                       node->value.payload.size() <= PCG::PCGGraphSourceHardLimits::NodePayloadBytes;
            if (const auto *input = std::get_if<PutPCGInput>(&operation))
                return input->value.key.size() <= PCG::PCGGraphSourceHardLimits::IdentifierBytes;
            return true;
        }

        /** @brief Dispatches the one closed semantic command vocabulary into private candidate storage. */
        Result<void> ApplyOperation(PCG::PCGGraphSourceData &data, const PCGDocumentOperation &operation) {
            return std::visit([&]<typename T>(const T &value) {
                bool valid = true;
                if constexpr (std::is_same_v<T, SetPCGGraphPolicy>) {
                    data.mode = value.mode;
                    data.deterministicSeed = value.seed;
                } else if constexpr (std::is_same_v<T, PutPCGNode>)
                    Put(data.nodes, value.value);
                else if constexpr (std::is_same_v<T, PutPCGEdge>)
                    Put(data.edges, value.value);
                else if constexpr (std::is_same_v<T, PutPCGInput>)
                    Put(data.exposedInputs, value.value);
                else if constexpr (std::is_same_v<T, RemovePCGNode>)
                    valid = Remove(data.nodes, value.id);
                else if constexpr (std::is_same_v<T, RemovePCGEdge>)
                    valid = Remove(data.edges, value.id);
                else if constexpr (std::is_same_v<T, RemovePCGInput>)
                    valid = Remove(data.exposedInputs, value.id);
                return valid ? Result<void>::Success() : Result<void>::Failure(MakeError(PCGDocumentError(Reason::Invalid)));
            }, operation);
        }
    }  // namespace

    /** @brief Single owner-thread authority and shared reversible history; save admission is explicit. */
    struct PCGGraphDocument::State final {
        Assets::AssetId asset;
        DocumentIdentity identity;
        DocumentSessionId session;
        std::thread::id owner{std::this_thread::get_id()};
        std::shared_ptr<const PCG::PCGGraphAsset> source;
        PCGDocumentCatalog catalog;
        SemanticDocumentHistory<GraphDelta> history;
        Assets::AssetSourceIdentity acceptedSource;
        PCG::GraphGeneration acceptedGeneration;
        Sha256Digest currentState;
        Sha256Digest savedState;
        std::uint64_t revision{1};
        std::uint64_t replacementEpoch{1};
        bool conflict{};
        bool publishing{};
        bool closed{};

        explicit State(SemanticHistoryLimits limits) : history(limits) {}
    };

    PCGGraphDocument::PCGGraphDocument(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

    PCGGraphDocument::~PCGGraphDocument() = default;
    PCGGraphDocument::PCGGraphDocument(PCGGraphDocument &&) noexcept = default;
    PCGGraphDocument &PCGGraphDocument::operator=(PCGGraphDocument &&) noexcept = default;

    /** @copydoc PCGGraphDocument::Open */
    Result<PCGGraphDocument> PCGGraphDocument::Open(Assets::AssetId asset, DocumentIdentity identity, DocumentSessionId session,
                                                    const Assets::AssetSourceCapture &capture, PCGDocumentCatalog catalog,
                                                    SemanticHistoryLimits history, const PCG::IPCGGraphSourceMigrator *migrator) {
        if (!asset.IsValid() || !identity.IsValid() || identity.key.kind != DocumentKind::PCGGraph || !session.IsValid() ||
            !capture.identity.exists || capture.identity.bytes != capture.bytes.size() ||
            capture.identity.digest != ComputeSha256(capture.bytes) || !ValidCatalog(catalog))
            return Result<PCGGraphDocument>::Failure(MakeError(PCGDocumentError(Reason::Invalid)));
        if (history.maximumEntries == 0 || history.maximumEntries > 128 || history.maximumBytes == 0 ||
            history.maximumBytes > 64 * 1024 * 1024)
            return Result<PCGGraphDocument>::Failure(MakeError(PCGDocumentError(Reason::Limit)));
        auto parsed = PCG::DeserializePCGGraphAsset(SourceBytes(capture), Context(catalog), migrator);
        if (parsed.HasError())
            return Result<PCGGraphDocument>::Failure(parsed.ErrorValue());
        auto digest = StateIdentity(parsed.Value(), catalog);
        if (digest.HasError())
            return Result<PCGGraphDocument>::Failure(digest.ErrorValue());
        auto state = std::make_unique<State>(history);
        state->asset = asset;
        state->identity = std::move(identity);
        state->session = session;
        state->source = std::make_shared<const PCG::PCGGraphAsset>(std::move(parsed).Value());
        state->catalog = std::move(catalog);
        state->acceptedSource = capture.identity;
        state->acceptedGeneration = state->source->Data().generation;
        state->currentState = digest.Value();
        if (const auto version = PCG::InspectPCGGraphSchemaVersion(SourceBytes(capture));
            version.HasValue() && version.Value() == PCG::CurrentPCGGraphSchemaVersion)
            state->savedState = state->currentState;
        return Result<PCGGraphDocument>::Success(PCGGraphDocument(std::move(state)));
    }

    /** @copydoc PCGGraphDocument::CheckAccess */
    Result<void> PCGGraphDocument::CheckAccess() const {
        if (!state_ || state_->closed)
            return Result<void>::Failure(MakeError(PCGDocumentError(Reason::Closed)));
        if (state_->owner != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(PCGDocumentError(Reason::WrongThread)));
        return Result<void>::Success();
    }

    /** @copydoc PCGGraphDocument::Admit */
    Result<void> PCGGraphDocument::Admit(const PCGDocumentCommand &command) const {
        using enum PCGDocumentCommandFailureReason;
        if (auto access = CheckAccess(); access.HasError())
            return access;
        if (command.session != state_->session || command.expectedRevision != state_->revision ||
            command.catalogRevision != state_->catalog.revision)
            return Result<void>::Failure(MakeError(PCGDocumentError(Stale)));
        if (!command.canEdit)
            return Result<void>::Failure(MakeError(PCGDocumentError(Unauthorized)));
        if (command.cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(PCGDocumentError(Cancelled)));
        if (state_->revision == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(PCGDocumentError(Exhausted)));
        return Result<void>::Success();
    }

    /** @copydoc PCGGraphDocument::Snapshot */
    Result<PCGDocumentSnapshot> PCGGraphDocument::Snapshot() const {
        if (auto access = CheckAccess(); access.HasError())
            return Result<PCGDocumentSnapshot>::Failure(access.ErrorValue());
        return Result<PCGDocumentSnapshot>::Success({state_->asset, state_->identity, state_->session, state_->revision,
                                                     state_->catalog.revision, state_->currentState, state_->acceptedSource,
                                                     state_->acceptedGeneration, state_->source, state_->currentState != state_->savedState,
                                                     state_->conflict});
    }

    /** @copydoc PCGGraphDocument::Commit */
    Result<void> PCGGraphDocument::Commit(PCG::PCGGraphSourceData candidate, CancellationToken cancellation) {
        auto validated = PCG::PCGGraphAsset::Create(std::move(candidate), Context(state_->catalog));
        if (validated.HasError())
            return Result<void>::Failure(validated.ErrorValue());
        if (validated.Value().Data() == state_->source->Data())
            return Result<void>::Success();
        auto digest = StateIdentity(validated.Value(), state_->catalog);
        if (digest.HasError())
            return Result<void>::Failure(digest.ErrorValue());
        GraphDelta delta;
        const auto &before = state_->source->Data();
        const auto &after = validated.Value().Data();
        delta.nodes = Changes(before.nodes, after.nodes, delta.bytes);
        delta.edges = Changes(before.edges, after.edges, delta.bytes);
        delta.inputs = Changes(before.exposedInputs, after.exposedInputs, delta.bytes);
        if (before.mode != after.mode || before.deterministicSeed != after.deterministicSeed)
            delta.policy =
                std::pair{SetPCGGraphPolicy{before.mode, before.deterministicSeed}, SetPCGGraphPolicy{after.mode, after.deterministicSeed}};
        delta.beforeState = state_->currentState;
        delta.afterState = digest.Value();
        auto history = state_->history.Prepare(std::move(delta));
        if (!history)
            return Result<void>::Failure(MakeError(PCGDocumentError(Reason::HistoryLimit)));
        auto source = std::make_shared<const PCG::PCGGraphAsset>(std::move(validated).Value());
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(PCGDocumentError(Reason::Cancelled)));
        state_->source.swap(source);
        state_->history.Commit(*history);
        state_->currentState = digest.Value();
        ++state_->revision;
        return Result<void>::Success();
    }

    /** @copydoc PCGGraphDocument::Execute */
    Result<void> PCGGraphDocument::Execute(const PCGDocumentCommand &command) {
        if (auto admitted = Admit(command); admitted.HasError())
            return admitted;
        if (command.operations.empty() || command.operations.size() > 256)
            return Result<void>::Failure(MakeError(PCGDocumentError(Reason::Limit)));
        for (const auto &operation : command.operations)
            if (!BoundedOperation(operation))
                return Result<void>::Failure(MakeError(PCGDocumentError(Reason::Limit)));
        auto candidate = state_->source->Data();
        for (const auto &operation : command.operations)
            if (auto applied = ApplyOperation(candidate, operation); applied.HasError())
                return applied;
        return Commit(std::move(candidate), command.cancellation);
    }

    /** @copydoc PCGGraphDocument::Replay */
    Result<void> PCGGraphDocument::Replay(const PCGDocumentCommand &fence, bool redo) {
        if (auto admitted = Admit(fence); admitted.HasError())
            return admitted;
        if (!fence.operations.empty())
            return Result<void>::Failure(MakeError(PCGDocumentError(Reason::Invalid)));
        const auto *entry = state_->history.Next(redo);
        if (!entry)
            return Result<void>::Failure(MakeError(PCGDocumentError(Reason::Invalid)));
        auto candidate = state_->source->Data();
        ApplyChanges(candidate.nodes, entry->nodes, redo);
        ApplyChanges(candidate.edges, entry->edges, redo);
        ApplyChanges(candidate.exposedInputs, entry->inputs, redo);
        if (entry->policy) {
            const auto policy = redo ? entry->policy->second : entry->policy->first;
            candidate.mode = policy.mode;
            candidate.deterministicSeed = policy.seed;
        }
        auto validated = PCG::PCGGraphAsset::Create(std::move(candidate), Context(state_->catalog));
        if (validated.HasError())
            return Result<void>::Failure(validated.ErrorValue());
        auto source = std::make_shared<const PCG::PCGGraphAsset>(std::move(validated).Value());
        if (fence.cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(PCGDocumentError(Reason::Cancelled)));
        state_->source.swap(source);
        state_->currentState = redo ? entry->afterState : entry->beforeState;
        state_->history.DidReplay(redo);
        ++state_->revision;
        return Result<void>::Success();
    }

    /** @copydoc PCGGraphDocument::ValidateCatalog */
    Result<void> PCGGraphDocument::ValidateCatalog(const PCGDocumentCatalog &catalog) {
        if (!ValidCatalog(catalog))
            return Result<void>::Failure(MakeError(PCGDocumentError(Reason::Invalid)));
        PCG::PCGGraphSourceData empty;
        empty.generation = {PCG::GraphId::Create(1).Value(), PCG::GraphRevision::Create(1).Value()};
        const auto validated = PCG::PCGGraphAsset::Create(std::move(empty), Context(catalog));
        return validated.HasError() ? Result<void>::Failure(validated.ErrorValue()) : Result<void>::Success();
    }

    /** @copydoc PCGGraphDocument::CaptureSave */
    Result<PCGDocumentSave> PCGGraphDocument::CaptureSave(const PCGDocumentCommand &fence) const {
        if (auto admitted = Admit(fence); admitted.HasError())
            return Result<PCGDocumentSave>::Failure(admitted.ErrorValue());
        if (!fence.operations.empty())
            return Result<PCGDocumentSave>::Failure(MakeError(PCGDocumentError(Reason::Invalid)));
        if (state_->conflict)
            return Result<PCGDocumentSave>::Failure(MakeError(PCGDocumentError(Reason::Conflict)));
        auto next = PCG::AdvanceGraphRevision(state_->acceptedGeneration.revision);
        if (next.HasError())
            return Result<PCGDocumentSave>::Failure(next.ErrorValue());
        auto data = state_->source->Data();
        data.generation.revision = next.Value();
        auto validated = PCG::PCGGraphAsset::Create(std::move(data), Context(state_->catalog));
        if (validated.HasError())
            return Result<PCGDocumentSave>::Failure(validated.ErrorValue());
        auto encoded = PCG::SerializePCGGraphAsset(validated.Value());
        if (encoded.HasError())
            return Result<PCGDocumentSave>::Failure(encoded.ErrorValue());
        PCGDocumentSave save;
        save.snapshot_ = Snapshot().Value();
        save.publishedGeneration_ = validated.Value().Data().generation;
        save.replacementEpoch_ = state_->replacementEpoch;
        const auto bytes = std::as_bytes(std::span(encoded.Value()));
        save.bytes_.assign(bytes.begin(), bytes.end());
        return Result<PCGDocumentSave>::Success(std::move(save));
    }

    /** @copydoc PCGGraphDocument::Save */
    Result<Assets::AssetSourcePublication> PCGGraphDocument::Save(const PCGDocumentSave &save, const std::filesystem::path &root,
                                                                  const std::filesystem::path &path,
                                                                  const ProjectMutationCoordinator &mutations, DurableFileSystem &files,
                                                                  CancellationToken cancellation) {
        try {
            if (auto access = CheckAccess(); access.HasError())
                return Result<Assets::AssetSourcePublication>::Failure(access.ErrorValue());
            if (state_->publishing)
                return Result<Assets::AssetSourcePublication>::Failure(MakeError(PCGDocumentError(Reason::Busy)));
            if (state_->conflict)
                return Result<Assets::AssetSourcePublication>::Failure(MakeError(PCGDocumentError(Reason::Conflict)));
            if (save.snapshot_.session != state_->session || save.replacementEpoch_ != state_->replacementEpoch ||
                save.snapshot_.catalogRevision != state_->catalog.revision || save.snapshot_.acceptedSource != state_->acceptedSource ||
                path != root / state_->identity.key.source.Value() || save.snapshot_.revision > state_->revision)
                return Result<Assets::AssetSourcePublication>::Failure(MakeError(PCGDocumentError(Reason::Stale)));
            auto projectLease = mutations.TryAcquire({root, ProjectMutationOwner::Asset, "pcg-source-save"});
            if (projectLease.HasError())
                return Result<Assets::AssetSourcePublication>::Failure(projectLease.ErrorValue());
            const auto heldProjectLease = std::move(projectLease).Value();
            const auto lockPath = Assets::AssetSourcePublicationLockPath(root, path);
            if (lockPath.HasError())
                return Result<Assets::AssetSourcePublication>::Failure(lockPath.ErrorValue());
            auto lease = files.TryAcquireExclusive(lockPath.Value(), "pcg-source-save");
            if (lease.HasError())
                return Result<Assets::AssetSourcePublication>::Failure(lease.ErrorValue());

            PublicationGuard guard(state_->publishing);

            auto publication =
                Assets::PublishAssetSource(root, path, save.snapshot_.acceptedSource, save.Bytes(), lease.Value(), files, cancellation);
            if (publication.HasError())
                return publication;
            // Tracked native replacement can commit before its directory sync fails. Preserve dirty/base and force explicit reconciliation.
            if (publication.Value().status == Assets::AssetSourcePublicationStatus::ReconciliationRequired) {
                state_->conflict = true;
                return publication;
            }
            state_->acceptedSource = publication.Value().identity;
            state_->acceptedGeneration = save.publishedGeneration_;
            state_->savedState = save.snapshot_.state;
            state_->conflict = false;
            return publication;
        } catch (const std::bad_alloc &) {
            return Result<Assets::AssetSourcePublication>::Failure(MakeError(Assets::AssetSourceErrors::Limit));
        } catch (...) {  // NOSONAR(cpp:S1181, cpp:S2738)
            // ADR-008 contains arbitrary precommit lease-provider failures at this typed save boundary.
            return Result<Assets::AssetSourcePublication>::Failure(MakeError(Assets::AssetSourceErrors::ReadFailed));
        }
    }

    /** @copydoc PCGGraphDocument::ObserveExternal */
    Result<void> PCGGraphDocument::ObserveExternal(const Assets::AssetSourceIdentity &identity) {
        if (auto access = CheckAccess(); access.HasError())
            return access;
        state_->conflict = identity != state_->acceptedSource;
        return Result<void>::Success();
    }

    /** @copydoc PCGGraphDocument::Reload */
    Result<void> PCGGraphDocument::Reload(const PCGDocumentCommand &fence, const Assets::AssetSourceCapture &capture, bool discardDirty,
                                          const PCG::IPCGGraphSourceMigrator *migrator) {
        using enum PCGDocumentCommandFailureReason;
        if (auto admitted = Admit(fence); admitted.HasError())
            return admitted;
        if (state_->publishing)
            return Result<void>::Failure(MakeError(PCGDocumentError(Busy)));
        if (!fence.operations.empty())
            return Result<void>::Failure(MakeError(PCGDocumentError(Invalid)));
        if (state_->currentState != state_->savedState && !discardDirty)
            return Result<void>::Failure(MakeError(PCGDocumentError(Dirty)));
        if (!capture.identity.exists || capture.identity.bytes != capture.bytes.size() ||
            capture.identity.digest != ComputeSha256(capture.bytes))
            return Result<void>::Failure(MakeError(PCGDocumentError(Invalid)));
        auto parsed = PCG::DeserializePCGGraphAsset(SourceBytes(capture), Context(state_->catalog), migrator);
        if (parsed.HasError())
            return Result<void>::Failure(parsed.ErrorValue());
        if (parsed.Value().Data().generation.graph != state_->acceptedGeneration.graph)
            return Result<void>::Failure(MakeError(PCGDocumentError(Conflict)));
        auto digest = StateIdentity(parsed.Value(), state_->catalog);
        if (digest.HasError())
            return Result<void>::Failure(digest.ErrorValue());
        auto source = std::make_shared<const PCG::PCGGraphAsset>(std::move(parsed).Value());
        if (fence.cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(PCGDocumentError(Cancelled)));
        state_->source.swap(source);
        state_->acceptedSource = capture.identity;
        state_->acceptedGeneration = state_->source->Data().generation;
        state_->currentState = digest.Value();
        state_->savedState = PCG::InspectPCGGraphSchemaVersion(SourceBytes(capture)).Value() == PCG::CurrentPCGGraphSchemaVersion
                                 ? digest.Value()
                                 : Sha256Digest{};
        state_->history.Clear();
        state_->conflict = false;
        ++state_->replacementEpoch;
        ++state_->revision;
        return Result<void>::Success();
    }

    /** @copydoc PCGGraphDocument::AcceptRecovery */
    Result<void> PCGGraphDocument::AcceptRecovery(const PCGDocumentCommand &fence, const PCG::PCGGraphAsset &recovered,
                                                  const Assets::AssetSourceIdentity &expectedBase) {
        if (auto admitted = Admit(fence); admitted.HasError())
            return admitted;
        if (!fence.operations.empty())
            return Result<void>::Failure(MakeError(PCGDocumentError(Reason::Invalid)));
        if (state_->conflict || expectedBase != state_->acceptedSource ||
            recovered.Data().generation.graph != state_->acceptedGeneration.graph)
            return Result<void>::Failure(MakeError(PCGDocumentError(Reason::Conflict)));
        auto candidate = recovered.Data();
        candidate.generation = state_->source->Data().generation;
        return Commit(std::move(candidate), fence.cancellation);
    }

    /** @copydoc PCGGraphDocument::PrepareCatalog */
    Result<PCGGraphDocument::CatalogReplacement> PCGGraphDocument::PrepareCatalog(PCGDocumentCatalog catalog) const {
        if (auto access = CheckAccess(); access.HasError())
            return Result<CatalogReplacement>::Failure(access.ErrorValue());
        if (state_->publishing)
            return Result<CatalogReplacement>::Failure(MakeError(PCGDocumentError(Reason::Busy)));
        if (!ValidCatalog(catalog) || catalog.revision <= state_->catalog.revision)
            return Result<CatalogReplacement>::Failure(MakeError(PCGDocumentError(Reason::Stale)));
        auto validated = PCG::PCGGraphAsset::Create(state_->source->Data(), Context(catalog));
        if (validated.HasError())
            return Result<CatalogReplacement>::Failure(validated.ErrorValue());
        auto source = std::make_shared<const PCG::PCGGraphAsset>(std::move(validated).Value());
        return Result<CatalogReplacement>::Success({std::move(catalog), std::move(source)});
    }

    /** @copydoc PCGGraphDocument::CommitCatalog */
    void PCGGraphDocument::CommitCatalog(CatalogReplacement &replacement) noexcept {
        state_->source.swap(replacement.source);
        std::swap(state_->catalog, replacement.catalog);
    }

    /** @copydoc PCGGraphDocument::ReplaceCatalog */
    Result<void> PCGGraphDocument::ReplaceCatalog(PCGDocumentCatalog catalog) {
        auto prepared = PrepareCatalog(std::move(catalog));
        if (prepared.HasError())
            return Result<void>::Failure(prepared.ErrorValue());
        auto replacement = std::move(prepared).Value();
        CommitCatalog(replacement);
        return Result<void>::Success();
    }

    /** @copydoc PCGGraphDocument::Close */
    Result<void> PCGGraphDocument::Close(bool discardDirty) {
        if (state_ && state_->closed)
            return Result<void>::Success();
        if (auto access = CheckAccess(); access.HasError())
            return access;
        if (state_->publishing)
            return Result<void>::Failure(MakeError(PCGDocumentError(Reason::Busy)));
        if (state_->currentState != state_->savedState && !discardDirty)
            return Result<void>::Failure(MakeError(PCGDocumentError(Reason::Dirty)));
        state_->closed = true;
        state_->source.reset();
        state_->history.Clear();
        return Result<void>::Success();
    }

    /** @copydoc PCGGraphDocument::UndoCount */
    std::size_t PCGGraphDocument::UndoCount() const noexcept {
        return state_ ? state_->history.UndoCount() : 0;
    }

    /** @copydoc PCGGraphDocument::RedoCount */
    std::size_t PCGGraphDocument::RedoCount() const noexcept {
        return state_ ? state_->history.RedoCount() : 0;
    }
}  // namespace Horo::Editor
