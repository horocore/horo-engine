#include "JsonUtils.h"
#include "SceneSourceInternal.h"

#include <algorithm>
#include <array>

namespace Horo::SceneSource::Detail {
    namespace {
        /** @brief Decodes one fully explicit authored source policy with no implicit identities, area or filter. */
        Result<Runtime::NavigationSourceComponent> ParseSource(const Json &value) {
            if (constexpr std::array fields{"collider", "surface", "area", "filter", "schemaVersion", "generation"};
                !value.is_object() || value.size() != fields.size() + 1 ||
                !std::ranges::all_of(fields,
                                     [&value](const char *field) {
                return value.contains(field) && value[field].is_number_unsigned();
            }) ||
                !value.contains("enabled") || !value["enabled"].is_boolean() || value["schemaVersion"].get<std::uint64_t>() != 1)
                return Result<Runtime::NavigationSourceComponent>::Failure(
                    PersistenceError(SceneInvalid, "Navigation source schema is incomplete or invalid."));
            auto surface = Navigation::SurfaceId::Create(value["surface"].get<std::uint64_t>());
            auto area = Navigation::NavigationAreaId::Create(value["area"].get<std::uint64_t>());
            auto filter = Navigation::NavigationFilterId::Create(value["filter"].get<std::uint64_t>());
            if (surface.HasError() || area.HasError() || filter.HasError())
                return Result<Runtime::NavigationSourceComponent>::Failure(
                    PersistenceError(SceneInvalid, "Navigation source identities are invalid."));
            Runtime::NavigationSourceComponent source{.collider = {value["collider"].get<std::uint64_t>()},
                                                      .surface = surface.Value(),
                                                      .area = area.Value(),
                                                      .filter = filter.Value(),
                                                      .generation = value["generation"].get<std::uint64_t>(),
                                                      .enabled = value["enabled"].get<bool>()};
            if (Runtime::ValidateNavigationSourceComponent(source).HasError())
                return Result<Runtime::NavigationSourceComponent>::Failure(
                    PersistenceError(SceneInvalid, "Navigation source policy is invalid."));
            return Result<Runtime::NavigationSourceComponent>::Success(source);
        }
    }  // namespace

    /** @copydoc ParseNavigationSources */
    Result<std::vector<Runtime::NavigationSourceComponent>> ParseNavigationSources(const Json &value) {
        using Return = Result<std::vector<Runtime::NavigationSourceComponent>>;
        if (!value.is_array() || value.size() > Runtime::MaximumNavigationSourcesPerObject)
            return Return::Failure(PersistenceError(SceneInvalid, "Navigation sources must be a bounded array."));
        std::vector<Runtime::NavigationSourceComponent> sources;
        sources.reserve(value.size());
        for (const auto &entry : value) {
            auto source = ParseSource(entry);
            if (source.HasError())
                return Return::Failure(source.ErrorValue());
            if (std::ranges::any_of(sources, [&source](const auto &existing) {
                return existing.collider == source.Value().collider && existing.surface == source.Value().surface;
            }))
                return Return::Failure(PersistenceError(SceneInvalid, "Navigation source bindings must be unique on their object."));
            sources.push_back(source.Value());
        }
        return Return::Success(std::move(sources));
    }
}  // namespace Horo::SceneSource::Detail
