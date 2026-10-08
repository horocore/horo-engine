#pragma once

/**
 * @file LogContext.h
 * @brief RAII diagnostic context and explicit cross-thread context forwarding.
 */

#include <cstddef>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Horo::Log {

    /** @brief A single MDC key-value pair. */
    using MdcField = std::pair<std::string, std::string>;

    /** @brief Immutable owned diagnostic context that may safely cross thread and job boundaries. */
    class LogContextSnapshot {
    public:
        LogContextSnapshot() = default;

        /**
         * @brief Creates a snapshot from already normalized key/value fields.
         * @param fields Owned ordered diagnostic fields.
         */
        explicit LogContextSnapshot(std::vector<MdcField> fields);
        /** @brief Creates a privacy boundary that excludes ambient fields when rebound, including in worker jobs.
         * @param fields Owned allowlisted context; callers must classify these values before construction.
         * @return Snapshot retaining only these fields and any explicit inner context while bound.
         */
        [[nodiscard]] static LogContextSnapshot Isolated(std::vector<MdcField> fields);
        /** @brief Returns whether rebinding this snapshot hides outer ambient context. @return Privacy-boundary state. */
        [[nodiscard]] bool IsIsolationBoundary() const noexcept;

        /**
         * @brief Returns the immutable ordered context fields.
         * @return View valid for the lifetime of this snapshot.
         */
        [[nodiscard]] std::span<const MdcField> Fields() const noexcept;

        /**
         * @brief Derives an independent snapshot with one key added or replaced.
         * @param key Stable diagnostic key.
         * @param value Diagnostic value.
         * @return Independent derived snapshot.
         */
        [[nodiscard]] LogContextSnapshot With(std::string key, std::string value) const;

    private:
        std::vector<MdcField> fields_;
        bool isolated_{};
    };

    /**
     * @brief RAII Mapped Diagnostic Context (MDC) for structured log enrichment.
     *
     * Push a set of key-value string pairs onto the thread-local MDC stack.
     * Every `LOG_*` call made within this scope automatically includes these
     * fields in its `"context"` object — no need to repeat them per log call.
     * Nested contexts accumulate; when the same key appears in multiple frames,
     * the innermost value wins.
     *
     * @par Thread safety
     * Each thread owns a private MDC stack. `LogContext` objects must not be
     * shared or transferred across threads.
     *
     * @par Usage
     * @code
     * void StartCreate(const ProjectCreationRequest& req)
     * {
     *     Horo::Log::LogContext ctx("project", req.projectName,
     *                               "op_id",   req.operationId);
     *
     *     LOG_INFO("editor.project_creation", "Worker started");
     *     // ↳ {"context":{"project":"MyGame","op_id":"abc-123"}}
     *
     *     DoWork(req); // downstream calls also inherit ctx
     * }
     * @endcode
     */
    class LogContext {
    public:
        /**
         * @brief Pushes a new MDC frame with the supplied key/value pairs.
         *
         * Arguments must be an even-count sequence of alternating string keys
         * and values.  Both `const char*` and `std::string` are accepted.
         *
         * @tparam KVs  Alternating key, value types convertible to `std::string`.
         */
        template <typename... KVs>
        explicit LogContext(KVs &&...kvs)
            requires(sizeof...(KVs) > 0)
        {
            static_assert(sizeof...(KVs) % 2 == 0, "LogContext requires an even number of key/value arguments.");
            std::vector<MdcField> fields;
            fields.reserve(sizeof...(KVs) / 2);
            CollectPairs(fields, std::forward<KVs>(kvs)...);
            m_frameIndex = PushFrame(std::move(fields));
        }

        LogContext(const LogContext &) = delete;
        LogContext &operator=(const LogContext &) = delete;
        LogContext(LogContext &&) = delete;
        LogContext &operator=(LogContext &&) = delete;

        /** @brief Pops this frame from the thread-local MDC stack. */
        ~LogContext();

        /** @brief Clears all active MDC frames for the calling thread. */
        static void ClearAll();

    private:
        std::size_t m_frameIndex;

        /** @brief Appends fields to the thread-local stack and returns the frame index. */
        static std::size_t PushFrame(std::vector<MdcField> fields);

        // Recursion base case.
        static void CollectPairs(std::vector<MdcField> &) {
            // The empty overload terminates recursive pair collection.
        }

        // Consume one key-value pair then recurse.
        template <typename K, typename V, typename... Rest>
        static void CollectPairs(std::vector<MdcField> &out, K &&key, V &&val, Rest &&...rest) {
            out.emplace_back(std::string(std::forward<K>(key)), std::string(std::forward<V>(val)));
            CollectPairs(out, std::forward<Rest>(rest)...);
        }
    };

    /** @brief RAII binding of a captured context on the calling thread. */
    class ScopedLogContext {
    public:
        /**
         * @brief Binds @p snapshot until this scope is destroyed.
         * @param snapshot Immutable context captured by the scheduling thread.
         */
        explicit ScopedLogContext(const LogContextSnapshot &snapshot);
        ScopedLogContext(const ScopedLogContext &) = delete;
        ScopedLogContext &operator=(const ScopedLogContext &) = delete;
        ScopedLogContext(ScopedLogContext &&) = delete;
        ScopedLogContext &operator=(ScopedLogContext &&) = delete;
        ~ScopedLogContext();

    private:
        std::size_t frameIndex_{};
    };

    /**
     * @brief Returns a merged snapshot of all active MDC fields for the calling thread.
     *
     * Fields are merged outermost-first; when the same key appears in multiple
     * frames the innermost (most recent) value wins.  Returns an empty vector
     * when no `LogContext` is active.
     *
     * This function is lock-free — MDC state is thread-local.
     */
    [[nodiscard]] std::vector<MdcField> GetMdcFields();

    /**
     * @brief Captures the calling thread's merged context for explicit job/thread forwarding.
     * @return Independent owned context snapshot.
     */
    [[nodiscard]] LogContextSnapshot CaptureLogContext();

}  // namespace Horo::Log
