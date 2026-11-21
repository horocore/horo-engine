#pragma once

/** @file NativePerceptionSense.h
 * @brief Native custom-sense annotations emitted by the gameplay descriptor generator.
 */

#include "Horo/AI/PerceptionSenseProvider.h"

#include <string_view>
#include <type_traits>
#include <utility>

namespace Horo::Gameplay {
    /** @brief Parses a canonical native custom-sense identity outside the reserved built-in range.
     * @param identity Quoted non-zero decimal identity. @return Typed identity, or invalid for malformed/reserved input.
     */
    [[nodiscard]] AI::SenseTypeId ParseNativePerceptionSenseIdentity(std::string_view identity);

    /** @brief Captures native metadata and its paired module-owned allocation callbacks without constructing a provider.
     * @tparam Sense Default-constructible native implementation with DescribePerceptionSense metadata.
     * @param identity Canonical annotation identity. @param source Annotation location owned by the generated descriptor.
     * @return Inert registration, validated by the host before module activation.
     */
    template <typename Sense>
    [[nodiscard]] AI::PerceptionSenseRegistration MakeNativePerceptionSenseRegistration(std::string_view identity, SourceLocation source) {
        static_assert(std::is_base_of_v<AI::IPerceptionSenseProvider, Sense>);
        static_assert(std::is_default_constructible_v<Sense>);
        AI::PerceptionSenseRegistration registration;
        registration.contract = Sense::DescribePerceptionSense();
        registration.contract.sense.identity = ParseNativePerceptionSenseIdentity(identity);
        if (registration.contract.source.source.empty())
            registration.contract.source = std::move(source);
        if (registration.contract.instanceBytes == 0)
            registration.contract.instanceBytes = sizeof(Sense);
        else if (registration.contract.instanceBytes < sizeof(Sense))
            registration.contract.instanceBytes = 0;
        registration.factory.create = [](void *) -> AI::IPerceptionSenseProvider * {
            return new Sense{};
        };
        registration.factory.destroy = [](void *, AI::IPerceptionSenseProvider *provider) noexcept {
            delete static_cast<Sense *>(provider);
        };
        return registration;
    }
}  // namespace Horo::Gameplay

/** @brief Marks a module-owned custom spatial sense; StableSenseId is a canonical quoted decimal identity greater than five. */
#define HORO_AI_SENSE(Type, StableSenseId)                                                                                                 \
    Horo::AI::PerceptionSenseRegistration HoroGeneratedPerceptionSense_##Type() noexcept {                                                 \
        try {                                                                                                                              \
            return Horo::Gameplay::MakeNativePerceptionSenseRegistration<Type>(StableSenseId, {__FILE__, __LINE__, 1});                    \
        } catch (...) {                                                                                                                    \
            return {};                                                                                                                     \
        }                                                                                                                                  \
    }
