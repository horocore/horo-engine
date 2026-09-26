#include "Horo/AI/AIErrors.h"
#include "Horo/AI/EnvironmentQuerySchema.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace Horo::AI {
    namespace {
        /** @brief Checks the inert origin metadata shared by all EQS descriptor families. */
        [[nodiscard]] bool ValidOrigin(const QueryDescriptorOrigin &origin) noexcept {
            return origin.kind < QueryDescriptorSourceKind::Count && origin.provider.IsValid() && origin.version != 0;
        }

        /** @brief Detects repeated stable identities within one bounded descriptor domain. */
        template <typename Descriptor> [[nodiscard]] bool HasDuplicateIds(const std::vector<Descriptor> &descriptors) {
            for (std::size_t outer = 0; outer < descriptors.size(); ++outer)
                for (std::size_t inner = outer + 1; inner < descriptors.size(); ++inner)
                    if (descriptors[outer].id == descriptors[inner].id)
                        return true;
            return false;
        }

        /** @brief Validates stable, typed property definitions without invoking a provider. */
        [[nodiscard]] Result<void> ValidateProperties(const std::vector<QueryPropertyDescriptor> &properties) {
            if (properties.size() > EnvironmentQuerySchemaLimits::DescriptorProperties)
                return Result<void>::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
            for (std::size_t index = 0; index < properties.size(); ++index) {
                const auto &property = properties[index];
                if (!property.id.IsValid() || property.kind >= QueryPropertyKind::Count || property.maximumBytes == 0 ||
                    property.maximumBytes > EnvironmentQuerySchemaLimits::CanonicalBytes)
                    return Result<void>::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
                for (std::size_t other = index + 1; other < properties.size(); ++other)
                    if (property.id == properties[other].id)
                        return Result<void>::Failure(MakeError(AIErrors::EnvironmentQueryIdentityConflict));
            }
            return Result<void>::Success();
        }

        /** @brief Validates bounded, unique typed context requirements for one stage descriptor. */
        [[nodiscard]] Result<void> ValidateContexts(const std::vector<QueryContextRequirement> &contexts) {
            if (contexts.size() > EnvironmentQuerySchemaLimits::DescriptorContexts)
                return Result<void>::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
            for (std::size_t index = 0; index < contexts.size(); ++index) {
                if (!contexts[index].id.IsValid() || !contexts[index].version.IsValid())
                    return Result<void>::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
                for (std::size_t other = index + 1; other < contexts.size(); ++other)
                    if (contexts[index].id == contexts[other].id)
                        return Result<void>::Failure(MakeError(AIErrors::EnvironmentQueryIdentityConflict));
            }
            return Result<void>::Success();
        }

        /** @brief Checks that every declared context has a compatible captured descriptor. */
        [[nodiscard]] Result<void> ResolveContexts(const QuerySchemaRegistry &registry,
                                                   const std::vector<QueryContextRequirement> &contexts) {
            for (const auto &requirement : contexts) {
                const auto *descriptor = registry.Find(requirement.id);
                if (descriptor == nullptr)
                    return Result<void>::Failure(MakeError(AIErrors::EnvironmentQueryDescriptorUnavailable));
                if (!requirement.version.Contains(descriptor->origin.version))
                    return Result<void>::Failure(MakeError(AIErrors::EnvironmentQueryVersionIncompatible));
            }
            return Result<void>::Success();
        }

        /** @brief Checks one generator's item and context dependencies after the registry is captured. */
        [[nodiscard]] Result<void> ResolveGenerator(const QuerySchemaRegistry &registry, const QueryGeneratorDescriptor &generator) {
            const auto *item = registry.Find(generator.outputItemType);
            if (item == nullptr)
                return Result<void>::Failure(MakeError(AIErrors::EnvironmentQueryDescriptorUnavailable));
            if (!generator.outputItemVersion.Contains(item->origin.version))
                return Result<void>::Failure(MakeError(AIErrors::EnvironmentQueryVersionIncompatible));
            return ResolveContexts(registry, generator.contexts);
        }

        /** @brief Checks one test's item and context dependencies after the registry is captured. */
        [[nodiscard]] Result<void> ResolveTest(const QuerySchemaRegistry &registry, const QueryTestDescriptor &test) {
            const auto *item = registry.Find(test.inputItemType);
            if (item == nullptr)
                return Result<void>::Failure(MakeError(AIErrors::EnvironmentQueryDescriptorUnavailable));
            if (!test.inputItemVersion.Contains(item->origin.version))
                return Result<void>::Failure(MakeError(AIErrors::EnvironmentQueryVersionIncompatible));
            return ResolveContexts(registry, test.contexts);
        }

        /** @brief Validates bounded item payload contracts before any dependency resolution. */
        [[nodiscard]] Result<void> ValidateItems(const std::vector<QueryItemTypeDescriptor> &items) {
            for (const auto &item : items)
                if (!item.id.IsValid() || !ValidOrigin(item.origin) || item.kind >= QueryItemKind::Count ||
                    (item.kind == QueryItemKind::Custom
                         ? item.maximumPayloadBytes == 0 || item.maximumPayloadBytes > EnvironmentQuerySchemaLimits::CanonicalBytes
                         : item.maximumPayloadBytes != 0))
                    return Result<void>::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
            return Result<void>::Success();
        }

        /** @brief Validates bounded context payload contracts before any dependency resolution. */
        [[nodiscard]] Result<void> ValidateContextDescriptors(const std::vector<QueryContextDescriptor> &contexts) {
            for (const auto &context : contexts)
                if (!context.id.IsValid() || !ValidOrigin(context.origin) || context.maximumPayloadBytes == 0 ||
                    context.maximumPayloadBytes > EnvironmentQuerySchemaLimits::CanonicalBytes)
                    return Result<void>::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
            return Result<void>::Success();
        }

        /** @brief Validates generator identity and its bounded typed property/context declarations. */
        [[nodiscard]] Result<void> ValidateGeneratorDescriptors(const std::vector<QueryGeneratorDescriptor> &generators) {
            for (const auto &generator : generators) {
                if (!generator.id.IsValid() || !ValidOrigin(generator.origin) || !generator.outputItemType.IsValid() ||
                    !generator.outputItemVersion.IsValid())
                    return Result<void>::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
                if (const auto properties = ValidateProperties(generator.properties); properties.HasError())
                    return properties;
                if (const auto contexts = ValidateContexts(generator.contexts); contexts.HasError())
                    return contexts;
            }
            return Result<void>::Success();
        }

        /** @brief Validates test identity and its bounded typed property/context declarations. */
        [[nodiscard]] Result<void> ValidateTestDescriptors(const std::vector<QueryTestDescriptor> &tests) {
            for (const auto &test : tests) {
                if (!test.id.IsValid() || !ValidOrigin(test.origin) || !test.inputItemType.IsValid() || !test.inputItemVersion.IsValid())
                    return Result<void>::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
                if (const auto properties = ValidateProperties(test.properties); properties.HasError())
                    return properties;
                if (const auto contexts = ValidateContexts(test.contexts); contexts.HasError())
                    return contexts;
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc QuerySchemaRegistry::Capture */
    Result<QuerySchemaRegistry> QuerySchemaRegistry::Capture(const QueryDescriptorContributions &contributions) {
        if (contributions.items.size() > EnvironmentQuerySchemaLimits::ItemTypes ||
            contributions.contexts.size() > EnvironmentQuerySchemaLimits::Contexts ||
            contributions.generators.size() > EnvironmentQuerySchemaLimits::Generators ||
            contributions.tests.size() > EnvironmentQuerySchemaLimits::Tests)
            return Result<QuerySchemaRegistry>::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));

        QuerySchemaRegistry registry;
        registry.items_.assign(contributions.items.begin(), contributions.items.end());
        registry.contexts_.assign(contributions.contexts.begin(), contributions.contexts.end());
        registry.generators_.assign(contributions.generators.begin(), contributions.generators.end());
        registry.tests_.assign(contributions.tests.begin(), contributions.tests.end());

        if (const auto items = ValidateItems(registry.items_); items.HasError())
            return Result<QuerySchemaRegistry>::Failure(items.ErrorValue());
        if (const auto contexts = ValidateContextDescriptors(registry.contexts_); contexts.HasError())
            return Result<QuerySchemaRegistry>::Failure(contexts.ErrorValue());
        if (const auto generators = ValidateGeneratorDescriptors(registry.generators_); generators.HasError())
            return Result<QuerySchemaRegistry>::Failure(generators.ErrorValue());
        if (const auto tests = ValidateTestDescriptors(registry.tests_); tests.HasError())
            return Result<QuerySchemaRegistry>::Failure(tests.ErrorValue());
        if (HasDuplicateIds(registry.items_) || HasDuplicateIds(registry.contexts_) || HasDuplicateIds(registry.generators_) ||
            HasDuplicateIds(registry.tests_))
            return Result<QuerySchemaRegistry>::Failure(MakeError(AIErrors::EnvironmentQueryIdentityConflict));

        for (const auto &generator : registry.generators_)
            if (const auto resolved = ResolveGenerator(registry, generator); resolved.HasError())
                return Result<QuerySchemaRegistry>::Failure(resolved.ErrorValue());
        for (const auto &test : registry.tests_)
            if (const auto resolved = ResolveTest(registry, test); resolved.HasError())
                return Result<QuerySchemaRegistry>::Failure(resolved.ErrorValue());

        return Result<QuerySchemaRegistry>::Success(std::move(registry));
    }

    /** @copydoc QuerySchemaRegistry::Find */
    const QueryItemTypeDescriptor *QuerySchemaRegistry::Find(const QueryItemTypeId id) const noexcept {
        const auto found = std::ranges::find_if(items_, [id](const auto &entry) {
            return entry.id == id;
        });
        return found == items_.end() ? nullptr : std::to_address(found);
    }

    /** @copydoc QuerySchemaRegistry::Find */
    const QueryContextDescriptor *QuerySchemaRegistry::Find(const QueryContextId id) const noexcept {
        const auto found = std::ranges::find_if(contexts_, [id](const auto &entry) {
            return entry.id == id;
        });
        return found == contexts_.end() ? nullptr : std::to_address(found);
    }

    /** @copydoc QuerySchemaRegistry::Find */
    const QueryGeneratorDescriptor *QuerySchemaRegistry::Find(const QueryGeneratorId id) const noexcept {
        const auto found = std::ranges::find_if(generators_, [id](const auto &entry) {
            return entry.id == id;
        });
        return found == generators_.end() ? nullptr : std::to_address(found);
    }

    /** @copydoc QuerySchemaRegistry::Find */
    const QueryTestDescriptor *QuerySchemaRegistry::Find(const QueryTestId id) const noexcept {
        const auto found = std::ranges::find_if(tests_, [id](const auto &entry) {
            return entry.id == id;
        });
        return found == tests_.end() ? nullptr : std::to_address(found);
    }
}  // namespace Horo::AI
