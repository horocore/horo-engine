#pragma once

/** @file EditorPlaySaveSession.h
 * @brief Non-installed, host-owned PIE save controls and immutable manager projection.
 */
#include "Horo/Foundation/Result.h"
#include "Horo/Gameplay/GameModuleHost.h"
#include "Horo/Runtime/RuntimeLifecycle.h"
#include "Horo/Runtime/Save/SaveManagerProjection.h"

#include <memory>
#include <optional>
#include <string>

namespace Horo::Editor {
    /** @brief Actual native registration metadata and privately issued installation/source leases for one Play activation.
     * @details Construct only through the current project gameplay owner. The installation revokes callback admission on
     * reload; the adapter pins its source and module code until accepted capture/restore operations retire.
     */
    struct PlaySaveGameplayBinding final {
        Gameplay::GameplayPersistenceDeclaration declaration;
        Runtime::GameplayPersistenceInstallation installation;
        std::shared_ptr<const Runtime::GameplayPersistenceAdapter> adapter;
    };

    /** @brief Independent owner operations; slot selection never grants storage or runtime authority. */
    enum class PlaySaveCommandKind {
        SaveNew,
        Load,
        Delete,
        Cancel,
        Refresh
    };

    /** @brief UI intent bound to an immutable slot projection, never a filesystem path. */
    struct PlaySaveCommand final {
        PlaySaveCommandKind kind{PlaySaveCommandKind::Refresh};
        std::optional<Runtime::SaveManagerCommand> selection;
        std::string displayName;
    };

    /** @brief One explicitly composed sandbox's presentation state, independent from Play pause state. */
    enum class PlaySaveAvailability {
        Opening,
        Ready,
        Retiring,
        Failed
    };

    /** @brief Owned immutable UI values containing neither a live Scene pointer nor a worker capability. */
    struct PlaySaveSnapshot final {
        std::uint64_t revision{};
        PlaySaveAvailability availability{PlaySaveAvailability::Opening};
        std::shared_ptr<const Runtime::SaveManagerProjection> manager;
        std::optional<Error> error;
        bool operationPending{};
        bool loading{};
    };

    /** @brief Single application-owned save composition belonging to one real Play activation.
     * @details It uses RuntimeSaveSession, canonical participants and the ordinary slot lifecycle in a unique PIE
     * environment namespace. No SceneDocument persistence or renderer implementation is reachable through this boundary.
     * Owner calls are synchronous and nonblocking; storage/codec workers retain detached inputs and their own leases.
     * The containing Scene host must remain alive until IsSettled after BeginRetirement. A new activation never reuses
     * this sandbox identity. No save claims support for an unregistered required runtime participant.
     */
    class EditorPlaySaveSession {
    public:
        virtual ~EditorPlaySaveSession() = default;
        /** @brief Borrows current owned presentation values without storage I/O. @return Immutable projection or initial absence. */
        [[nodiscard]] virtual std::shared_ptr<const PlaySaveSnapshot> Snapshot() const noexcept = 0;
        /** @brief Admits bounded UI intent on the owner thread. @param command Exact path-free intent and optional selection.
         * @return Accepted asynchronous request or original typed admission/staleness/unsupported failure. */
        [[nodiscard]] virtual Result<void> Request(const PlaySaveCommand &command) = 0;
        /** @brief Pumps capture/restore only at the real host lifecycle boundary. @param frame Current owner frame/cancellation.
         * @return Pending progress or original typed runtime/storage/retirement failure; never waits for a worker. */
        [[nodiscard]] virtual Result<void> CommitAtSafePoint(const Runtime::FrameContext &frame) = 0;
        /** @brief Closes admission and requests cancellation without releasing Scene/module owners or waiting. */
        virtual void BeginRetirement() noexcept = 0;
        /** @brief Reports actual detached-work settlement. @return True only when external owners can be retired safely. */
        [[nodiscard]] virtual bool IsSettled() const noexcept = 0;
    };
}  // namespace Horo::Editor
