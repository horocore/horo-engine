#pragma once

/** @file Sdl3AudioBackend.h
 * @brief Build-tree-only SDL3 portability/reference audio backend.
 */

#include "Horo/Audio/Internal/AudioBackend.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace Horo::Audio::Backend {
    /** @brief Fixed construction facts; construction performs no SDL initialization or discovery. */
    struct Sdl3AudioBackendConfig final {
        AudioRuntimeId owner;
        std::uint64_t clockDomain{};
    };

    /** @brief Backend-local facts; parent AudioRuntime state remains control-owned. */
    enum class Sdl3AudioBackendState : std::uint8_t {
        Closed,
        Opened,
        Priming,
        Rendering,
        Quiescing,
        Quiesced,
        Stopped
    };

    /**
     * @brief Equal-peer SDL3 output adapter with private native identity and conversion state.
     *
     * Begin and AdvanceControl run on one declared control thread. SDL invokes only the retained
     * Horo RenderPort on its audio thread; that path performs no Horo allocation, blocking, logging,
     * discovery or lifecycle policy. Stop detaches and destroys the stream before reporting success.
     */
    class Sdl3AudioBackend final : public AudioBackend {
    public:
        Sdl3AudioBackend(const Sdl3AudioBackend &) = delete;
        Sdl3AudioBackend &operator=(const Sdl3AudioBackend &) = delete;
        Sdl3AudioBackend(Sdl3AudioBackend &&) = delete;
        Sdl3AudioBackend &operator=(Sdl3AudioBackend &&) = delete;
        ~Sdl3AudioBackend() override;

        /** @copydoc AudioBackend::Kind */
        [[nodiscard]] AudioBackendKind Kind() const noexcept override;
        /** @copydoc AudioBackend::Owner */
        [[nodiscard]] AudioRuntimeId Owner() const noexcept override;

        /** @brief Return the current backend-local lifecycle fact. @return Current SDL3 adapter state. */
        [[nodiscard]] Sdl3AudioBackendState State() const noexcept;

        /** @copydoc AudioBackend::Begin */
        [[nodiscard]] Result<OperationId> Begin(const Request &request, const AudioMonotonicTimestamp &deadline) override;
        /** @copydoc AudioBackend::Poll */
        [[nodiscard]] Result<std::optional<Completion>> Poll(const OperationId &operation) override;

        /**
         * @brief Execute one admitted native control operation.
         * @return Success when its outcome is published or remains asynchronously pending.
         */
        [[nodiscard]] Result<void> AdvanceControl();

        /** @copydoc AudioBackend::AcknowledgeCompletion */
        [[nodiscard]] Result<void> AcknowledgeCompletion(const OperationId &operation) override;
        /** @copydoc AudioBackend::Cancel */
        [[nodiscard]] Result<CancelDisposition> Cancel(const OperationId &operation) override;
        /** @copydoc AudioBackend::CommitRendering */
        [[nodiscard]] Result<void> CommitRendering(const AudioDeviceEpoch &epoch) override;
        /** @copydoc AudioBackend::DrainEvents */
        [[nodiscard]] std::size_t DrainEvents(std::span<Event> output) noexcept override;

    private:
        struct Impl;
        friend Result<std::unique_ptr<Sdl3AudioBackend>> CreateSdl3AudioBackend(const Sdl3AudioBackendConfig &config);
        explicit Sdl3AudioBackend(std::unique_ptr<Impl> implementation) noexcept;
        std::unique_ptr<Impl> impl_;
    };

    /**
     * @brief Construct an inert SDL3 peer without initializing SDL or opening a device.
     * @param config Fixed runtime owner and host monotonic clock domain.
     * @return Owned backend or a stable validation/allocation failure.
     */
    [[nodiscard]] Result<std::unique_ptr<Sdl3AudioBackend>> CreateSdl3AudioBackend(const Sdl3AudioBackendConfig &config);
}  // namespace Horo::Audio::Backend
