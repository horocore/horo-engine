#include "Horo/Application/NavigationContentIntegration.h"

#include <type_traits>

int main() {
    static_assert(std::is_move_constructible_v<Horo::Application::AdmittedNavigationReleaseContent>);
    static_assert(std::is_move_constructible_v<Horo::Application::PreparedNavigationReleaseContent>);
    static_assert(std::is_same_v<decltype(Horo::Application::PreparedNavigationReleaseContent::expectations),
                                 std::vector<Horo::Navigation::NavMeshAssetContentExpectation>>);
    static_assert(std::is_same_v<decltype(std::declval<const Horo::Assets::AssetArchiveProvider &>().Members()),
                                 std::span<const Horo::Assets::AssetArchiveMember>>);
    static_assert(
        std::is_same_v<decltype(std::declval<const Horo::Assets::AssetArchiveProvider &>().Target()), const Horo::AssetCookTargetId &>);
    return 0;
}
