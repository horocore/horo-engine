#include "Horo/Application/NavigationProjectSettings.h"
#include "Horo/Application/NavigationProjectSettingsStore.h"
#include "Horo/Editor/NavigationDefinitionDocument.h"
#include "Horo/Editor/NavigationProjectSettingsSession.h"
#include "Horo/Editor/NavigationSettingsWorkflow.h"

#include <type_traits>
static_assert(!std::is_copy_constructible_v<Horo::Editor::NavigationDefinitionDocument>);
static_assert(!std::is_copy_constructible_v<Horo::Editor::NavigationProjectSettingsSession>);
static_assert(std::is_same_v<decltype(std::declval<const Horo::Application::NavigationProjectSettings &>().RuntimeProfile()),
                             const Horo::Navigation::NavigationProjectProfile &>);

static_assert(std::is_same_v<decltype(std::declval<const Horo::Editor::NavigationProjectSettingsSession &>().Definitions()),
                             Horo::Result<std::span<const Horo::Editor::NavigationDefinitionSnapshot>>>);

static_assert(std::is_copy_constructible_v<Horo::Editor::NavigationProjectSettingsSessionBorrow>);
static_assert(std::is_same_v<decltype(std::declval<const Horo::Editor::NavigationProjectSettingsSession &>().Borrow()),
                             Horo::Editor::NavigationProjectSettingsSessionBorrow>);
static_assert(noexcept(std::declval<const Horo::Editor::NavigationProjectSettingsSessionBorrow &>().Resolve()));

namespace {
    template <typename Session>
    concept ConstPreview = requires(const Session &session, Horo::Assets::AssetId asset,
                                    std::span<const Horo::Editor::NavigationDefinitionEdit> edits) { session.Preview(asset, edits); };

    template <typename Session>
    concept ConstCommit =
        requires(const Session &session, const Horo::Editor::NavigationSettingsReview &review) { session.Commit(review); };

    template <typename Session>
    concept ConstSave = requires(const Session &session) { session.SaveDocuments(); };

    template <typename Session>
    concept ConstSceneOpen =
        requires(std::filesystem::path root, const Horo::Assets::AssetRegistry &registry, Horo::DurableFileSystem &files,
                 Horo::Editor::ProjectMutationCoordinator &mutations, const Horo::Editor::SceneDocumentTransactions *activeScene) {
            Session::Open(root, registry, files, mutations, std::uint64_t{7}, activeScene);
        };

    static_assert(ConstSceneOpen<Horo::Editor::NavigationProjectSettingsSession>);
    static_assert(ConstPreview<Horo::Editor::NavigationProjectSettingsSession>);
    static_assert(!ConstCommit<Horo::Editor::NavigationProjectSettingsSession>);
    static_assert(!ConstSave<Horo::Editor::NavigationProjectSettingsSession>);
    static_assert(!std::is_default_constructible_v<Horo::Editor::NavigationProjectSettingsSession>);
}  // namespace

int main() {
    const auto malformed = Horo::Application::DecodeNavigationProjectSettings("{}");
    return malformed.HasError() ? 0 : 1;
}
