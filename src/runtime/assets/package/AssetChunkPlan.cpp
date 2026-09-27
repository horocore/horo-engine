#include "Horo/Assets/AssetChunkPlan.h"

#include <algorithm>
#include <cctype>
#include <deque>
#include <numeric>
#include <ranges>
#include <utility>

namespace Horo::Assets {
    namespace {
        const ErrorDomainId Domain{"horo.asset"};
        const ErrorCodeDescriptor InvalidId{Domain, ErrorCode{"asset.chunk.invalid_id"}, ErrorSeverity::Error, "Chunk identity is invalid.",
                                            "Use a lowercase portable chunk ID."};
        const ErrorCodeDescriptor InvalidPlan{Domain, ErrorCode{"asset.chunk.invalid_plan"}, ErrorSeverity::Error,
                                              "Release chunk plan is invalid.", "Check chunk roles and membership."};
        const ErrorCodeDescriptor ResourceLimit{Domain, ErrorCode{"asset.chunk.resource_limit"}, ErrorSeverity::Error,
                                                "Release chunk plan exceeds its finite bounds.", "Reduce the chunk graph."};
        const ErrorCodeDescriptor DependencyInvalid{Domain, ErrorCode{"asset.chunk.dependency_invalid"}, ErrorSeverity::Error,
                                                    "Release chunk dependency graph is invalid.", "Remove missing or cyclic edges."};

        [[nodiscard]] bool IsChunkId(const std::string_view text) noexcept {
            if (text.empty() || text.size() > 64U || text.front() < 'a' || text.front() > 'z')
                return false;
            return std::ranges::all_of(text, [](const unsigned char ch) {
                return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
            });
        }

        [[nodiscard]] bool IsKnownKind(const AssetChunkKind kind) noexcept {
            using enum AssetChunkKind;
            switch (kind) {
                case Base:
                case Optional:
                case Language:
                case Dlc:
                case DedicatedServer:
                    return true;
            }
            return false;
        }

        [[nodiscard]] bool IsZeroDigest(const Sha256Digest &digest) noexcept {
            return std::ranges::all_of(digest.bytes, [](const std::uint8_t byte) {
                return byte == 0U;
            });
        }

        [[nodiscard]] bool HasDependencyCycle(std::span<const AssetChunkDefinition> chunks) {
            struct PendingChunk final {
                const AssetChunkDefinition *definition;
                std::size_t dependencies;
            };

            std::vector<PendingChunk> pending;
            pending.reserve(chunks.size());
            for (const auto &chunk : chunks)
                pending.emplace_back(&chunk, chunk.dependencies.size());

            std::deque<PendingChunk *> ready;
            for (auto &chunk : pending)
                if (chunk.dependencies == 0U)
                    ready.push_back(&chunk);

            std::size_t visited{};
            while (!ready.empty()) {
                const auto &finished = ready.front()->definition->id;
                ready.pop_front();
                ++visited;
                for (auto &chunk : pending) {
                    if (!std::ranges::binary_search(chunk.definition->dependencies, finished))
                        continue;
                    --chunk.dependencies;
                    if (chunk.dependencies == 0U)
                        ready.push_back(&chunk);
                }
            }
            return visited != chunks.size();
        }

        [[nodiscard]] bool HasMissingDependencies(std::span<const AssetChunkDefinition> chunks) {
            for (const auto &chunk : chunks)
                for (const auto &dependency : chunk.dependencies)
                    if (!std::ranges::binary_search(chunks, dependency, {}, &AssetChunkDefinition::id))
                        return true;
            return false;
        }
    }  // namespace

    /** @copydoc AssetChunkId::AssetChunkId */
    AssetChunkId::AssetChunkId(std::string value) : value_(std::move(value)) {}

    /** @copydoc AssetChunkId::Parse */
    Result<AssetChunkId> AssetChunkId::Parse(const std::string_view text) {
        if (!IsChunkId(text))
            return Result<AssetChunkId>::Failure(MakeError(InvalidId));
        return Result<AssetChunkId>::Success(AssetChunkId{std::string{text}});
    }

    /** @copydoc AssetChunkId::Value */
    const std::string &AssetChunkId::Value() const noexcept {
        return value_;
    }

    /** @copydoc AssetChunkPlan::AssetChunkPlan */
    AssetChunkPlan::AssetChunkPlan(std::vector<AssetChunkDefinition> chunks) : chunks_(std::move(chunks)) {}

    /** @copydoc AssetChunkPlan::Create */
    Result<AssetChunkPlan> AssetChunkPlan::Create(const std::span<const AssetChunkDefinition> definitions,
                                                  const AssetChunkPlanLimits &limits) {
        if (limits.maximumChunks == 0U || limits.maximumAssets == 0U || limits.maximumDependenciesPerChunk == 0U || definitions.empty())
            return Result<AssetChunkPlan>::Failure(MakeError(InvalidPlan));
        if (definitions.size() > limits.maximumChunks)
            return Result<AssetChunkPlan>::Failure(MakeError(ResourceLimit));

        std::vector<AssetChunkDefinition> chunks(definitions.begin(), definitions.end());
        std::ranges::sort(chunks, {}, &AssetChunkDefinition::id);
        std::vector<AssetId> allAssets;
        for (auto &chunk : chunks) {
            if (!IsChunkId(chunk.id.Value()) || !IsKnownKind(chunk.kind) ||
                (chunk.kind == AssetChunkKind::Dlc) != chunk.requiredBaseManifest.has_value() ||
                (chunk.requiredBaseManifest.has_value() && IsZeroDigest(*chunk.requiredBaseManifest)) || chunk.assets.empty())
                return Result<AssetChunkPlan>::Failure(MakeError(InvalidPlan));
            if (chunk.assets.size() > limits.maximumAssets - allAssets.size() ||
                chunk.dependencies.size() > limits.maximumDependenciesPerChunk)
                return Result<AssetChunkPlan>::Failure(MakeError(ResourceLimit));
            std::ranges::sort(chunk.assets);
            if (std::ranges::adjacent_find(chunk.assets) != chunk.assets.end() || !std::ranges::all_of(chunk.assets, &AssetId::IsValid))
                return Result<AssetChunkPlan>::Failure(MakeError(InvalidPlan));
            allAssets.insert(allAssets.end(), chunk.assets.begin(), chunk.assets.end());

            std::ranges::sort(chunk.dependencies);
            if (std::ranges::adjacent_find(chunk.dependencies) != chunk.dependencies.end() ||
                std::ranges::binary_search(chunk.dependencies, chunk.id))
                return Result<AssetChunkPlan>::Failure(MakeError(DependencyInvalid));
        }
        if (std::ranges::adjacent_find(chunks, {}, &AssetChunkDefinition::id) != chunks.end())
            return Result<AssetChunkPlan>::Failure(MakeError(InvalidPlan));
        std::ranges::sort(allAssets);
        if (std::ranges::adjacent_find(allAssets) != allAssets.end())
            return Result<AssetChunkPlan>::Failure(MakeError(InvalidPlan));
        if (HasMissingDependencies(chunks))
            return Result<AssetChunkPlan>::Failure(MakeError(DependencyInvalid));
        if (HasDependencyCycle(chunks))
            return Result<AssetChunkPlan>::Failure(MakeError(DependencyInvalid));
        return Result<AssetChunkPlan>::Success(AssetChunkPlan{std::move(chunks)});
    }

    /** @copydoc AssetChunkPlan::Chunks */
    std::span<const AssetChunkDefinition> AssetChunkPlan::Chunks() const noexcept {
        return chunks_;
    }
}  // namespace Horo::Assets
