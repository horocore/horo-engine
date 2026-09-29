#include "Horo/AI/EnvironmentQueryContexts.h"

#include "Horo/AI/AIErrors.h"

#include <algorithm>
#include <utility>

namespace Horo::AI {
    namespace {
        constexpr std::uint64_t BuiltinBase = 0x484f524f00010000ULL;
        constexpr std::size_t MaximumContextGroup = 64;

        /** @brief Validates an exact scene/entity generation without retaining its borrowed view. */
        [[nodiscard]] Result<void> ValidateEntity(const Runtime::RuntimeSceneView &scene, const Runtime::EntityRef entity) {
            if (!entity.IsValid() || entity.runtime != scene.RuntimeId() || scene.Get(entity).HasError())
                return Result<void>::Failure(MakeError(AIErrors::EnvironmentQueryContextStale));
            return Result<void>::Success();
        }

        /** @brief Validates all entity-bearing provider values before owned publication. */
        [[nodiscard]] Result<void> ValidateValue(const Runtime::RuntimeSceneView &scene, const QueryContextValue &value,
                                                 const std::size_t maximumBytes) {
            if (const auto *entity = std::get_if<Runtime::EntityRef>(&value))
                return ValidateEntity(scene, *entity);
            if (const auto *group = std::get_if<std::vector<Runtime::EntityRef>>(&value)) {
                if (group->size() > MaximumContextGroup)
                    return Result<void>::Failure(MakeError(AIErrors::EnvironmentQueryContextInvalid));
                for (const auto member : *group)
                    if (const auto valid = ValidateEntity(scene, member); valid.HasError())
                        return valid;
            }
            if (const auto *bytes = std::get_if<QueryCanonicalContext>(&value);
                bytes != nullptr && (bytes->schemaVersion == 0 || bytes->size > bytes->bytes.size() || bytes->size > maximumBytes))
                return Result<void>::Failure(MakeError(AIErrors::EnvironmentQueryContextInvalid));
            return Result<void>::Success();
        }

        /** @brief Creates one built-in owned value after required-presence checks. */
        [[nodiscard]] Result<QueryContextValue> CaptureBuiltin(const BuiltinQueryContext kind, const QueryContextCaptureSource &source) {
            switch (kind) {
                case BuiltinQueryContext::Querier:
                    return Result<QueryContextValue>::Success(source.querier);
                case BuiltinQueryContext::Target:
                    if (source.target)
                        return Result<QueryContextValue>::Success(*source.target);
                    break;
                case BuiltinQueryContext::QuerierLocation:
                    return Result<QueryContextValue>::Success(source.querierLocation);
                case BuiltinQueryContext::TargetLocation:
                    if (source.target && source.targetLocation)
                        return Result<QueryContextValue>::Success(*source.targetLocation);
                    break;
                case BuiltinQueryContext::WorldOrigin:
                    return Result<QueryContextValue>::Success(Math::WorldCoordinate64{});
                case BuiltinQueryContext::Group:
                    if (source.group && source.group->size() <= MaximumContextGroup)
                        return Result<QueryContextValue>::Success(
                            std::vector<Runtime::EntityRef>{source.group->begin(), source.group->end()});
                    if (source.group && source.group->size() > MaximumContextGroup)
                        return Result<QueryContextValue>::Failure(MakeError(AIErrors::EnvironmentQueryContextInvalid));
                    break;
                case BuiltinQueryContext::Count:
                    break;
            }
            return Result<QueryContextValue>::Failure(MakeError(AIErrors::EnvironmentQueryContextMissing));
        }

        /** @brief Returns reserved kind only for an exact built-in identity. */
        [[nodiscard]] std::optional<BuiltinQueryContext> BuiltinKind(const QueryContextId id) {
            for (std::uint8_t index = 0; index < static_cast<std::uint8_t>(BuiltinQueryContext::Count); ++index) {
                const auto kind = static_cast<BuiltinQueryContext>(index);
                if (id == BuiltinQueryContextId(kind))
                    return kind;
            }
            return std::nullopt;
        }
    }  // namespace

    /** @copydoc BuiltinQueryContextId */
    QueryContextId BuiltinQueryContextId(const BuiltinQueryContext kind) {
        if (kind >= BuiltinQueryContext::Count)
            return {};
        return QueryContextId::Create(BuiltinBase + static_cast<std::uint64_t>(kind)).Value();
    }

    /** @copydoc BuiltinQueryContextDescriptors */
    std::array<QueryContextDescriptor, 6> BuiltinQueryContextDescriptors() {
        std::array<QueryContextDescriptor, 6> descriptors{};
        const QueryDescriptorOrigin origin{.kind = QueryDescriptorSourceKind::Native,
                                           .provider = QueryProviderId::Create(BuiltinBase).Value(),
                                           .version = 1};
        for (std::size_t index = 0; index < descriptors.size(); ++index)
            descriptors[index] = {.id = BuiltinQueryContextId(static_cast<BuiltinQueryContext>(index)),
                                  .origin = origin,
                                  .maximumPayloadBytes = EnvironmentQuerySchemaLimits::CanonicalBytes};
        return descriptors;
    }

    /** @copydoc QueryContextSnapshot::Scene */
    Runtime::SceneRuntimeId QueryContextSnapshot::Scene() const noexcept {
        return scene_;
    }

    /** @copydoc QueryContextSnapshot::Revision */
    std::uint64_t QueryContextSnapshot::Revision() const noexcept {
        return revision_;
    }

    /** @copydoc QueryContextSnapshot::Values */
    std::span<const QueryCapturedContext> QueryContextSnapshot::Values() const noexcept {
        return values_;
    }

