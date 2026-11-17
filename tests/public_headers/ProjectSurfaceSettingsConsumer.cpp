#include "Horo/Application/ProjectSurfaceSettings.h"

#include <type_traits>

static_assert(!std::is_convertible_v<Horo::Assets::AssetId, Horo::Physics::SurfaceMaterialId>);
static_assert(!std::is_convertible_v<Horo::Physics::SurfaceMaterialId, Horo::Assets::AssetId>);

namespace {
    /** @brief Constructs real authored semantic identity through only the exported application contract. */
    Horo::Result<Horo::Application::ProjectSurfaceSettings> AuthoredCatalog() {
        using namespace Horo;
        auto id = Physics::SurfaceMaterialId::Parse("00000000-0000-0000-0000-000000000011");
        if (id.HasError())
            return Result<Application::ProjectSurfaceSettings>::Failure(std::move(id).ErrorValue());
        return Application::ProjectSurfaceSettings::Create(
            {.projectId = "surface_public_consumer", .revision = 1, .defaultSurfaceId = id.Value(), .surfaces = {{id.Value(), "Stone"}}});
    }
}  // namespace

int main() {
    using namespace Horo;
    auto authored = AuthoredCatalog();
    if (authored.HasError())
        return 1;
    auto encoded = Application::EncodeProjectSurfaceSettings(authored.Value());
    if (encoded.HasError())
        return 2;
    auto decoded = Application::DecodeProjectSurfaceSettings(encoded.Value());
    if (decoded.HasError())
        return 3;
    auto binding = decoded.Value().Capture(41);
    if (binding.HasError())
        return 4;
    if (!binding.Value().defaultSurface.has_value())
        return 5;
    auto evidence = Physics::MakePhysicsSurfaceEvidence(binding.Value(), *binding.Value().defaultSurface);
    if (!Physics::MatchesPhysicsSurfaceEvidence(binding.Value(), evidence))
        return 6;
    ++evidence.mappingGeneration;
    return Physics::MatchesPhysicsSurfaceEvidence(binding.Value(), evidence) ? 7 : 0;
}
