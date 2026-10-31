#include "Horo/Editor/AnimationGraphDocument.h"

#include "AnimationGraphDocumentBounds.h"
#include "Horo/Editor/SemanticDocumentHistory.h"

#include <algorithm>
#include <limits>
#include <thread>
#include <type_traits>

namespace Horo::Editor {
    namespace AnimationDocumentErrors {
        const ErrorCodeDescriptor InvalidCommand{ErrorDomainId{"editor.animation"}, ErrorCode{"editor.animation.command.invalid"},
                                                 ErrorSeverity::Error, "Invalid animation document identity, command or policy.",
                                                 "Refresh the source document."};
        const ErrorCodeDescriptor StaleRevision{ErrorDomainId{"editor.animation"}, ErrorCode{"editor.animation.revision.stale"},
                                                ErrorSeverity::Error, "Animation document capture is stale or belongs to another session.",
                                                "Capture the current document."};
        const ErrorCodeDescriptor HistoryUnavailable{ErrorDomainId{"editor.animation"}, ErrorCode{"editor.animation.history.unavailable"},
                                                     ErrorSeverity::Error, "Animation history step is unavailable.",
                                                     "Inspect current history availability."};
        const ErrorCodeDescriptor HistoryLimit{ErrorDomainId{"editor.animation"}, ErrorCode{"editor.animation.history.limit"},
                                               ErrorSeverity::Error, "Animation transaction exceeds the history budget.",
                                               "Submit a smaller transaction."};
        const ErrorCodeDescriptor DirtyDocument{ErrorDomainId{"editor.animation"}, ErrorCode{"editor.animation.document.dirty"},
                                                ErrorSeverity::Error, "Animation document contains unsaved changes.",
                                                "Save, recover or explicitly discard changes."};
        const ErrorCodeDescriptor Closed{ErrorDomainId{"editor.animation"}, ErrorCode{"editor.animation.document.closed"},
                                         ErrorSeverity::Error, "Animation document is closed.", "Open a document session."};
        const ErrorCodeDescriptor Exhausted{ErrorDomainId{"editor.animation"}, ErrorCode{"editor.animation.identity.exhausted"},
                                            ErrorSeverity::Error, "Animation document revision is exhausted.",
                                            "Open a new document session."};
        const ErrorCodeDescriptor WrongThread{ErrorDomainId{"editor.animation"}, ErrorCode{"editor.animation.thread.wrong"},
                                              ErrorSeverity::Error, "Animation document requires its owner thread.",
                                              "Dispatch the operation on the document owner."};
    }  // namespace AnimationDocumentErrors

    namespace Detail {
        /** @brief Owned canonical source; captures keep this publication alive after owner retirement. */
        struct AnimationDocumentRoot final {
            Animation::AnimationGraphData data;
            std::string bytes;
        };

        /** @brief Private lifetime token distinguishes even accidentally duplicated host document identities. */
        struct AnimationDocumentOwner final {};
    }  // namespace Detail

    namespace {
        using namespace Animation;
        using Root = Detail::AnimationDocumentRoot;

        /** @brief Exact before/after values of one stable authored element. */
        template <typename T> struct Change final {
            std::optional<T> before;
            std::optional<T> after;
        };

        /** @brief Source-only stable-ID deltas; no runtime state or complete graph snapshots. */
        struct Delta final {
            std::vector<Change<GraphDefinition>> definitions;
            std::vector<Change<GraphParameter>> parameters;
            GraphDefinitionId beforeEntry;
            GraphDefinitionId afterEntry;
            std::uint64_t beforeState{};
            std::uint64_t afterState{};
            std::size_t bytes{sizeof(Delta)};
        };

        /** @brief Charges actual variable-size storage retained by a semantic parameter value. */
        std::size_t ExtraBytes(const GraphParameter &value) {
            return value.name.capacity() + 1;
        }

        /** @brief Charges every nested source allocation in a retained semantic definition. */
        std::size_t ExtraBytes(const GraphDefinition &value) {
            std::size_t bytes = value.inputs.capacity() * sizeof(GraphInterfaceInput) + value.nodes.capacity() * sizeof(GraphNode) +
                                value.connections.capacity() * sizeof(GraphConnection);
            for (const auto &node : value.nodes) {
                bytes += node.pins.capacity() * sizeof(GraphPin);
                if (const auto *blend = std::get_if<GraphBlendNode>(&node.payload))
                    bytes += blend->mask.joints.capacity() * sizeof(GraphBoneWeight);
                if (const auto *machine = std::get_if<GraphStateMachineNode>(&node.payload)) {
                    bytes +=
                        machine->states.capacity() * sizeof(GraphState) + machine->transitions.capacity() * sizeof(GraphStateTransition);
                    for (const auto &transition : machine->transitions)
                        bytes += transition.conditions.capacity() * sizeof(GraphStateCondition);
                }
            }
            return bytes;
        }

