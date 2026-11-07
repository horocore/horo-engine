#pragma once
/** @file PrefabCookBuiltinProjection.h
 * @brief Cold host split between the actual native Scene codec and the separately captured opaque Gameplay owner.
 */
#include "Horo/Scene/PrefabComponentProjection.h"

namespace Horo::Application::PrefabCookDetail {
    struct CookBuiltinProjection final {
        Runtime::RuntimeComponentSet components;
        std::optional<Runtime::PrimitiveMeshDescriptor> primitive;

        [[nodiscard]] Result<void> Collect(const Prefab::RawComponentPayload &source, std::vector<Gameplay::SerializedComponent> &custom) {
            auto native = SceneSource::ProjectPrefabBuiltinComponent(source, components, primitive);
            if (native.HasError())
                return Result<void>::Failure(std::move(native).ErrorValue());
            if (!native.Value())
                custom.push_back(source.component);
            return Result<void>::Success();
        }
    };
}  // namespace Horo::Application::PrefabCookDetail
