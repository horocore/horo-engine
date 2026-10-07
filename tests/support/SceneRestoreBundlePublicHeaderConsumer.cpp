#include "Horo/Runtime/Scene/SceneRestoreBundle.h"

#include <type_traits>

static_assert(std::is_base_of_v<Horo::Runtime::SceneAggregateRestore, Horo::Runtime::SceneRestoreBundle>);
static_assert(!std::is_copy_constructible_v<Horo::Runtime::SceneRestoreBundle>);
static_assert(!std::is_default_constructible_v<Horo::Runtime::SceneRestoreBundle>);
static_assert(!std::is_aggregate_v<Horo::Runtime::SceneRestoreBundle>);

// Brace-initialized external keys must not bypass bounded factory validation.
template <typename T>
concept ExternallyFabricableRestoreBundle = requires { T{{}, {}}; };
static_assert(!ExternallyFabricableRestoreBundle<Horo::Runtime::SceneRestoreBundle>);
static_assert(std::variant_size_v<Horo::Runtime::RestoreReferenceTarget> == 5);

static_assert(std::is_same_v<decltype(std::declval<const Horo::Runtime::PreparedRestoreReferenceGraph &>().Find(
                                 std::declval<const Horo::Runtime::SaveParticipantId &>(), std::uint64_t{1})),
                             const Horo::Runtime::ResolvedRestoreReference *>);

int main() {
    // Invalid composition must be rejected before any external owner callback or Scene mutation.
    return Horo::Runtime::SceneRestoreBundle::Create({}, {}, {}).HasError() ? 0 : 1;
}
