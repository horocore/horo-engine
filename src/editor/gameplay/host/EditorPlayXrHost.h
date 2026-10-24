#pragma once
/** @file EditorPlayXrHost.h
 * @brief Non-installed explicit application XR bridge for the existing workspace input and Play owners.
 */
#include "EditorPlayState.h"
#include "Horo/Runtime/InputCollection.h"

#include <memory>

namespace Horo::Runtime {
    class SceneActivationParticipant;
    class RuntimeSceneView;
}  // namespace Horo::Runtime

namespace Horo::Editor {
    class EditorXrSceneSourcePort;

    /** @brief Borrowed selected host; it cannot create contexts/actions/players or commit Input.
     * @details All calls are serialized on the workspace/router owner lane. The application keeps this
     * adapter and its verified native parents alive until after screen destruction and native drain.
     * Ordinary desktop omits the adapter. Explicitly selected XR preserves failures without fallback.
     */
    class IEditorPlayXrHost {
    public:
        virtual ~IEditorPlayXrHost() = default;
        /** @brief Attaches the actual workspace's current context/player-bound collection port.
         * @param input Exact existing InputRouter-issued port, after project profile installation.
         * @param geometry Weak source from the SAME workspace document and mesh owners; no parent lifetime is extended.
         * @return Retained context-bound port or typed owner/duplicate failure; attachment grants no Ready or sampling authority.
         * @post The actual Scene preparation reserves native actions/source against its real origin and candidate.
         * Failed later preparation retains native cleanup debt in the application, never in the workspace token.
         */
        [[nodiscard]] virtual Result<void> AttachInput(const Input::InputActionCollection &input,
                                                       const EditorXrSceneSourcePort &geometry) = 0;
        /** @brief Updates owned presentation values from the actual canonical Play Scene, without retaining its borrow.
         * @param scene Exact active occurrence whose entities were captured by the Scene participant.
         * @return Original source/mapping/transform failure; no camera, native call or Input publication occurs.
         */
        [[nodiscard]] virtual Result<void> UpdateScene(Runtime::RuntimeSceneView scene) = 0;
        /** @brief Observes the actual existing Play state without inventing another Play lifecycle.
         * @param state Exact canonical controller projection, including Paused/Stopping/Failed.
         * @return Original native/admission outcome; failure closes this adapter's Input producer before returning.
         */
        [[nodiscard]] virtual Result<void> SetPlayState(EditorPlayState state) = 0;
        /** @brief Closes Input admission before the workspace releases/reconfigures its context.
         * @return Actual registration revocation outcome; native frame/completion debt remains application-owned.
         * @post No append uses the detached context, including on failure. No blocking GPU/native retirement occurs.
         */
        [[nodiscard]] virtual Result<void> DetachInput() = 0;
        /** @brief Creates the narrow participant used by the actual Play Scene factory, before the UI participant.
         * @return Owned canonical Scene observer/preparation participant; it never publishes a second Scene.
         * @pre The application host outlives the factory, every returned participant and retained native retirement debt.
         */
        [[nodiscard]] virtual std::unique_ptr<Runtime::SceneActivationParticipant> MakeSceneParticipant() = 0;
        /** @brief Non-allocating logical revocation for failed-enter/destructor recovery.
         * @pre Owner lane. The host outlives the screen; native parents/completion debt stay retained by the host.
         * @post No native callback, wait, clock, Input commit or exception; next canonical cutoff commits neutral state.
         */
        virtual void RevokeInput() noexcept = 0;
    };
}  // namespace Horo::Editor
