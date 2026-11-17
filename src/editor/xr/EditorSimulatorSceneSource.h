#pragma once
/** @file EditorSimulatorSceneSource.h @brief Noninstalled primitive-sample authoring source over the actual Editor document/caches. */
#include "Horo/Runtime/Scene/RuntimeSceneDefinition.h"

#include <memory>

namespace Horo::SceneSource {
    struct SceneSourceDocument;
}

namespace Horo::Editor {
    class EditorXrSceneSourcePort;

    /** @brief Owns a genuine authored document and its exact immutable definition, never a RuntimeScene. */
    class EditorSimulatorSceneSource final {
    public:
        /** @param source Already decoded values from the admitted project capture; imported mesh/prefab inputs are unsupported.
         * @param identity Actual project Scene definition identity. @return Owned source/definition pair or original validation failure. */
        [[nodiscard]] static Result<std::unique_ptr<EditorSimulatorSceneSource>> Create(SceneSource::SceneSourceDocument source,
                                                                                        Runtime::SceneDefinitionId identity);
        ~EditorSimulatorSceneSource();
        /** @return Exact document-state-derived definition to activate once in the existing host Scene owner. */
        [[nodiscard]] const Runtime::RuntimeSceneDefinition &Definition() const noexcept;
        /** @return Weak preparation port whose document/cache parents remain owned by this object. */
        [[nodiscard]] EditorXrSceneSourcePort Port() const noexcept;

    private:
        struct Impl;
        explicit EditorSimulatorSceneSource(std::unique_ptr<Impl> impl) noexcept;
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Editor
