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
        struct MdcStack;
        thread_local MdcStack *activeMdcState{};

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

            std::vector<Frame> frames{std::size_t{0}};

            MdcStack() {
                activeMdcState = this;
            }

            ~MdcStack() {
                activeMdcState = nullptr;
            }

            MdcStack(const MdcStack &) = delete;
            MdcStack &operator=(const MdcStack &) = delete;
        };

        MdcStack &MdcState() {
            thread_local MdcStack state;
            return state;
        }

        /** @brief Adopts fields without a checked-STL vector move that can allocate inside noexcept. */
        std::shared_ptr<const std::vector<MdcField>> OwnSnapshotFields(std::vector<MdcField> &fields) {
            if (fields.empty())
                return {};
            auto owned = std::make_shared<std::vector<MdcField>>(std::size_t{0});
            owned->swap(fields);
            return owned;
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

        /** @brief Merges into unpublished caller-owned storage; inner fields and privacy boundaries take precedence. */
        void MergeFrames(const MdcStack &state, std::vector<MdcField> &merged) {
            merged.reserve(8);
            for (const auto &frame : state.frames) {
                if (frame.isolated)
                    merged.clear();
                for (const auto &field : frame.fields) {
                    const auto it = std::ranges::find_if(merged, [&field](const MdcField &entry) {
                        return entry.first == field.first;
                    });
                    if (it == merged.end())
                        merged.push_back(field);
                    else
                        it->second = field.second;
                }
            }
        }
    }  // namespace

    /** @copydoc LogContextSnapshot::LogContextSnapshot */
    LogContextSnapshot::LogContextSnapshot(std::vector<MdcField> fields) : fields_(OwnSnapshotFields(fields)) {}

    /** @copydoc LogContextSnapshot::Isolated */
    LogContextSnapshot LogContextSnapshot::Isolated(std::vector<MdcField> fields) {
        LogContextSnapshot snapshot;
        snapshot.fields_ = OwnSnapshotFields(fields);
        snapshot.isolated_ = true;
        return snapshot;
    }

    /** @copydoc LogContextSnapshot::IsIsolationBoundary */
    bool LogContextSnapshot::IsIsolationBoundary() const noexcept {
        return isolated_;
    }

    /** @copydoc LogContextSnapshot::Fields */
    std::span<const MdcField> LogContextSnapshot::Fields() const noexcept {
        return fields_ ? std::span<const MdcField>{*fields_} : std::span<const MdcField>{};
    }

    /** @copydoc LogContextSnapshot::With */
    LogContextSnapshot LogContextSnapshot::With(std::string key, std::string value) const {
        const auto fields = Fields();
        std::vector<MdcField> derived(fields.begin(), fields.end());
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
        std::vector<MdcField> merged(std::size_t{0});
        if (activeMdcState && !activeMdcState->frames.empty())
            MergeFrames(*activeMdcState, merged);
        return merged;
    }

    void LogContext::ClearAll() {
        MdcState().frames.clear();
    }

    /** @copydoc CaptureLogContext */
    LogContextSnapshot CaptureLogContext() {
        if (!activeMdcState || activeMdcState->frames.empty())
            return {};
        auto fields = std::make_shared<std::vector<MdcField>>(std::size_t{0});
        MergeFrames(*activeMdcState, *fields);
        LogContextSnapshot snapshot;
        if (!fields->empty())
            snapshot.fields_ = std::move(fields);
        snapshot.isolated_ = std::ranges::any_of(activeMdcState->frames, [](const auto &frame) {
            return frame.isolated;
        });
        return snapshot;
    }
}  // namespace Horo::Log
