#include "Horo/AI/EnvironmentQueryCache.h"

#include "Horo/AI/AIErrors.h"

#include <algorithm>
#include <new>
#include <ranges>
#include <tuple>
#include <type_traits>
#include <utility>

namespace Horo::AI {
    namespace {
        constexpr std::size_t MaximumEntries = 256;
        constexpr std::size_t MaximumBytes = 128 * 1024 * 1024;
        constexpr std::size_t MaximumCanonicalBytes = 16 * 1024;
        constexpr std::size_t MaximumDependencies = 256;

        /** @brief Identifies a dependency independently of its revision. */
        auto DependencyIdentity(const QueryCacheDependency &dependency) noexcept {
            return std::tie(dependency.provider, dependency.incarnation, dependency.dependency, dependency.scope);
        }

        /** @brief Admits a complete, bounded footprint without aliasing duplicate versions of the same read. */
        bool ValidKey(const QueryCacheKey &key) {
            if (!key.completeFootprint || !key.query.IsValid() || key.artifact == Sha256Digest{} || key.canonicalInputs.empty() ||
                key.canonicalInputs.size() > MaximumCanonicalBytes || key.dependencies.size() > MaximumDependencies)
                return false;
            return std::ranges::all_of(key.dependencies, [](const auto &dependency) {
                return dependency.provider.IsValid() && dependency.incarnation != 0 && dependency.dependency != 0 &&
                       dependency.revision != 0;
            }) && std::ranges::adjacent_find(key.dependencies, [](const auto &left, const auto &right) {
                return DependencyIdentity(left) >= DependencyIdentity(right);
            }) == key.dependencies.end();
        }

        /** @brief A cached entry cannot supply recycled entity handles or project opaque data into shared points. */
        bool CurrentContext(const Runtime::RuntimeSceneView &scene, const QueryContextValue &context, const bool shared) {
            if (const auto *entity = std::get_if<Runtime::EntityRef>(&context))
                return !shared && scene.Get(*entity).HasValue();
            if (const auto *group = std::get_if<std::vector<Runtime::EntityRef>>(&context))
                return !shared && std::ranges::all_of(*group, [&scene](const auto entity) {
                    return scene.Get(entity).HasValue();
                });
            return !shared || std::holds_alternative<Math::WorldCoordinate64>(context);
        }

        /** @brief Checks the fresh submission independently of retained cache bytes. */
        bool CurrentRequest(const Runtime::RuntimeSceneView &scene, const Runtime::SceneRuntimeId expectedScene,
                            const QueryExecutionInputs &inputs, const IQueryExecutionSource &source, const QueryCacheKey &key) {
            return scene.IsCurrent() && scene.RuntimeId() == expectedScene && inputs.agent.IsValid() && inputs.contexts &&
                   inputs.contexts->Scene() == expectedScene && inputs.contexts->Query() == key.query && ValidKey(key) &&
                   source.IsCurrent(inputs) && std::ranges::all_of(inputs.contexts->Values(), [&](const auto &context) {
                return CurrentContext(scene, context.value, key.sharedPointData);
            });
        }

        /** @brief Revalidates the entire winner set without changing its rank or coverage. */
        bool CurrentWinners(const Runtime::RuntimeSceneView &scene, const QueryExecutionResult &result, const bool shared) {
            return std::ranges::all_of(result.winners, [&](const auto &winner) {
                const auto *entity = std::get_if<Runtime::EntityRef>(&winner.item);
                return !entity || (!shared && scene.Get(*entity).HasValue());
            });
        }

        /** @brief Compares normalized captured values, including the live prefix of any provider payload. */
        bool SameContext(const QueryCapturedContext &left, const QueryCapturedContext &right) {
            if (left.id != right.id)
                return false;
            return std::visit([&](const auto &value) {
                using Value = std::decay_t<decltype(value)>;
                const auto *other = std::get_if<Value>(&right.value);
                if (!other)
                    return false;
                if constexpr (std::is_same_v<Value, QueryCanonicalContext>) {
                    return value.schemaVersion == other->schemaVersion && value.size == other->size &&
                           std::ranges::equal(std::span{value.bytes}.first(value.size), std::span{other->bytes}.first(other->size));
                } else {
                    return value == *other;
                }
            }, left.value);
        }

        /** @brief Charges context records and the nested entity-group copies retained by the cache. */
        std::size_t ContextBytes(std::span<const QueryCapturedContext> contexts) noexcept {
            auto bytes = contexts.size_bytes();
            for (const auto &context : contexts)
                if (const auto *group = std::get_if<std::vector<Runtime::EntityRef>>(&context.value))
                    bytes += group->size() * sizeof(Runtime::EntityRef);
            return bytes;
        }
    }  // namespace

    /** @copydoc QueryResultCache::Create */
    Result<QueryResultCache> QueryResultCache::Create(const Runtime::SceneRuntimeId scene, const QueryResultCacheLimits limits) {
        using Output = Result<QueryResultCache>;
        if (!scene.IsValid() || limits.maximumEntries == 0 || limits.maximumEntries > MaximumEntries || limits.maximumBytes == 0 ||
            limits.maximumBytes > MaximumBytes || limits.lifetimeTicks == 0 || limits.maximumEntries > limits.maximumBytes / sizeof(Entry))
            return Output::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
        try {
            return Output::Success(QueryResultCache{scene, limits});
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
        }
    }

