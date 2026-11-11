#pragma once

/** @file ConsolePresentation.h @brief Runtime terminal input ownership and bounded read-only overlay projections. */
#include "Horo/Debug/CommandExecutor.h"
#include "Horo/Foundation/Logging/StructuredLogStore.h"
#include "Horo/Runtime/Input.h"

namespace Horo::Debug {
    /** @brief Optional terminal controller, independent of editor ImGui or a renderer backend.
     * The graphical host paints its runtime UI from these bounded projections. All methods run on
     * the Input/history owner. Router and executor outlive this controller; destruction releases focus.
     */
    class ConsoleTerminal final {
    public:
        /** @brief Binds existing owners without creating a UI, input backend or second history store.
         * @param input Existing router. @param executor Same dispatcher used by all console surfaces.
         * @param history Existing visible history. @param enabled Fixed profile/launch admission.
         */
        ConsoleTerminal(Input::InputRouter &input, CommandExecutor &executor, ConsoleHistory &history, bool enabled);
        ConsoleTerminal(const ConsoleTerminal &) = delete;
        ConsoleTerminal &operator=(const ConsoleTerminal &) = delete;
        /** @brief Acquires text focus and a central input exclusion context.
         * @param passThrough Explicit inspection policy; false suppresses lower gameplay contexts.
         * @return Success or typed profile/focus/thread failure.
         */
        [[nodiscard]] Result<void> Open(bool passThrough = false);
        /** @brief Releases the RAII context and its text focus. Host calls on suspension/focus loss/shutdown. */
        void Close() noexcept;
        /** @brief Submits through the common policy/parser/dispatcher only while focus is eligible.
         * @param text Command line. @param principal Current authenticated host principal.
         * @return Immediate output or asynchronous store identities; malformed/denied input is never added to history.
         */
        [[nodiscard]] Result<CommandReceipt> Submit(std::string_view text, const DebugCommandContext &principal);
        /** @brief Takes committed Unicode text and native pre-edit from the central focus owner once per input frame.
         * @return Owned delivery or no eligible text; wrong-thread access fails before touching the router.
         * @note Host runtime UI owns editing, selection and native text-input activation; this does not execute a command.
         */
        [[nodiscard]] Result<std::optional<Input::TextInputDelivery>> TakeText();
        /** @brief Reports central focus eligibility. @return True only while this context may route. */
        [[nodiscard]] bool IsOpen() const noexcept;
        /** @brief Returns a bounded virtual window without allocating/copying rows.
         * @param first First retained row. @param count At most 128 rows are exposed per window.
         * @return Borrowed history rows, invalidated by history append/clear/destruction.
         */
        [[nodiscard]] std::span<const HistoryEntry> Rows(std::size_t first, std::size_t count) const noexcept;
        /** @brief Returns retained history cardinality for virtual scrolling. @return Bounded row count. */
        [[nodiscard]] std::size_t RowCount() const noexcept;

    private:
        Input::InputRouter &input_;
        CommandExecutor &executor_;
        ConsoleHistory &history_;
        bool enabled_{};
        std::thread::id owner_;
        Input::InputContextToken focus_;
    };

    /** @brief Read-only adapter over the actual process log and operation stores.
     * No production, persistence, per-sample bus or second history is introduced. Refresh is capped
     * at four times per second. Painting slices borrowed immutable snapshots and allocates no rows.
     * Hidden panels query nothing; snapshots are dropped when hidden. Owner-thread only.
     */
    class ConsoleStoreOverlays final {
    public:
        /** @brief Binds read-only process stores; does not query until shown and refreshed.
         * @param logs Existing log authority. @param operations Existing operation authority.
         */
        ConsoleStoreOverlays(const Log::IStructuredLogQuery &logs, const IOperationQuery &operations);
        /** @brief Changes visibility independently and drops hidden projections.
         * @param logs Logs panel visibility. @param jobs Operations panel visibility.
         */
        void SetVisible(bool logs, bool jobs);
        /** @brief Queries changed snapshots at bounded cadence; disabled path does no allocation or query.
         * @param now Host monotonic presentation time. @return Number of changed projections or a source-cardinality failure.
         */
        [[nodiscard]] Result<std::size_t> Refresh(std::chrono::steady_clock::time_point now);
        /** @brief Returns a virtualized log window, capped to 128 rows and 4096 retained display entries.
         * @param first First row. @param count Requested visible count. @return Borrowed source-owned records.
         */
        [[nodiscard]] std::span<const std::shared_ptr<const Log::StructuredLogRecord>> Logs(std::size_t first,
                                                                                            std::size_t count) const noexcept;
        /** @brief Returns a virtualized operations window under the same display bounds.
         * @param first First row. @param count Requested visible count. @return Borrowed authoritative snapshot records.
         */
        [[nodiscard]] std::span<const OperationRecord> Jobs(std::size_t first, std::size_t count) const noexcept;
        /** @brief Returns accepted log cardinality for virtual scrolling. @return Zero while hidden/unavailable. */
        [[nodiscard]] std::size_t LogRowCount() const noexcept;
        /** @brief Returns accepted operation cardinality for virtual scrolling. @return Zero while hidden/unavailable. */
        [[nodiscard]] std::size_t JobRowCount() const noexcept;

    private:
        const Log::IStructuredLogQuery &logs_;
        const IOperationQuery &operations_;
        bool logsVisible_{}, jobsVisible_{};
        std::chrono::steady_clock::time_point nextLogRefresh_{};
        std::chrono::steady_clock::time_point nextJobRefresh_{};
        std::optional<Log::StructuredLogSnapshot> logSnapshot_;
        std::optional<OperationStoreSnapshot> operationSnapshot_;
    };
}  // namespace Horo::Debug
