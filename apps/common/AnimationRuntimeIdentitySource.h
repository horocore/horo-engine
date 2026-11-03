#pragma once

/** @file AnimationRuntimeIdentitySource.h
 * @brief Non-installed application authority for non-reusable animation-runtime owner identities.
 */
#include "Horo/Animation/AnimationIdentity.h"

#include <thread>

namespace Horo::Application::Internal {
    /** @brief Explicit disjoint range assigned by the process composition root, never by a Scene counter. */
    struct AnimationRuntimeIdentityRange final {
        std::uint64_t first{};
        std::uint64_t last{};
    };

    /** @brief Allocation-free cold admission; no identity is issued on failure. */
    enum class AnimationRuntimeIdentityFailure : std::uint8_t {
        None,
        InvalidRange,
        WrongThread,
        Exhausted
    };

    /** @brief Typed identity or exact admission failure, without diagnostic allocation. */
    struct AnimationRuntimeIdentityIssue final {
        Animation::AnimationRuntimeId identity;
        AnimationRuntimeIdentityFailure failure{AnimationRuntimeIdentityFailure::InvalidRange};

        [[nodiscard]] bool HasValue() const noexcept {
            return identity.IsValid() && failure == AnimationRuntimeIdentityFailure::None;
        }
    };

    /**
     * @brief One host-owned monotonic issuer shared across Scene services and project/workspace lifetimes.
     * @details The composition root creates and retains this authority, sharing its pin with Play,
     * preview and headless factories rather than copying or restarting a counter. Separate issuers
     * require explicitly disjoint ranges. Failed later preparation consumes its issued identity.
     * This is not a global allocator, persistent identity, clock or resource owner.
     */
    class AnimationRuntimeIdentitySource final {
    public:
        /** @brief Captures the explicit range and the creating cold owner lane. */
        explicit AnimationRuntimeIdentitySource(const AnimationRuntimeIdentityRange range) noexcept : range_(range), next_(range.first) {}

        AnimationRuntimeIdentitySource(const AnimationRuntimeIdentitySource &) = delete;
        AnimationRuntimeIdentitySource &operator=(const AnimationRuntimeIdentitySource &) = delete;
        AnimationRuntimeIdentitySource(AnimationRuntimeIdentitySource &&) = delete;
        AnimationRuntimeIdentitySource &operator=(AnimationRuntimeIdentitySource &&) = delete;

        /** @brief Issues once, or rejects without advancing; the last allowed value is issued before permanent exhaustion. */
        [[nodiscard]] AnimationRuntimeIdentityIssue Issue() noexcept {
            using enum AnimationRuntimeIdentityFailure;
            if (owner_ != std::this_thread::get_id())
                return {{}, WrongThread};
            if (range_.first == 0 || range_.last < range_.first)
                return {{}, InvalidRange};
            if (next_ == 0)
                return {{}, Exhausted};
            const auto identity = Animation::AnimationRuntimeId::Create(next_).Value();
            next_ = next_ == range_.last ? 0 : next_ + 1;
            return {identity, None};
        }

    private:
        const std::thread::id owner_{std::this_thread::get_id()};
        const AnimationRuntimeIdentityRange range_;
        std::uint64_t next_{};
    };
}  // namespace Horo::Application::Internal
