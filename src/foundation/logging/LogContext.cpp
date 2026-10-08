/**
 * @file LogContext.cpp
 * @brief Thread-local MDC stack implementation.
 */

#include "Horo/Foundation/Logging/LogContext.h"

#include "Horo/Foundation/Assertions.h"

#include <algorithm>
#include <vector>

namespace Horo::Log {
    namespace {
        /**
         * @brief Per-thread MDC frame stack.
         *
         * Each `LogContext` owns one frame (a `std::vector<MdcField>`).
         * Frames are stored by index rather than pointer so that the stack
         * can grow without invalidating existing indices.
         */
        struct MdcStack {
            struct Frame {
                std::vector<MdcField> fields;
                bool isolated{};
            };

            std::vector<Frame> frames;
        };

        MdcStack &MdcState() {
            thread_local MdcStack state;
            return state;
        }

        std::size_t PushContextFrame(std::vector<MdcField> fields, const bool isolated = false) {
            auto &state = MdcState();
            const std::size_t index = state.frames.size();
            state.frames.emplace_back(std::move(fields), isolated);
            return index;
        }

        void PopContextFrame(const std::size_t index) {
            auto &frames = MdcState().frames;
            if (index >= frames.size())
                return;
            HORO_INVARIANT_MSG(index == frames.size() - 1, "Log contexts must be destroyed in last-in, first-out order.");
            frames.pop_back();
        }
    }  // namespace

    /** @copydoc LogContextSnapshot::LogContextSnapshot */
    LogContextSnapshot::LogContextSnapshot(std::vector<MdcField> fields) : fields_(std::move(fields)) {}

    /** @copydoc LogContextSnapshot::Isolated */
    LogContextSnapshot LogContextSnapshot::Isolated(std::vector<MdcField> fields) {
        LogContextSnapshot snapshot{std::move(fields)};
        snapshot.isolated_ = true;
        return snapshot;
    }

    /** @copydoc LogContextSnapshot::IsIsolationBoundary */
    bool LogContextSnapshot::IsIsolationBoundary() const noexcept {
        return isolated_;
    }

    /** @copydoc LogContextSnapshot::Fields */
    std::span<const MdcField> LogContextSnapshot::Fields() const noexcept {
        return fields_;
    }

    /** @copydoc LogContextSnapshot::With */
    LogContextSnapshot LogContextSnapshot::With(std::string key, std::string value) const {
        std::vector<MdcField> derived = fields_;
        if (const auto existing = std::ranges::find(derived, key, &MdcField::first); existing == derived.end())
            derived.emplace_back(std::move(key), std::move(value));
        else
            existing->second = std::move(value);
        return isolated_ ? Isolated(std::move(derived)) : LogContextSnapshot{std::move(derived)};
    }

    std::size_t LogContext::PushFrame(std::vector<MdcField> fields) {
        return PushContextFrame(std::move(fields));
    }

    LogContext::~LogContext() {
        PopContextFrame(m_frameIndex);
    }

    /** @copydoc ScopedLogContext::ScopedLogContext */
    ScopedLogContext::ScopedLogContext(const LogContextSnapshot &snapshot)
        : frameIndex_(PushContextFrame({snapshot.Fields().begin(), snapshot.Fields().end()}, snapshot.IsIsolationBoundary())) {}

    /** @copydoc ScopedLogContext::~ScopedLogContext */
    ScopedLogContext::~ScopedLogContext() {
        PopContextFrame(frameIndex_);
    }

    std::vector<MdcField> GetMdcFields() {
        const auto &frames = MdcState().frames;
        if (frames.empty())
            return {};

        // Merge frames outermost-first; innermost value wins on key collision.
        std::vector<MdcField> merged;
        merged.reserve(8);  // typical small field count
        for (const auto &frame : frames) {
            if (frame.isolated)
                merged.clear();
            for (const auto &field : frame.fields) {
                const auto it = std::ranges::find_if(merged, [&](const MdcField &f) {
                    return f.first == field.first;
                });
                if (it == merged.end())
                    merged.push_back(field);
                else
                    it->second = field.second;  // inner overrides outer
            }
        }
        return merged;
    }

    void LogContext::ClearAll() {
        MdcState().frames.clear();
    }

    /** @copydoc CaptureLogContext */
    LogContextSnapshot CaptureLogContext() {
        const bool isolated = std::ranges::any_of(MdcState().frames, [](const auto &frame) {
            return frame.isolated;
        });
        return isolated ? LogContextSnapshot::Isolated(GetMdcFields()) : LogContextSnapshot{GetMdcFields()};
    }
}  // namespace Horo::Log
