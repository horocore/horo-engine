#pragma once

/** @file ScenePrefabExpansionOwner.h
 * @brief SceneSource-owned bounded worker preparation, memoization and stale-completion retirement.
 */
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Scene/SceneRuntimeConversion.h"

namespace Horo::SceneSource {
    /** @brief Exact owner/session inputs copied into one immutable worker attempt; no live document is borrowed. */
    struct ScenePrefabExpansionRequest final {
        std::uint64_t documentSession{}; /**< Nonzero host session incarnation, never reused after close/replacement. */
        Runtime::SceneDefinitionId scene;
        Runtime::SceneDefinitionRevision revision;
        SceneSourceDocument document;
        Prefab::PrefabSourceResolverSnapshot resolver;
        Prefab::PrefabLimitProfile limits;
    };

    /**
     * @brief Owns one pending expansion job and a bounded cache; only its owner may publish a completion.
     * @details Workers capture an owned envelope, never this object or a mutable document/service reference.
     * Ordinary completion is nonblocking. Close/replacement/shutdown close admission and cancel/join with
     * an explicit host wait policy; timeout preserves the draining attempt for retry. Jobs must outlive this owner.
     * Destruction is a teardown safety drain, not an ordinary frame operation; hosts call Shutdown first.
     */
    class ScenePrefabExpansionOwner final {
    public:
        /** @brief Captures the injected scheduler and cache policy without submitting work.
         * @param jobs Process-owned scheduler, alive through shutdown. @param limits Finite cache/storage ceilings. */
        explicit ScenePrefabExpansionOwner(JobSystem &jobs, Prefab::PrefabExpansionCacheLimits limits = {});
        /** @brief Cancels and drains any remaining accepted job before releasing immutable inputs. */
        ~ScenePrefabExpansionOwner();
        ScenePrefabExpansionOwner(const ScenePrefabExpansionOwner &) = delete;
        ScenePrefabExpansionOwner &operator=(const ScenePrefabExpansionOwner &) = delete;
        ScenePrefabExpansionOwner(ScenePrefabExpansionOwner &&) = delete;
        ScenePrefabExpansionOwner &operator=(ScenePrefabExpansionOwner &&) = delete;
        /**
         * @brief Admits one bounded complete attempt and reuses exact immutable cache hits.
         * @param request Coherent source/document/settings snapshot, borrowed during admission and copied into owned work.
         * @param cancellation Parent operation cancellation.
         * @return Accepted job identity or typed invalid/capacity/thread/in-progress error without replacing pending work.
         * @details Queue admission never blocks, including inside a host NonCritical producer scope;
         * an injected Block policy at capacity returns the scheduler's typed wait-forbidden error.
         * Before capture, Scene input is limited to 65,536 objects, 256 placements, 64 MiB logical
         * codec/copy input and 16 MiB encoded Scene bytes. All resolver sources together must fit
         * 256 documents and 32 MiB canonical bytes, including unreachable sources retained by the snapshot.
         */
        [[nodiscard]] Result<JobId> Submit(const ScenePrefabExpansionRequest &request, CancellationToken cancellation = {});
        /**
         * @brief Returns a terminal definition only after checking all current session/document/source/settings inputs.
         * @param current Coherent owner-thread current evidence, borrowed only during this call.
         * @return Empty while running; complete runtime definition, or typed terminal/stale error. No partial output escapes.
         * @details Validation allocation failure is typed and retires that attempt. After validation,
         * cache insertion is best-effort: capacity/allocation failure does not discard the definition.
         * Successful earlier insertions may remain; each individual failing insertion preserves existing entries.
         * Parent cancellation is checked again after memoization before output publication.
         */
        [[nodiscard]] Result<std::optional<Runtime::RuntimeSceneDefinition>> TakeCompleted(const ScenePrefabExpansionRequest &current);
        /** @brief Explicit bounded test/tool/teardown wait, never implicit in completion polling.
         * @param options Host-approved finite policy. @return Job terminal result or retryable wait-control error. */
        [[nodiscard]] Result<void> Join(const JoinOptions &options) const;
        /** @brief Closes document admission, cancels/joins the exact attempt and retires its cache.
         * @param options Host-approved finite teardown policy. @return Success, or wait interruption retaining owned work. */
        [[nodiscard]] Result<void> CloseDocument(const JoinOptions &options);
        /** @brief Drains the previous document/scene before opening new admission; no old result transfers.
         * @param options Host-approved finite replacement policy. @return Success or wait interruption, still closed. */
        [[nodiscard]] Result<void> ReplaceScene(const JoinOptions &options);
        /** @brief Idempotently closes admission and drains owned work before scheduler/service destruction.
         * @param options Host-approved finite teardown policy. @return Success or wait interruption preserving ownership. */
        [[nodiscard]] Result<void> Shutdown(const JoinOptions &options);

    private:
        struct Work;
        [[nodiscard]] bool IsOwner() const noexcept;
        /** @brief Checks lifecycle/thread/identity admission before owned input capture. */
        [[nodiscard]] Result<void> AdmitRequest(const ScenePrefabExpansionRequest &request, const CancellationToken &cancellation) const;
        /** @brief Captures bounded detached work and exact immutable cache hits without submitting it. */
        [[nodiscard]] Result<std::shared_ptr<Work>> CaptureWork(const ScenePrefabExpansionRequest &request,
                                                                const CancellationToken &cancellation) const;
        /** @brief Best-effort retention after all publication fences pass; pressure never discards valid output. */
        [[nodiscard]] Result<void> Memoize(Work &completed);
        /** @brief Retains one candidate or accepts optional capacity/allocation pressure without discarding output. */
        [[nodiscard]] Result<void> MemoizeCandidate(Work &completed, std::size_t index);
        [[nodiscard]] Result<void> Drain(const JoinOptions &options);
        JobSystem &jobs_;
        Prefab::PrefabExpansionCache cache_;
        const std::thread::id owner_;
        std::shared_ptr<Work> work_;
        JobHandle handle_;
        bool closed_{};
        bool shutdown_{};
    };
}  // namespace Horo::SceneSource
