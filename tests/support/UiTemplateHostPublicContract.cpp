#include "Horo/Application/UiTemplateHost.h"

#include <type_traits>
#include <utility>

using Horo::Application::UiTemplateCanvasProjection;
using Horo::Application::UiTemplateRuntimeLease;
static_assert(
    std::is_same_v<decltype(std::declval<const UiTemplateRuntimeLease &>().Projections()), std::span<const UiTemplateCanvasProjection>>);
static_assert(noexcept(std::declval<const UiTemplateRuntimeLease &>().Projections()));
// Runtime holds the actual layout slot. A second host pin would prevent bounded retirement from draining.
static_assert(std::is_same_v<decltype(UiTemplateCanvasProjection::layout), Horo::Runtime::Ui::UiLayoutSnapshotDescriptor>);

template <typename Owner>
concept PublicConstructionKey = requires { typename Owner::ConstructionKey; };

using DocumentOwner = Horo::UiTemplates::UiTemplateDocumentOwner;
using HostOwner = Horo::Application::UiTemplateHost;
static_assert(!PublicConstructionKey<DocumentOwner>);
static_assert(!PublicConstructionKey<HostOwner>);
template <typename Owner>
concept EmptyDocumentKeyConstruction =
    requires(std::shared_ptr<const Horo::UiTemplates::Detail::UiTemplateOwnerState> state, std::shared_ptr<const std::uint8_t> identity) {
        Owner{{}, state, identity, std::size_t{1}};
    };
template <typename Owner>
concept EmptyHostKeyConstruction =
    requires(std::unique_ptr<DocumentOwner> document, Horo::Runtime::Ui::UiHotReload runtime, Horo::Application::UiTemplateHostCook cook,
             std::vector<std::optional<Horo::Application::UiTemplateHostCook>> retired) {
        Owner{{}, std::move(document), std::move(runtime), std::move(cook), std::move(retired)};
    };
static_assert(!EmptyDocumentKeyConstruction<DocumentOwner>);
static_assert(!EmptyHostKeyConstruction<HostOwner>);
static_assert(std::is_final_v<DocumentOwner> && std::is_final_v<HostOwner>);
static_assert(!std::is_default_constructible_v<DocumentOwner> && !std::is_default_constructible_v<HostOwner>);
static_assert(!std::is_constructible_v<DocumentOwner, std::shared_ptr<const Horo::UiTemplates::Detail::UiTemplateOwnerState>,
                                       std::shared_ptr<const std::uint8_t>, std::size_t>);
static_assert(
    !std::is_constructible_v<HostOwner, std::unique_ptr<DocumentOwner>, Horo::Runtime::Ui::UiHotReload,
                             Horo::Application::UiTemplateHostCook, std::vector<std::optional<Horo::Application::UiTemplateHostCook>>>);
static_assert(std::is_same_v<decltype(&Horo::Application::UiTemplateInputLease::Capture),
                             Horo::Result<Horo::Application::UiTemplateInputLease> (*)(
                                 const std::filesystem::path &, Horo::Assets::AssetRegistrySnapshot,
                                 const Horo::Editor::ProjectMutationCoordinator &, const Horo::Packages::PackageRestoreService *,
                                 const Horo::Application::UiTemplateInputCapturePolicy &, const Horo::CancellationToken &)>);