    /** @copydoc QueryResultCache::QueryResultCache */
    QueryResultCache::QueryResultCache(const Runtime::SceneRuntimeId scene, const QueryResultCacheLimits limits)
        : scene_(scene), limits_(limits) {
        entries_.reserve(limits.maximumEntries);
        bytes_ = entries_.capacity() * sizeof(Entry);
    }

    QueryResultCache::QueryResultCache(QueryResultCache &&other) noexcept
        : scene_(std::exchange(other.scene_, {})), limits_(other.limits_), entries_(std::move(other.entries_)), bytes_(other.bytes_) {}

    /** @copydoc QueryResultCache::Expired */
    bool QueryResultCache::Expired(const Entry &entry, const std::uint64_t tick) const noexcept {
        const auto publication = entry.result->publicationTick;
        return tick < publication || tick - publication >= limits_.lifetimeTicks;
    }

    /** @copydoc QueryResultCache::Matches */
    bool QueryResultCache::Matches(const Entry &entry, const QueryCacheKey &key, const QueryExecutionInputs &inputs) {
        const auto owner = key.sharedPointData ? AgentHandle{} : inputs.agent;
        return entry.query == key.query && entry.artifact == key.artifact && entry.privateOwner == owner &&
               std::ranges::equal(entry.canonicalInputs, key.canonicalInputs) && std::ranges::equal(entry.dependencies, key.dependencies) &&
               std::ranges::equal(entry.contexts, inputs.contexts->Values(), SameContext);
    }

    /** @copydoc QueryResultCache::Store */
    Result<bool> QueryResultCache::Store(const Runtime::RuntimeSceneView &scene, const QueryExecutionInputs &inputs,
                                         const IQueryExecutionSource &source, const QueryCacheKey &key, const QueryExecutionResult &result,
                                         const std::uint64_t tick) {
        using Output = Result<bool>;
        if (result.status != QueryExecutionStatus::Completed || result.mode != QueryExecutionMode::AdaptiveRealtime || result.error ||
            result.publicationTick != tick || result.scene != scene_ || result.agent != inputs.agent ||
            !CurrentRequest(scene, scene_, inputs, source, key) || result.contextRevision != inputs.contexts->Revision() ||
            result.winners.size() > EnvironmentQuerySchemaLimits::MaximumResults || !CurrentWinners(scene, result, key.sharedPointData))
            return Output::Success(false);
        const auto contexts = inputs.contexts->Values();
        const auto charge = sizeof(QueryExecutionResult) + ContextBytes(contexts) + key.canonicalInputs.size_bytes() +
                            key.dependencies.size_bytes() + result.winners.size() * sizeof(QueryExecutionWinner);
        const auto storage = entries_.capacity() * sizeof(Entry);
        if (storage > limits_.maximumBytes || charge > limits_.maximumBytes - storage)
            return Output::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
        try {
            auto retained = std::make_shared<QueryExecutionResult>(result);
            if (key.sharedPointData)
                retained->agent = {};
            Entry candidate{.query = key.query,
                            .artifact = key.artifact,
                            .privateOwner = key.sharedPointData ? AgentHandle{} : inputs.agent,
                            .contexts = std::vector(contexts.begin(), contexts.end()),
                            .canonicalInputs = std::vector(key.canonicalInputs.begin(), key.canonicalInputs.end()),
                            .dependencies = std::vector(key.dependencies.begin(), key.dependencies.end()),
                            .result = std::move(retained),
                            .charge = charge};
            const auto replaced = std::ranges::find_if(entries_, [&](const auto &entry) {
                return Matches(entry, key, inputs);
            });
            if (replaced != entries_.end()) {
                bytes_ -= replaced->charge;
                entries_.erase(replaced);
            }
            // Entries stay in insertion order so eviction is stable, even across same-tick insertions.
            while (entries_.size() == limits_.maximumEntries || charge > limits_.maximumBytes - bytes_) {
                bytes_ -= entries_.front().charge;
                entries_.erase(entries_.begin());
            }
            entries_.push_back(std::move(candidate));
            bytes_ += charge;
            return Output::Success(true);
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
        }
    }

    /** @copydoc QueryResultCache::Lookup */
    std::shared_ptr<const QueryExecutionResult> QueryResultCache::Lookup(const Runtime::RuntimeSceneView &scene,
                                                                         const QueryExecutionInputs &inputs,
                                                                         const IQueryExecutionSource &source, const QueryCacheKey &key,
                                                                         const std::uint64_t tick) const {
        if (!CurrentRequest(scene, scene_, inputs, source, key))
            return {};
        for (const auto &entry : entries_ | std::views::reverse) {
            if (!Expired(entry, tick) && Matches(entry, key, inputs))
                return CurrentWinners(scene, *entry.result, key.sharedPointData) ? entry.result : nullptr;
        }
        return {};
    }

    /** @copydoc QueryResultCache::Clear */
    void QueryResultCache::Clear() noexcept {
        entries_.clear();
        bytes_ = entries_.capacity() * sizeof(Entry);
    }
}  // namespace Horo::AI
