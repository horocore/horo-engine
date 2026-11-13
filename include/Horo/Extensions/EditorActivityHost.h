#pragma once
/** @file EditorActivityHost.h @brief Explicit owner-lane composition of copied extension activity contributions. */
#include "Horo/Extensions/EditorCommandRegistry.h"
#include "Horo/Extensions/EditorSurfaceRegistry.h"
#include "Horo/Extensions/EditorSvgIcon.h"
#include "Horo/Extensions/ExtensionRetirement.h"

#include <memory>
#include <span>
#include <string_view>

namespace Horo {
    class JobSystem;
}

namespace Horo::Extensions {
    struct EditorActivitySession;

    /** @brief Owned immutable host projection; no executable module state or native handles are carried. */
    struct EditorActivityProjection final {
        EditorSurfaceSnapshot surface;
        std::shared_ptr<const EditorSvgIcon> icon;
        std::uint64_t revision{};
    };

    /** @brief Copied command metadata, valid only while its provider generation remains active. */
    struct EditorActivityCommandProjection final {
        EditorCommandDescriptor command;
        EditorSurfaceProviderIdentity provider;
    };

    /**
     * @brief Explicit application-owned activity session authority using the existing surface registry.
     * @details All methods run on the editor owner lane. Update pumps queued semantic work before projection capture;
     * draw traversal consumes Prepared() only. Providers never execute while a registry lock is held.
     */
    class EditorActivityHost final {
    public:
        /** @brief Composes a bounded surface authority. @param jobs Application scheduler that outlives the host and its admitted work. */
        explicit EditorActivityHost(JobSystem &jobs, ExtensionAdmissionPolicy policy = {.revision = 1});
        /** @brief Borrows sealed custom-composition admission; empty grants support standard activity controls only. */
        [[nodiscard]] const ExtensionAdmissionPolicy &AdmissionPolicy() const noexcept;
        ~EditorActivityHost();
        EditorActivityHost(const EditorActivityHost &) = delete;
        EditorActivityHost &operator=(const EditorActivityHost &) = delete;
        /** @brief Returns the single surface persistence and routing authority. */
        [[nodiscard]] EditorSurfaceRegistry &Registry() noexcept;
        /** @brief Checks exact committed activation liveness without copying or invoking provider code.
         * @param provider Exact identity and generation. @return Whether the committed session still admits work. */
        [[nodiscard]] bool IsLive(const EditorSurfaceProviderIdentity &provider) const noexcept;
        /** @brief Refreshes retained projections only after registry publication changes. */
        void Update();
        /** @brief Applies the host's exclusive modal/input decision; withdrawal discards queued interactions.
         * @param admitted True only while the workspace owns input. This is an owner-lane host operation.
         */
        void SetInputAdmitted(bool admitted) noexcept;
        /** @brief Queues an exact prepared-form semantic action; dispatch occurs at the next owner-lane Update.
         * @param provider Exact live provider generation. @param activityId Registered activity identity.
         * @param nodeId Enabled action node in the current copied form. @param actionId Approved semantic action identity.
         * @param revision Exact prepared snapshot revision. @return Success or explicit stale, unknown, disabled or busy rejection.
         * @details Const refers to the host facade; the separately owned live session receives the pending command.
         * This still requires the editor owner lane and is not a thread-safe query. */
        [[nodiscard]] Result<void> QueueAction(const EditorSurfaceProviderIdentity &provider, std::string_view activityId,
                                               std::string_view nodeId, std::string_view actionId, std::uint64_t revision) const;
        /** @brief Returns retained prepared data until the next owner-lane Update. */
        [[nodiscard]] std::span<const EditorActivityProjection> Prepared() const noexcept;
        /** @brief Returns retained explicit command contributions until the next Update. */
        [[nodiscard]] std::span<const EditorActivityCommandProjection> PreparedCommands() const noexcept;
        /** @brief Queues a live registered command through the existing bounded scheduled activity action path.
         * @param id Registered command identity. @return Success or a typed stale/disabled/busy rejection.
         * @details Requires owner-lane input admission; removed/disabled action nodes cannot execute.
         */
        [[nodiscard]] Result<void> QueueCommand(std::string_view id) const;
        /** @brief Revokes every session and clears pending focus before host shutdown. */
        void BeginShutdown() noexcept;
        /** @brief Resolves copied package text for an exact live provider, with en-US fallback and no allocation.
         * @param provider Exact live provider generation. @param key Approved package localization key.
         * @param locale Active editor locale. @return Borrowed copied text or the key, valid until session retirement. */
        [[nodiscard]] std::string_view LocalizedText(const EditorSurfaceProviderIdentity &provider, std::string_view key,
                                                     std::string_view locale) const noexcept;

    private:
        friend struct EditorActivitySession;
        friend struct EditorActivityHostAccess;
        struct State;
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::Extensions
