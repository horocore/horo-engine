#pragma once

/** @file EditorDebugCapture.h @brief Bounded numeric debug capture at the editor owner boundary. */

#include "Horo/Foundation/Result.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <thread>

namespace Horo::Editor {
    struct EditorDebugCaptureRequest {
        static constexpr std::uint32_t MaximumSamples = 16;
        static constexpr std::uint32_t MinimumIntervalMs = 100;
        static constexpr std::uint32_t MaximumIntervalMs = 1000;
        std::uint64_t target{};
        std::uint32_t samples{};
        std::uint32_t intervalMs{};
    };

    enum class EditorDebugCapturePhase {
        Sampling,
        Ready,
        Cancelled,
        SceneChanged
    };

    struct EditorDebugCaptureSample {
        std::uint32_t elapsedMs{};
        std::array<float, 3> worldPosition{};
    };

    struct EditorDebugCaptureArtifact {
        std::uint64_t id{};
        EditorDebugCaptureRequest request;
        EditorDebugCapturePhase phase{};
        std::array<EditorDebugCaptureSample, EditorDebugCaptureRequest::MaximumSamples> samples{};
        std::uint32_t count{};
    };

    /** @brief One bounded ephemeral artifact. All methods belong to the editor owner thread. */
    class EditorDebugCaptureStorage final {
    public:
        using Clock = std::chrono::steady_clock;
        static constexpr auto Lifetime = std::chrono::seconds{30};
        static constexpr auto StartSpacing = std::chrono::seconds{1};

        /** @brief Admits an exact validated request if no capture is in flight and the rate limit allows it. */
        std::optional<std::uint64_t> Start(EditorDebugCaptureRequest request, Clock::time_point now) {
            Expire(now);
            if (!Valid(request) || (slot_ && slot_->phase == EditorDebugCapturePhase::Sampling) ||
                (started_ && now - *started_ < StartSpacing) || id_ == std::numeric_limits<std::uint64_t>::max())
                return std::nullopt;
            started_ = now;
            nextSample_ = now;
            slot_ = EditorDebugCaptureArtifact{.id = ++id_, .request = request, .phase = EditorDebugCapturePhase::Sampling};
            return id_;
        }

        static bool Valid(const EditorDebugCaptureRequest &request) {
            return request.target && request.samples > 0 && request.samples <= EditorDebugCaptureRequest::MaximumSamples &&
                   request.intervalMs >= EditorDebugCaptureRequest::MinimumIntervalMs &&
                   request.intervalMs <= EditorDebugCaptureRequest::MaximumIntervalMs;
        }

        /** @brief Returns an observation only when the owner is due to collect it. */
        bool Due(Clock::time_point now) {
            Expire(now);
            return slot_ && slot_->phase == EditorDebugCapturePhase::Sampling && now >= nextSample_;
        }

        /** @brief Appends one redacted observation with no catch-up bursts. */
        void Sample(const std::array<float, 3> position, Clock::time_point now) {
            if (!Due(now))
                return;
            auto &capture = *slot_;
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - *started_);
            capture.samples[capture.count++] = {.elapsedMs = static_cast<std::uint32_t>(elapsed.count()), .worldPosition = position};
            nextSample_ = now + std::chrono::milliseconds{capture.request.intervalMs};
            if (capture.count == capture.request.samples)
                capture.phase = EditorDebugCapturePhase::Ready;
        }

        /** @brief Stops publication when the exact Play scene or entity changes. */
        void RetireScene() {
            if (slot_ && slot_->phase == EditorDebugCapturePhase::Sampling)
                slot_->phase = EditorDebugCapturePhase::SceneChanged;
        }

        /** @brief Returns a copied artifact or no evidence if it expired or belongs to another request. */
        std::optional<EditorDebugCaptureArtifact> Read(std::uint64_t id, Clock::time_point now) {
            Expire(now);
            return slot_ && slot_->id == id ? slot_ : std::nullopt;
        }

        /** @brief Cancels only the exact active request. */
        bool Cancel(std::uint64_t id, Clock::time_point now) {
            Expire(now);
            if (!slot_ || slot_->id != id || slot_->phase != EditorDebugCapturePhase::Sampling)
                return false;
            slot_->phase = EditorDebugCapturePhase::Cancelled;
            return true;
        }

        /** @brief Withdraws all retained evidence before session teardown. */
        void Clear() noexcept {
            slot_.reset();
        }

    private:
        void Expire(const Clock::time_point now) {
            if (started_ && now - *started_ >= Lifetime)
                slot_.reset();
        }

        std::uint64_t id_{};
        std::optional<Clock::time_point> started_;
        Clock::time_point nextSample_{};
        std::optional<EditorDebugCaptureArtifact> slot_;
    };

    /** @brief Workspace owner callbacks for approved debug capture and authenticated observation. */
    struct EditorDebugCaptureEndpoint {
        std::string project;
        std::uint64_t authorizationRevision{};
        std::thread::id owner;
        std::function<Result<std::uint64_t>(const EditorDebugCaptureRequest &)> start;
        std::function<std::optional<EditorDebugCaptureArtifact>(std::uint64_t)> read;
        std::function<bool(std::uint64_t)> cancel;
    };
}  // namespace Horo::Editor