        /** @brief Builds exact semantic deltas from canonically sorted source collections. */
        template <typename T> std::vector<Change<T>> Changes(const std::vector<T> &before, const std::vector<T> &after) {
            std::vector<Change<T>> changes;
            for (const auto &value : before) {
                const auto found = std::ranges::find(after, value.id, &T::id);
                if (found == after.end())
                    changes.push_back({value, {}});
                else if (*found != value)
                    changes.push_back({value, *found});
            }
            for (const auto &value : after)
                if (std::ranges::find(before, value.id, &T::id) == before.end())
                    changes.push_back({{}, value});
            return changes;
        }

        /** @brief Charges actual retained vector capacities after detached delta construction. */
        template <typename T> std::size_t Bytes(const std::vector<Change<T>> &changes) {
            std::size_t bytes = changes.capacity() * sizeof(Change<T>);
            for (const auto &change : changes) {
                if (change.before)
                    bytes += ExtraBytes(*change.before);
                if (change.after)
                    bytes += ExtraBytes(*change.after);
            }
            return bytes;
        }

        /** @brief Replaces or inserts one semantic value; the canonical validator admits final topology. */
        template <typename T> void Put(std::vector<T> &values, const T &value) {
            const auto found = std::ranges::find(values, value.id, &T::id);
            if (found == values.end())
                values.push_back(value);
            else
                *found = value;
        }

        /** @brief Missing removal is a malformed command, not an implicit no-op. */
        template <typename T, typename Id> Result<void> Remove(std::vector<T> &values, const Id id) {
            if (std::erase_if(values, [id](const T &value) {
                return value.id == id;
            }) == 0)
                return Result<void>::Failure(MakeError(AnimationDocumentErrors::InvalidCommand));
            return Result<void>::Success();
        }

        /** @brief Replays only the selected side of each exact semantic delta into detached source. */
        template <typename T> void ReplayChanges(std::vector<T> &values, const std::vector<Change<T>> &changes, const bool redo) {
            for (const auto &change : changes) {
                const auto &selected = redo ? change.after : change.before;
                if (selected)
                    Put(values, *selected);
                else
                    static_cast<void>(Remove(values, (change.before ? change.before : change.after)->id));
            }
        }

        /** @brief Stages one closed typed command without exposing intermediate invalid topology. */
        Result<void> Apply(AnimationGraphData &data, const AnimationGraphDocumentCommand &command) {
            return std::visit([&data]<typename T>(const T &value) -> Result<void> {
                if constexpr (std::is_same_v<T, SetAnimationGraphEntry>)
                    data.entry = value.entry;
                else if constexpr (std::is_same_v<T, PutAnimationGraphParameter>)
                    Put(data.parameters, value.value);
                else if constexpr (std::is_same_v<T, PutAnimationGraphDefinition>)
                    Put(data.definitions, value.value);
                else if constexpr (std::is_same_v<T, RemoveAnimationGraphParameter>)
                    return Remove(data.parameters, value.id);
                else
                    return Remove(data.definitions, value.id);
                return Result<void>::Success();
            }, command);
        }

        /** @brief Uses the sole existing authoring validator/codec without discovering runtime dependencies. */
        Result<std::shared_ptr<const Root>> Prepare(const AnimationGraphData &source, const AnimationGraphDocumentLimits &limits,
                                                    const CancellationToken &cancellation) {
            const AnimationGraphCompileContext context{.limits = limits.graph, .cancellation = cancellation};
            auto validated = MigrateAnimationGraph(source, context);
            if (validated.HasError())
                return Result<std::shared_ptr<const Root>>::Failure(validated.ErrorValue());
            auto bytes = SerializeAnimationGraphSource(validated.Value(), context, limits.source);
            if (bytes.HasError())
                return Result<std::shared_ptr<const Root>>::Failure(bytes.ErrorValue());
            return Result<std::shared_ptr<const Root>>::Success(
                std::make_shared<const Root>(Root{std::move(validated).Value(), std::move(bytes).Value()}));
        }

        /** @brief Unknown enum values cannot authorize discard. */
        bool ValidPolicy(const AnimationDocumentDiscardPolicy policy) {
            return policy == AnimationDocumentDiscardPolicy::RequireClean || policy == AnimationDocumentDiscardPolicy::DiscardChanges;
        }
    }  // namespace