    /** @copydoc QueryContextSnapshot::Find */
    const QueryCapturedContext *QueryContextSnapshot::Find(const QueryContextId id) const noexcept {
        const auto found = std::ranges::find_if(values_, [id](const auto &entry) {
            return entry.id == id;
        });
        return found == values_.end() ? nullptr : &*found;
    }

    /** @copydoc QueryContextProviderRegistry::Capture */
    Result<QueryContextProviderRegistry> QueryContextProviderRegistry::Capture(
        const std::span<const QueryContextProviderRegistration> providers, const QuerySchemaRegistry &schema) {
        if (providers.size() > EnvironmentQuerySchemaLimits::Contexts)
            return Result<QueryContextProviderRegistry>::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
        QueryContextProviderRegistry registry;
        registry.providers_.reserve(providers.size());
        for (const auto &provider : providers) {
            const auto *descriptor = schema.Find(provider.id);
            if (!provider.id.IsValid() || BuiltinKind(provider.id) || provider.capture == nullptr ||
                !provider.requiredCapabilities.IsValid())
                return Result<QueryContextProviderRegistry>::Failure(MakeError(AIErrors::EnvironmentQueryContextInvalid));
            if (descriptor == nullptr)
                return Result<QueryContextProviderRegistry>::Failure(MakeError(AIErrors::EnvironmentQueryDescriptorUnavailable));
            if (provider.schemaVersion == 0 || provider.schemaVersion != descriptor->origin.version)
                return Result<QueryContextProviderRegistry>::Failure(MakeError(AIErrors::EnvironmentQueryVersionIncompatible));
            if (registry.Find(provider.id) != nullptr)
                return Result<QueryContextProviderRegistry>::Failure(MakeError(AIErrors::EnvironmentQueryIdentityConflict));
            registry.providers_.push_back(provider);
        }
        return Result<QueryContextProviderRegistry>::Success(std::move(registry));
    }

    /** @copydoc QueryContextProviderRegistry::Find */
    const QueryContextProviderRegistration *QueryContextProviderRegistry::Find(const QueryContextId id) const noexcept {
        const auto found = std::ranges::find_if(providers_, [id](const auto &entry) {
            return entry.id == id;
        });
        return found == providers_.end() ? nullptr : &*found;
    }

    /** @copydoc QueryContextCapture::Capture */
    Result<std::shared_ptr<const QueryContextSnapshot>> QueryContextCapture::Capture(const EnvironmentQueryPlan &plan,
                                                                                     const QuerySchemaRegistry &schema,
                                                                                     const QueryContextProviderRegistry &providers,
                                                                                     const Runtime::RuntimeSceneView &scene,
                                                                                     const QueryContextCaptureSource &source) {
        using SnapshotResult = Result<std::shared_ptr<const QueryContextSnapshot>>;
        if (!scene.IsCurrent() || !scene.RuntimeId().IsValid())
            return SnapshotResult::Failure(MakeError(AIErrors::EnvironmentQueryContextStale));
        if (!plan.Id().IsValid() || !source.availableCapabilities.IsValid() || source.executionRevision == 0)
            return SnapshotResult::Failure(MakeError(AIErrors::EnvironmentQueryContextInvalid));
        if (plan_.IsValid() && (plan_ != plan.Id() || source.executionRevision < latestRevision_ ||
                                (latest_ != nullptr && latest_->Scene() != scene.RuntimeId())))
            return SnapshotResult::Failure(MakeError(AIErrors::EnvironmentQueryContextStale));
        if (latest_ != nullptr && source.executionRevision == latestRevision_)
            return SnapshotResult::Success(latest_);
        if (const auto valid = ValidateEntity(scene, source.querier); valid.HasError())
            return SnapshotResult::Failure(valid.ErrorValue());

        std::shared_ptr<QueryContextSnapshot> captured{new QueryContextSnapshot};
        captured->scene_ = scene.RuntimeId();
        captured->revision_ = source.executionRevision;
        captured->values_.reserve(plan.RequiredContexts().size());
        for (const auto &requirement : plan.RequiredContexts()) {
            const auto *descriptor = schema.Find(requirement.id);
            if (descriptor == nullptr)
                return SnapshotResult::Failure(MakeError(AIErrors::EnvironmentQueryContextMissing));
            if (!requirement.version.Contains(descriptor->origin.version))
                return SnapshotResult::Failure(MakeError(AIErrors::EnvironmentQueryVersionIncompatible));
            Result<QueryContextValue> value = Result<QueryContextValue>::Failure(MakeError(AIErrors::EnvironmentQueryContextMissing));
            if (const auto kind = BuiltinKind(requirement.id)) {
                if ((*kind == BuiltinQueryContext::Target || *kind == BuiltinQueryContext::TargetLocation) && source.target) {
                    if (const auto valid = ValidateEntity(scene, *source.target); valid.HasError())
                        return SnapshotResult::Failure(valid.ErrorValue());
                }
                value = CaptureBuiltin(*kind, source);
            } else if (const auto *provider = providers.Find(requirement.id)) {
                if ((provider->requiredCapabilities.bits & source.availableCapabilities.bits) != provider->requiredCapabilities.bits)
                    return SnapshotResult::Failure(MakeError(AIErrors::EnvironmentQueryContextCapabilityUnavailable));
                value = provider->capture(provider->state, scene);
            }
            if (value.HasError())
                return SnapshotResult::Failure(value.ErrorValue());
            if (const auto valid = ValidateValue(scene, value.Value(), descriptor->maximumPayloadBytes); valid.HasError())
                return SnapshotResult::Failure(valid.ErrorValue());
            captured->values_.push_back({.id = requirement.id, .value = std::move(value).Value()});
        }
        plan_ = plan.Id();
        latestRevision_ = source.executionRevision;
        latest_ = std::move(captured);
        return SnapshotResult::Success(latest_);
    }
}  // namespace Horo::AI
