#include "Horo/Gameplay/NativePerceptionSense.h"

#include <charconv>

namespace Horo::Gameplay {
    /** @copydoc ParseNativePerceptionSenseIdentity */
    AI::SenseTypeId ParseNativePerceptionSenseIdentity(const std::string_view identity) {
        if (identity.empty() || identity.front() == '0')
            return {};
        std::uint64_t value{};
        const auto parsed = std::from_chars(identity.data(), identity.data() + identity.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != identity.data() + identity.size() || value <= AI::SenseTypeIds::Team.Value())
            return {};
        auto result = AI::SenseTypeId::Create(value);
        return result.HasValue() ? result.Value() : AI::SenseTypeId{};
    }
}  // namespace Horo::Gameplay