    /** @brief Owner-thread source and bounded semantic history; no borrowed dependency or caller policy is retained. */
    struct AnimationGraphDocument::Impl final {
        DocumentIdentity identity;
        AnimationGraphDocumentLimits limits;
        std::shared_ptr<const Root> root;
        std::shared_ptr<const Root> savedRoot;
        std::shared_ptr<const Detail::AnimationDocumentOwner> owner{std::make_shared<const Detail::AnimationDocumentOwner>()};
        SemanticDocumentHistory<Delta> history;
        const std::thread::id thread{std::this_thread::get_id()};
        std::uint64_t revision{1}, currentState{1}, nextState{2}, firstState{1};
        bool dirty{};

        explicit Impl(const AnimationGraphDocumentLimits &policy) : limits(policy), history({policy.historyEntries, policy.historyBytes}) {}
    };

    AnimationGraphDocumentSnapshot::AnimationGraphDocumentSnapshot(DocumentIdentity identity, const std::uint64_t revision,
                                                                   const std::uint64_t state, std::shared_ptr<const Root> root,
                                                                   std::shared_ptr<const Detail::AnimationDocumentOwner> owner)
        : identity_(std::move(identity)), revision_(revision), state_(state), root_(std::move(root)), owner_(std::move(owner)) {}

    /** @copydoc AnimationGraphDocumentSnapshot::Identity */
    const DocumentIdentity &AnimationGraphDocumentSnapshot::Identity() const noexcept {
        return identity_;
    }

    /** @copydoc AnimationGraphDocumentSnapshot::Revision */
    std::uint64_t AnimationGraphDocumentSnapshot::Revision() const noexcept {
        return revision_;
    }

    /** @copydoc AnimationGraphDocumentSnapshot::State */
    std::uint64_t AnimationGraphDocumentSnapshot::State() const noexcept {
        return state_;
    }

    /** @copydoc AnimationGraphDocumentSnapshot::Data */
    const AnimationGraphData &AnimationGraphDocumentSnapshot::Data() const noexcept {
        return root_->data;
    }

    /** @copydoc AnimationGraphDocumentSnapshot::SourceBytes */
    std::string_view AnimationGraphDocumentSnapshot::SourceBytes() const noexcept {
        return root_->bytes;
    }

    AnimationGraphDocument::AnimationGraphDocument(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    AnimationGraphDocument::~AnimationGraphDocument() = default;

    /** @copydoc AnimationGraphDocument::Open */
    Result<std::unique_ptr<AnimationGraphDocument>> AnimationGraphDocument::Open(DocumentIdentity identity,
                                                                                 const AnimationGraphData &source,
                                                                                 const AnimationGraphDocumentLimits &limits,
                                                                                 const CancellationToken cancellation) {
        if (!identity.IsValid() || identity.key.kind != DocumentKind::Asset || limits.historyEntries == 0 || limits.historyEntries > 64 ||
            limits.historyBytes == 0 || limits.historyBytes > 8 * 1024 * 1024)
            return Result<std::unique_ptr<AnimationGraphDocument>>::Failure(MakeError(AnimationDocumentErrors::InvalidCommand));
        auto root = Prepare(source, limits, cancellation);
        if (root.HasError())
            return Result<std::unique_ptr<AnimationGraphDocument>>::Failure(root.ErrorValue());
        auto impl = std::make_unique<Impl>(limits);
        impl->identity = std::move(identity);
        impl->root = std::move(root).Value();
        if (source.schemaVersion != CurrentAnimationGraphSchemaVersion || source.contractVersion != CurrentAnimationGraphContractVersion)
            impl->dirty = true;
        else
            impl->savedRoot = impl->root;
        return Result<std::unique_ptr<AnimationGraphDocument>>::Success(std::unique_ptr<AnimationGraphDocument>{
            new AnimationGraphDocument(std::move(impl))});  // NOSONAR: make_unique cannot access the private constructor.
    }

    /** @copydoc AnimationGraphDocument::Check */
    Result<void> AnimationGraphDocument::Check(const std::uint64_t expectedRevision, const CancellationToken cancellation) const {
        if (impl_->thread != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(AnimationDocumentErrors::WrongThread));
        if (!impl_->root)
            return Result<void>::Failure(MakeError(AnimationDocumentErrors::Closed));
        if (expectedRevision != impl_->revision)
            return Result<void>::Failure(MakeError(AnimationDocumentErrors::StaleRevision));
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(AnimationErrors::GraphOperationCancelled));
        if (impl_->revision == std::numeric_limits<std::uint64_t>::max() || impl_->nextState == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(AnimationDocumentErrors::Exhausted));
        return Result<void>::Success();
    }

