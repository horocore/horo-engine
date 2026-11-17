#include "Horo/Gameplay/BehaviorRestore.h"

#include <type_traits>
static_assert(std::is_copy_constructible_v<Horo::Gameplay::BehaviorPersistenceTarget>);
static_assert(std::is_same_v<decltype(Horo::Gameplay::BehaviorEntityRestoreTarget{}.entity), Horo::Runtime::SaveEntityReference>);

class NativeRestorePreparation final : public Horo::Gameplay::IBehaviorRestorePreparation {
public:
    Horo::Gameplay::BehaviorRestoreServices RequiredRestoreServices() const noexcept override {
        return Horo::Gameplay::BehaviorRestoreServices::None;
    }

    Horo::Result<void> PrepareInactive(const Horo::Gameplay::BehaviorRestoreInstance &) override {
        return Horo::Result<void>::Success();
    }
};

int main() {
    Horo::Gameplay::BehaviorPersistenceTarget target;
    NativeRestorePreparation native;
    return target.CanUse() || native.RequiredRestoreServices() != Horo::Gameplay::BehaviorRestoreServices::None ? 1 : 0;
}
