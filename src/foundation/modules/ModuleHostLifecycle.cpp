#include "Horo/Foundation/Assertions.h"
#include "Horo/Foundation/ModuleHost.h"

#include <algorithm>
#include <array>
#include <utility>

namespace Horo {
    namespace {
        /** @brief Returns whether a host-owned lifecycle state transition is legal. */
        [[nodiscard]] constexpr bool IsLegalTransition(const ModuleLifecycleState from, const ModuleLifecycleState to) noexcept {
            using enum ModuleLifecycleState;
            constexpr std::array<std::pair<ModuleLifecycleState, ModuleLifecycleState>, 7> kAllowed{{
                {Registered, Activating},
                {Registered, Stopped},
                {Activating, Active},
                {Activating, Failed},
                {Active, CancellationRequested},
                {CancellationRequested, Draining},
                {Draining, Stopped},
            }};
            return std::ranges::find(kAllowed, std::pair{from, to}) != kAllowed.end();
        }
    }  // namespace

    /** @copydoc ModuleHost::StateOf */
    std::optional<ModuleLifecycleState> ModuleHost::StateOf(const ModuleId &id) const noexcept {
        const auto found = std::ranges::find_if(m_states, [&id](const ModuleStateRecord &r) {
            return r.id == id;
        });
        if (found == m_states.end())
            return std::nullopt;
        return found->state;
    }

    void ModuleHost::Transition(const ModuleId &id, const ModuleLifecycleState to) noexcept {
        const auto found = std::ranges::find_if(m_states, [&id](const ModuleStateRecord &r) {
            return r.id == id;
        });
        const bool valid = found != m_states.end() && IsLegalTransition(found->state, to);
        HORO_INVARIANT_MSG(valid, "Invalid module lifecycle transition.");
        found->state = to;
    }

    void ModuleHost::RequestCancellationFrom(const std::size_t base) noexcept {
        for (std::size_t i = m_active.size(); i > base; --i) {
            const ActiveModule &active = m_active[i - 1];
            active.context->RequestShutdown();
            Transition(active.id, ModuleLifecycleState::CancellationRequested);
        }
    }

    void ModuleHost::DeactivateActiveModule(ActiveModule &active) noexcept {
        Transition(active.id, ModuleLifecycleState::Draining);
        active.context->DrainCallbacks();
        if (active.drain != nullptr)
            active.drain(*active.context);
        if (active.deactivate != nullptr)
            active.deactivate(*active.context);
        active.context.reset();
        Transition(active.id, ModuleLifecycleState::Stopped);
    }

    void ModuleHost::DeactivateFrom(const std::size_t base) noexcept {
        while (m_active.size() > base) {
            DeactivateActiveModule(m_active.back());
            m_active.pop_back();
        }
    }

    void ModuleHost::StopUnactivatedRegistered() noexcept {
        for (const ModuleDescriptor &d : m_registered) {
            if (StateOf(d.id) == ModuleLifecycleState::Registered)
                Transition(d.id, ModuleLifecycleState::Stopped);
        }
        m_registered.clear();
    }

    void ModuleHost::RollbackActivation(const ModuleId &failedId, std::unique_ptr<ModuleActivationContext> failedContext,
                                        const std::size_t base) noexcept {
        Transition(failedId, ModuleLifecycleState::Failed);
        failedContext->RequestShutdown();
        RequestCancellationFrom(base);
        failedContext->DrainCallbacks();
        failedContext.reset();
        DeactivateFrom(base);
        StopUnactivatedRegistered();
    }
}  // namespace Horo
