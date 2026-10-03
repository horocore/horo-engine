#pragma once

#include "Horo/Physics/PhysicsCompoundCook.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "PhysicsConvexHullCookInternal.h"

#include <algorithm>
#include <array>

namespace Horo::Physics::CompoundDetail {
    using Writer = Detail::ConvexPayloadWriter;
    constexpr std::array<std::uint8_t, 4> Magic{'P', 'H', 'C', 'P'};
    constexpr std::uint32_t Schema = 1;
    constexpr std::uint64_t HeaderBytes = 100;
    constexpr std::uint64_t ChildHeaderBytes = 112;

    /** @brief Compound allocation/capability admission is fail-only and bounded by CanonicalV1. */
    [[nodiscard]] inline bool Bounded(const PhysicsCompoundCookLimits &limits) noexcept {
        return limits.maximumChildren > 0 && limits.maximumChildren <= PhysicsCompoundCookLimits::MaximumChildren &&
               limits.maximumPayloadBytes >= HeaderBytes &&
               limits.maximumPayloadBytes <= PhysicsConvexHullCookLimits::MaximumPayloadBytes &&
               Detail::ConvexLimitsAreBounded(limits.convex);
    }

    /** @brief Domain-separated content key binds the complete encoded semantic and target identity. */
    [[nodiscard]] inline Sha256Digest CookKey(std::span<const std::uint8_t> payload) {
        Sha256Builder hash;
        constexpr std::array<std::uint8_t, 4> domain{'P', 'C', 'K', '1'};
        (void)hash.Update(std::as_bytes(std::span{domain}));
        (void)hash.Update(std::as_bytes(payload));
        return hash.Finalize();
    }

    /** @brief Bounded little-endian reader; failed reads never advance past the supplied bytes. */
    class Reader final {
    public:
        explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

        bool Bytes(std::span<std::uint8_t> output) {
            if (output.size() > bytes_.size())
                return false;
            std::ranges::copy(bytes_.first(output.size()), output.begin());
            bytes_ = bytes_.subspan(output.size());
            return true;
        }

        bool U64(std::uint64_t &value) {
            std::array<std::uint8_t, 8> bytes{};
            if (!Bytes(bytes))
                return false;
            value = 0;
            for (unsigned i = 0; i < bytes.size(); ++i)
                value |= static_cast<std::uint64_t>(bytes[i]) << (i * 8U);
            return true;
        }

        bool U32(std::uint32_t &value) {
            std::array<std::uint8_t, 4> bytes{};
            if (!Bytes(bytes))
                return false;
            value = 0;
            for (unsigned i = 0; i < bytes.size(); ++i)
                value |= static_cast<std::uint32_t>(bytes[i]) << (i * 8U);
            return true;
        }

        bool Payload(std::uint64_t size, std::span<const std::uint8_t> &output) {
            if (size > bytes_.size())
                return false;
            output = bytes_.first(static_cast<std::size_t>(size));
            bytes_ = bytes_.subspan(static_cast<std::size_t>(size));
            return true;
        }

        [[nodiscard]] bool Finished() const noexcept {
            return bytes_.empty();
        }

    private:
        std::span<const std::uint8_t> bytes_;
    };

    template <typename T> [[nodiscard]] Result<T> Invalid(const char *reason) {
        return Result<T>::Failure(MakeError(PhysicsErrors::ShapeArtifactInvalid, reason));
    }

}  // namespace Horo::Physics::CompoundDetail
