#include "Horo/Application/WorldBakeApplication.h"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<Horo::Application::WorldBakeApplication>);
static_assert(std::is_same_v<decltype(std::declval<const Horo::Application::WorldBakeApplication &>().Published()),
                             std::shared_ptr<const Horo::Application::WorldBakeOutput>>);
static_assert(!std::is_same_v<Horo::Editor::WorldDocumentRevision, Horo::Runtime::SceneDefinitionRevision>);
static_assert(std::is_const_v<std::remove_reference_t<decltype(*std::declval<Horo::Application::WorldBakeRequest>().document)>>);

static_assert(
    std::is_same_v<decltype(Horo::Application::WorldBakeProjectCapture::replication), Horo::Network::ReplicationMetadataCapturePtr>);
static_assert(std::is_const_v<std::remove_reference_t<decltype(*std::declval<Horo::Network::ReplicationMetadataCapturePtr>())>>);

namespace {
    template <typename Capture>
    concept BypassesReplicationMetadataFactory = requires { Capture{{}}; };
    static_assert(!std::is_default_constructible_v<Horo::Network::ReplicationMetadataCapture>);
    static_assert(!BypassesReplicationMetadataFactory<Horo::Network::ReplicationMetadataCapture>);
}  // namespace
