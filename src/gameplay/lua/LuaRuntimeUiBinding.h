#pragma once
/** @file LuaRuntimeUiBinding.h
 * @brief Target-private VM scope revocation and closed Runtime UI transport.
 */
#include "Horo/Gameplay/Behavior.h"
#include "Horo/Runtime/Ui/RuntimeUiClient.h"

#include <array>
struct lua_State;

namespace Horo::Gameplay::Detail {
    /** @brief Fixed weak VM inventory; successful source replacement revokes before any old VM callback runs. */
    class LuaRuntimeUiScopes final {
    public:
        /** @brief Derives and tracks one VM child. @param parent Explicit runner grant. @return Child or typed finite-capacity failure. */
        [[nodiscard]] Result<std::shared_ptr<const Runtime::Ui::RuntimeUiClient>> Bind(
            const std::shared_ptr<const Runtime::Ui::RuntimeUiClient> &parent);
        /** @brief Permanently revokes all observed VM scopes without executing a callback. */
        void Revoke() noexcept;

    private:
        std::array<std::weak_ptr<const Runtime::Ui::RuntimeUiClient>, 256> scopes_;
    };

    /** @brief Installs typed retryable terminal/event observation methods on the top closed method table.
     * @param state Borrowed VM inside protected context construction; no callback or VM pointer is retained.
     */
    void InstallLuaRuntimeUiObservationMethods(lua_State *state);

    /** @brief Pushes an opaque child capability (including typed unavailable methods when missing).
     * @param state Exact VM. @param client Host-owned VM child, never a BehaviorContext/Scene reference.
     */
    void PushLuaRuntimeUi(lua_State *state, const std::shared_ptr<const Runtime::Ui::RuntimeUiClient> &client);
}  // namespace Horo::Gameplay::Detail
