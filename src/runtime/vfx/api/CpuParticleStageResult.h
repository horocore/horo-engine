#pragma once

#include "Horo/Foundation/Result.h"
#include "Horo/Vfx/VfxErrors.h"

#include <array>
#include <optional>
#include <utility>
#include <variant>

namespace Horo::Vfx::Detail {
    /** @brief Static kernel failure or an explicitly legacy provider diagnostic; success never materializes Error. */
    struct CpuParticleStageFailure final {
        const ErrorCodeDescriptor *descriptor{};
        std::optional<Error> legacy;
        bool wrap{};

        /** @brief Retains exact legacy metadata; identifies known buffer failures without replacing their codes. */
        [[nodiscard]] static CpuParticleStageFailure Owned(const Error &error) {
            constexpr std::array descriptors{&VfxErrors::ParticleBufferAllocationFailed,
                                             &VfxErrors::ParticleBufferCapacityExceeded,
                                             &VfxErrors::ParticleBufferInvalid,
                                             &VfxErrors::ParticleBufferShutDown,
                                             &VfxErrors::ParticleBufferThreadViolation,
                                             &VfxErrors::ParticleHandleInvalid,
                                             &VfxErrors::ParticleHandleStale,
                                             &VfxErrors::ParticleSimulationIdentityInvalid,
                                             &VfxErrors::ParticleStableCompactionInvalid};
            const ErrorCodeDescriptor *known{};
            for (const auto descriptor : descriptors)
                if (descriptor->domain.Value() == error.domain.Value() && descriptor->code.Value() == error.code.Value())
                    known = descriptor;
            return {known, error, false};
        }

        /** @brief Materializes full legacy metadata only at the public Result translation boundary. */
        [[nodiscard]] Error Materialize() && {
            if (wrap && descriptor && legacy)
                return WrapError(*descriptor, std::move(*legacy));
            if (legacy)
                return std::move(*legacy);
            return MakeError(*descriptor);
        }
    };

    /** @brief Target-private stage carrier; static failures copy only a descriptor pointer. */
    template <typename T> class CpuParticleStageResult final {
    public:
        [[nodiscard]] static CpuParticleStageResult Success(T value) {
            return CpuParticleStageResult{std::move(value)};
        }

        [[nodiscard]] static CpuParticleStageResult Failure(CpuParticleStageFailure failure) {
            return CpuParticleStageResult{std::move(failure)};
        }

        [[nodiscard]] static CpuParticleStageResult Failure(const Error &error) {
            return Failure(CpuParticleStageFailure::Owned(error));
        }

        [[nodiscard]] bool HasError() const noexcept {
            return std::holds_alternative<CpuParticleStageFailure>(value_);
        }

        [[nodiscard]] const T &ValueRef() const {
            return std::get<T>(value_);
        }

        [[nodiscard]] const T &Value() const {
            return ValueRef();
        }

        [[nodiscard]] const CpuParticleStageFailure &ErrorValue() const {
            return std::get<CpuParticleStageFailure>(value_);
        }

        [[nodiscard]] CpuParticleStageFailure TakeFailure() {
            return std::get<CpuParticleStageFailure>(std::move(value_));
        }

    private:
        explicit CpuParticleStageResult(T value) : value_(std::move(value)) {}

        explicit CpuParticleStageResult(CpuParticleStageFailure failure) : value_(std::move(failure)) {}

        std::variant<T, CpuParticleStageFailure> value_;
    };

    template <> class CpuParticleStageResult<void> final {
    public:
        [[nodiscard]] static CpuParticleStageResult Success() {
            return CpuParticleStageResult{};
        }

        [[nodiscard]] static CpuParticleStageResult Failure(CpuParticleStageFailure failure) {
            CpuParticleStageResult result;
            result.failure_ = std::move(failure);
            return result;
        }

        [[nodiscard]] static CpuParticleStageResult Failure(const Error &error) {
            return Failure(CpuParticleStageFailure::Owned(error));
        }

        [[nodiscard]] bool HasError() const noexcept {
            return failure_.has_value();
        }

        [[nodiscard]] const CpuParticleStageFailure &ErrorValue() const {
            return *failure_;
        }

        [[nodiscard]] CpuParticleStageFailure TakeFailure() {
            return std::move(*failure_);
        }

    private:
        std::optional<CpuParticleStageFailure> failure_;
    };
}  // namespace Horo::Vfx::Detail
