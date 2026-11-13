#include "AnimationAssetCookState.h"

#include <algorithm>

namespace Horo::Animation {
    namespace {
        using Artifact = Assets::AssetCookCandidateArtifactView;

        bool SupportedType(std::string_view type) noexcept {
            return type == "core.animation.skeleton" || type == "core.animation.clip" || type == "core.animation.graph";
        }

        std::vector<const Artifact *> Ordered(std::span<const Artifact> artifacts) {
            std::vector<const Artifact *> ordered;
            ordered.reserve(artifacts.size());
            for (const auto &artifact : artifacts)
                ordered.push_back(&artifact);
            std::ranges::sort(ordered, {}, [](const auto *value) {
                return value->identity.id;
            });
            return ordered;
        }

        const Artifact *Lookup(const std::vector<const Artifact *> &ordered, Assets::AssetId id) noexcept {
            const auto found = std::ranges::lower_bound(ordered, id, {}, [](const auto *value) {
                return value->identity.id;
            });
            return found != ordered.end() && (*found)->identity.id == id ? *found : nullptr;
        }
    }  // namespace

    Result<void> PreparedAnimationCook::State::ValidateArtifact(const Entry &entry, const Artifact &artifact) const {
        if (artifact.identity.id != entry.id || artifact.identity.type != entry.type ||
            artifact.identity.artifactDigest != ComputeSha256(std::as_bytes(artifact.envelope)))
            return Result<void>::Failure(MakeError(AnimationAssetErrors::Stale));
        const Assets::AssetCookLimits bounds{.maximumArtifactBytes = static_cast<std::size_t>(limits.codec.bytes + 4096)};
        auto decoded = Assets::DecodeCookedArtifact(artifact.envelope, bounds);
        if (decoded.HasError())
            return Result<void>::Failure(std::move(decoded).ErrorValue());
        const auto &value = decoded.Value();
        if (value.id != entry.id || value.type != entry.type || value.target != target)
            return Result<void>::Failure(MakeError(AnimationAssetErrors::Stale));
        if (value.sourceDigest != capture.Find(entry.id)->sourceDigest || !std::ranges::equal(value.payload, entry.payload.bytes))
            return Result<void>::Failure(MakeError(AnimationAssetErrors::Stale));
        return Result<void>::Success();
    }

    Result<void> PreparedAnimationCook::State::ValidateEmitted(std::span<const Artifact> artifacts,
                                                               const CancellationToken &cancellation) const {
        if (artifacts.size() > 16384)
            return Result<void>::Failure(MakeError(AnimationAssetErrors::Capacity));
        const auto ordered = Ordered(artifacts);
        if (std::ranges::adjacent_find(ordered, {}, [](const auto *value) {
            return value->identity.id;
        }) != ordered.end())
            return Result<void>::Failure(MakeError(AnimationAssetErrors::Malformed));
        for (const auto *artifact : ordered)
            if (SupportedType(artifact->identity.type.Value()) && !Find(artifact->identity.id))
                return Result<void>::Failure(MakeError(AnimationAssetErrors::Stale));
        for (const auto &entry : entries) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(AnimationAssetErrors::Cancelled));
            const auto *artifact = Lookup(ordered, entry.id);
            if (!artifact)
                return Result<void>::Failure(MakeError(AnimationAssetErrors::Unavailable));
            if (auto valid = ValidateArtifact(entry, *artifact); valid.HasError())
                return valid;
        }
        return Result<void>::Success();
    }

    Result<void> PreparedAnimationCook::State::Merge(std::vector<Assets::AssetDependency> &closure, const Assets::AssetDependency &role,
                                                     std::size_t maximumAssets) const {
        if (const auto *record = capture.Registry().Find(role.id); !record || record->type != role.expectedType)
            return Result<void>::Failure(MakeError(AnimationAssetErrors::Stale));
        if (const auto found = std::ranges::find(closure, role.id,
                                                 [](const auto &value) {
            return value.id;
        });
            found != closure.end()) {
            if (found->expectedType != role.expectedType)
                return Result<void>::Failure(MakeError(AnimationAssetErrors::Malformed));
            return Result<void>::Success();
        }
        if (closure.size() >= maximumAssets)
            return Result<void>::Failure(MakeError(AnimationAssetErrors::Capacity));
        closure.push_back(role);
        return Result<void>::Success();
    }

    Result<void> PreparedAnimationCook::State::AppendRoles(std::vector<Assets::AssetDependency> &closure, const Entry &entry,
                                                           std::size_t maximumAssets, std::size_t &charged) const {
        if (entry.payload.dependencies.size() > limits.maximumReferences - charged)
            return Result<void>::Failure(MakeError(AnimationAssetErrors::Capacity));
        charged += entry.payload.dependencies.size();
        for (const auto &role : entry.payload.dependencies)
            if (auto merged = Merge(closure, role, maximumAssets); merged.HasError())
                return merged;
        return Result<void>::Success();
    }

    Result<std::vector<Assets::AssetDependency>> PreparedAnimationCook::State::BeginClosure(std::span<const Assets::AssetDependency> roots,
                                                                                            std::size_t maximumAssets) const {
        using Return = Result<std::vector<Assets::AssetDependency>>;
        std::vector<Assets::AssetDependency> closure;
        for (const auto &root : roots)
            if (auto merged = Merge(closure, root, maximumAssets); merged.HasError())
                return Return::Failure(std::move(merged).ErrorValue());
        return Return::Success(std::move(closure));
    }

    Result<std::vector<Assets::AssetDependency>> PreparedAnimationCook::State::Expand(std::span<const Assets::AssetDependency> roots,
                                                                                      std::span<const Artifact> artifacts,
                                                                                      std::size_t maximumAssets,
                                                                                      const CancellationToken &cancellation) const {
        using Return = Result<std::vector<Assets::AssetDependency>>;
        if (maximumAssets == 0 || maximumAssets > 16384 || roots.size() > limits.maximumReferences)
            return Return::Failure(MakeError(AnimationAssetErrors::Capacity));
        if (auto valid = ValidateEmitted(artifacts, cancellation); valid.HasError())
            return Return::Failure(std::move(valid).ErrorValue());
        auto initial = BeginClosure(roots, maximumAssets);
        if (initial.HasError())
            return Return::Failure(std::move(initial).ErrorValue());
        auto closure = std::move(initial).Value();
        std::size_t charged = roots.size();
        for (std::size_t index = 0; index < closure.size(); ++index) {
            if (cancellation.IsCancellationRequested())
                return Return::Failure(MakeError(AnimationAssetErrors::Cancelled));
            const auto *entry = Find(closure[index].id);
            if (!entry)
                continue;
            if (auto appended = AppendRoles(closure, *entry, maximumAssets, charged); appended.HasError())
                return Return::Failure(std::move(appended).ErrorValue());
        }
        std::ranges::sort(closure, {}, [](const auto &value) {
            return value.id;
        });
        return Return::Success(std::move(closure));
    }
}  // namespace Horo::Animation
