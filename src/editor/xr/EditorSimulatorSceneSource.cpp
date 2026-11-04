#include "EditorSimulatorSceneSource.h"

#include "Horo/Runtime/Scene/PrimitiveMesh.h"
#include "Horo/Scene/SceneSource.h"
#include "Horo/XR/XRErrors.h"
#include "editor/document/EditorAssetMeshCache.h"
#include "editor/document/RuntimeSceneConversion.h"
#include "editor/document/SceneDocument.h"
#include "editor/renderer/xr/EditorXrSceneGeometry.h"

#include <algorithm>

namespace Horo::Editor {
    struct EditorSimulatorSceneSource::Impl final {
        SceneDocument document;
        Runtime::PrimitiveMeshCache primitives;
        EditorAssetMeshCache assets;
        std::optional<Runtime::RuntimeSceneDefinition> definition;
        EditorXrSceneSourceOwner source{document, primitives, assets};  // Revoked before its borrowed parents retire.
    };

    EditorSimulatorSceneSource::EditorSimulatorSceneSource(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    EditorSimulatorSceneSource::~EditorSimulatorSceneSource() = default;

    /** @copydoc EditorSimulatorSceneSource::Create */
    Result<std::unique_ptr<EditorSimulatorSceneSource>> EditorSimulatorSceneSource::Create(SceneSource::SceneSourceDocument values,
                                                                                           const Runtime::SceneDefinitionId identity) {
        using Output = Result<std::unique_ptr<EditorSimulatorSceneSource>>;
        const bool imported = std::ranges::any_of(values.objects, [](const auto &object) {
            return object.meshAsset.has_value();
        });
        if (imported || !values.prefabInstances.empty() || !values.unsupportedComponentOwners.empty())
            return Output::Failure(MakeError(XR::XRErrors::OperationUnsupported));
        auto impl = std::make_unique<Impl>();
        if (auto loaded = impl->document.LoadSaved(std::move(values.objects), std::move(values.prefabInstances)); loaded.HasError())
            return Output::Failure(loaded.ErrorValue());
        auto definition = ConvertSceneDocumentToRuntime(impl->document.Snapshot(), identity);
        if (definition.HasError())
            return Output::Failure(definition.ErrorValue());
        impl->definition.emplace(std::move(definition).Value());
        return Output::Success(std::unique_ptr<EditorSimulatorSceneSource>{new EditorSimulatorSceneSource(std::move(impl))});
    }

    /** @copydoc EditorSimulatorSceneSource::Definition */
    const Runtime::RuntimeSceneDefinition &EditorSimulatorSceneSource::Definition() const noexcept {
        return *impl_->definition;
    }

    /** @copydoc EditorSimulatorSceneSource::Port */
    EditorXrSceneSourcePort EditorSimulatorSceneSource::Port() const noexcept {
        return impl_->source.Port();
    }
}  // namespace Horo::Editor
