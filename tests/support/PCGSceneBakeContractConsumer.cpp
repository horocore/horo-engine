#include "Horo/PCGScene/SceneBake.h"

#include <array>
#include <utility>

int main() {
    using namespace Horo;
    using namespace Horo::PCG;
    std::array<std::uint8_t, 16> bytes{};
    bytes[15] = 1;
    auto created = Editor::SceneDocumentTransactions::Create({Assets::AssetId::FromBytes(bytes), 1}, {{{1}, {}, "authored"}});
    if (created.HasError())
        return 1;
    auto document = std::move(created).Value();
    const auto capabilities = PCGCapabilitySet::Create(std::array{PCGCapability::OfflineBake, PCGCapability::SceneOutput});
    if (capabilities.HasError())
        return 2;
    const PCGSceneBakeBinding binding{GenerationTargetOwnerId::Create(1).Value(),
                                      1,
                                      capabilities.Value(),
                                      GenerationDependencyId::Create(1).Value(),
                                      {GenerationLineageId::Create(1).Value(), GeneratedSetId::Create(1).Value(),
                                       GenerationCellId::Create(1).Value()}};
    const auto target = CapturePCGSceneBakeTarget(document, binding);
    if (target.HasError() || target.Value().occupied.size() != 1)
        return 3;
    const auto before = document.Snapshot().Value();
    const std::array changes{
        Editor::SceneObjectReplacement{{2}, {}, {}, SceneSource::SceneObjectSnapshot{.id = {2}, .name = "new authored"}, {}}};
    const auto applied = document.Execute({before.session, before.revision, before.state, true, {}}, changes);
    if (applied.HasError() || !applied.Value().committed)
        return 4;
    const auto after = document.Snapshot().Value();
    if (document.Replay({after.session, after.revision, after.state, true, {}}, false).HasError())
        return 5;
    const auto undone = document.Snapshot().Value();
    return undone.objects.size() == 1 && !undone.dirty ? 0 : 6;
}