    /** @copydoc AnimationGraphDocument::Snapshot */
    Result<AnimationGraphDocumentSnapshot> AnimationGraphDocument::Snapshot() const {
        if (impl_->thread != std::this_thread::get_id())
            return Result<AnimationGraphDocumentSnapshot>::Failure(MakeError(AnimationDocumentErrors::WrongThread));
        if (!impl_->root)
            return Result<AnimationGraphDocumentSnapshot>::Failure(MakeError(AnimationDocumentErrors::Closed));
        return Result<AnimationGraphDocumentSnapshot>::Success(
            {impl_->identity, impl_->revision, impl_->currentState, impl_->root, impl_->owner});
    }

    /** @copydoc AnimationGraphDocument::IsDirty */
    bool AnimationGraphDocument::IsDirty() const noexcept {
        return impl_->root && impl_->dirty;
    }

    /** @copydoc AnimationGraphDocument::CanUndo */
    bool AnimationGraphDocument::CanUndo() const noexcept {
        return impl_->root && impl_->history.UndoCount() != 0;
    }

    /** @copydoc AnimationGraphDocument::CanRedo */
    bool AnimationGraphDocument::CanRedo() const noexcept {
        return impl_->root && impl_->history.RedoCount() != 0;
    }

    /** @copydoc AnimationGraphDocument::Commit */
    Result<void> AnimationGraphDocument::Commit(const AnimationGraphData &candidate, const CancellationToken cancellation) {
        if (candidate.id != impl_->root->data.id || candidate.skeleton != impl_->root->data.skeleton)
            return Result<void>::Failure(MakeError(AnimationDocumentErrors::InvalidCommand));
        auto root = Prepare(candidate, impl_->limits, cancellation);
        if (root.HasError())
            return Result<void>::Failure(root.ErrorValue());
        if (root.Value()->bytes == impl_->root->bytes)
            return Result<void>::Success();
        const auto &before = impl_->root->data;
        const auto &after = root.Value()->data;
        Delta delta{Changes(before.definitions, after.definitions),
                    Changes(before.parameters, after.parameters),
                    before.entry,
                    after.entry,
                    impl_->currentState,
                    impl_->nextState};
        delta.bytes += Bytes(delta.definitions) + Bytes(delta.parameters);
        auto history = impl_->history.Prepare(std::move(delta));
        if (!history)
            return Result<void>::Failure(MakeError(AnimationDocumentErrors::HistoryLimit));
        if (auto checked = Check(impl_->revision, cancellation); checked.HasError())
            return checked;
        impl_->history.Commit(*history);
        impl_->dirty = !impl_->savedRoot || root.Value()->bytes != impl_->savedRoot->bytes;
        impl_->root = std::move(root).Value();
        impl_->currentState = impl_->nextState++;
        ++impl_->revision;
        return Result<void>::Success();
    }

    /** @copydoc AnimationGraphDocument::Execute */
    Result<void> AnimationGraphDocument::Execute(const std::uint64_t expectedRevision,
                                                 const std::span<const AnimationGraphDocumentCommand> commands,
                                                 const CancellationToken cancellation) {
        if (auto checked = Check(expectedRevision, cancellation); checked.HasError())
            return checked;
        if (commands.empty() || commands.size() > 64)
            return Result<void>::Failure(MakeError(AnimationDocumentErrors::InvalidCommand));
        auto candidate = impl_->root->data;
        for (const auto &command : commands) {
            if (auto bounded = AnimationDocumentInternal::CheckCommandBounds(candidate, command, impl_->limits); bounded.HasError())
                return bounded;
            if (auto applied = Apply(candidate, command); applied.HasError())
                return applied;
        }
        return Commit(candidate, cancellation);
    }

    /** @copydoc AnimationGraphDocument::Replay */
    Result<void> AnimationGraphDocument::Replay(const std::uint64_t expectedRevision, const bool redo,
                                                const CancellationToken cancellation) {
        if (auto checked = Check(expectedRevision, cancellation); checked.HasError())
            return checked;
        const auto *entry = impl_->history.Next(redo);
        if (!entry)
            return Result<void>::Failure(MakeError(AnimationDocumentErrors::HistoryUnavailable));
        auto candidate = impl_->root->data;
        ReplayChanges(candidate.definitions, entry->definitions, redo);
        ReplayChanges(candidate.parameters, entry->parameters, redo);
        candidate.entry = redo ? entry->afterEntry : entry->beforeEntry;
        auto root = Prepare(candidate, impl_->limits, cancellation);
        if (root.HasError())
            return Result<void>::Failure(root.ErrorValue());
        if (auto checked = Check(expectedRevision, cancellation); checked.HasError())
            return checked;
        impl_->dirty = !impl_->savedRoot || root.Value()->bytes != impl_->savedRoot->bytes;
        impl_->root = std::move(root).Value();
        impl_->currentState = redo ? entry->afterState : entry->beforeState;
        impl_->history.DidReplay(redo);
        ++impl_->revision;
        return Result<void>::Success();
    }

