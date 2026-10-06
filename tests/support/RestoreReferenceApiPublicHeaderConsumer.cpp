#include "Horo/Gameplay/PersistenceSource.h"
#include "Horo/Runtime/Save/SaveRestoreReferenceContext.h"

#include <type_traits>

namespace {
    class LegacyCandidate final : public Horo::Gameplay::IPreparedPersistenceState {
    public:
        void Publish() noexcept override {}
    };
}  // namespace

static_assert(!std::is_same_v<Horo::Runtime::PersistentEntityId, Horo::Runtime::SaveAssetId>);

int main() {
    auto context = Horo::Runtime::SaveRestoreReferenceContext::Create({1, 2, 3}, {});
    if (context.HasError())
        return 1;
    LegacyCandidate candidate;
    return candidate.FixupRuntimeReferences(context.Value().ForParticipant({})).HasValue() ? 0 : 2;
}
