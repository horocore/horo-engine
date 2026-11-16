#include "Horo/Gameplay/NativeDecisionNode.h"

#include <charconv>

namespace Horo::Gameplay {
    AI::DecisionNodeTypeId ParseNativeDecisionNodeIdentity(const std::string_view identity) {
        std::uint64_t value{};
        if (const auto parsed = std::from_chars(identity.data(), identity.data() + identity.size(), value);
            parsed.ec != std::errc{} || parsed.ptr != identity.data() + identity.size())
            return {};
        auto result = AI::DecisionNodeTypeId::Create(value);
        return result.HasValue() ? result.Value() : AI::DecisionNodeTypeId{};
    }
}  // namespace Horo::Gameplay