    /** @copydoc AnimationGraphDocument::Undo */
    Result<void> AnimationGraphDocument::Undo(const std::uint64_t revision, const CancellationToken cancellation) {
        return Replay(revision, false, cancellation);
    }

    /** @copydoc AnimationGraphDocument::Redo */
    Result<void> AnimationGraphDocument::Redo(const std::uint64_t revision, const CancellationToken cancellation) {
        return Replay(revision, true, cancellation);
    }

    /** @copydoc AnimationGraphDocument::MarkSaved */
    Result<void> AnimationGraphDocument::MarkSaved(const AnimationGraphDocumentSnapshot &snapshot) {
        if (impl_->thread != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(AnimationDocumentErrors::WrongThread));
        if (!impl_->root)
            return Result<void>::Failure(MakeError(AnimationDocumentErrors::Closed));
        if (snapshot.owner_ != impl_->owner || snapshot.identity_ != impl_->identity || snapshot.revision_ > impl_->revision ||
            snapshot.state_ < impl_->firstState || snapshot.state_ >= impl_->nextState)
            return Result<void>::Failure(MakeError(AnimationDocumentErrors::StaleRevision));
        impl_->dirty = impl_->root->bytes != snapshot.root_->bytes;
        impl_->savedRoot = snapshot.root_;
        return Result<void>::Success();
    }

    /** @copydoc AnimationGraphDocument::Reload */
    Result<void> AnimationGraphDocument::Reload(const std::uint64_t expectedRevision, const AnimationGraphData &source,
                                                const AnimationDocumentDiscardPolicy policy, const CancellationToken cancellation) {
        if (auto checked = Check(expectedRevision, cancellation); checked.HasError())
            return checked;
        if (!ValidPolicy(policy) || source.id != impl_->root->data.id || source.skeleton != impl_->root->data.skeleton)
            return Result<void>::Failure(MakeError(AnimationDocumentErrors::InvalidCommand));
        if (IsDirty() && policy == AnimationDocumentDiscardPolicy::RequireClean)
            return Result<void>::Failure(MakeError(AnimationDocumentErrors::DirtyDocument));
        auto root = Prepare(source, impl_->limits, cancellation);
        if (root.HasError())
            return Result<void>::Failure(root.ErrorValue());
        if (auto checked = Check(expectedRevision, cancellation); checked.HasError())
            return checked;
        impl_->root = std::move(root).Value();
        impl_->history.Clear();
        impl_->currentState = impl_->nextState++;
        impl_->firstState = impl_->currentState;
        impl_->dirty =
            source.schemaVersion != CurrentAnimationGraphSchemaVersion || source.contractVersion != CurrentAnimationGraphContractVersion;
        impl_->savedRoot = impl_->dirty ? nullptr : impl_->root;
        ++impl_->revision;
        return Result<void>::Success();
    }

    /** @copydoc AnimationGraphDocument::ApplyRecovery */
    Result<void> AnimationGraphDocument::ApplyRecovery(const std::uint64_t expectedRevision, const std::string_view bytes,
                                                       const CancellationToken cancellation) {
        if (auto checked = Check(expectedRevision, cancellation); checked.HasError())
            return checked;
        const AnimationGraphCompileContext context{.limits = impl_->limits.graph, .cancellation = cancellation};
        auto parsed = DeserializeAnimationGraphSource(bytes, impl_->root->data.id, context, impl_->limits.source);
        if (parsed.HasError())
            return Result<void>::Failure(parsed.ErrorValue());
        return Commit(parsed.Value(), cancellation);
    }

    /** @copydoc AnimationGraphDocument::Close */
    Result<void> AnimationGraphDocument::Close(const AnimationDocumentDiscardPolicy policy) {
        if (impl_->thread != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(AnimationDocumentErrors::WrongThread));
        if (!ValidPolicy(policy))
            return Result<void>::Failure(MakeError(AnimationDocumentErrors::InvalidCommand));
        if (IsDirty() && policy == AnimationDocumentDiscardPolicy::RequireClean)
            return Result<void>::Failure(MakeError(AnimationDocumentErrors::DirtyDocument));
        impl_->history.Clear();
        impl_->root.reset();
        impl_->savedRoot.reset();
        impl_->dirty = false;
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
