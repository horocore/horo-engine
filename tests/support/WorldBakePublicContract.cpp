#include "Horo/Application/WorldBakeApplication.h"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<Horo::Application::WorldBakeApplication>);
static_assert(std::is_same_v<decltype(std::declval<const Horo::Application::WorldBakeApplication &>().Published()),
                             std::shared_ptr<const Horo::Application::WorldBakeOutput>>);
static_assert(!std::is_same_v<Horo::Editor::WorldDocumentRevision, Horo::Runtime::SceneDefinitionRevision>);
static_assert(std::is_const_v<std::remove_reference_t<decltype(*std::declval<Horo::Application::WorldBakeRequest>().document)>>);
