#pragma once

#include "Horo/Foundation/ErrorCode.h"
#include "Horo/Foundation/Result.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>

namespace Horo::Tests {
    /** @brief Forms one non-zero typed identity and fails the active test if construction regresses. */
    template <typename Identity> [[nodiscard]] Identity IdentityValue(const std::uint64_t value) {
        auto result = Identity::Create(value);
        REQUIRE(result.HasValue());
        return result.Value();
    }

    /** @brief Verifies that one typed result preserved the expected stable error identity. */
    template <typename Value> void RequireFailureIdentity(const Result<Value> &result, const ErrorCodeDescriptor &expected) {
        REQUIRE_FALSE(result.HasValue());
        CHECK(result.ErrorValue().domain.Value() == expected.domain.Value());
        CHECK(result.ErrorValue().code.Value() == expected.code.Value());
    }

    /** @brief Concise alias for descriptor-aware result failure assertions. */
    template <typename Value> void RequireError(const Result<Value> &result, const ErrorCodeDescriptor &expected) {
        RequireFailureIdentity(result, expected);
    }

    /** @brief Verifies stable identity plus actionable diagnostic content. */
    template <typename Value> void RequireActionableError(const Result<Value> &result, const ErrorCodeDescriptor &expected) {
        RequireFailureIdentity(result, expected);
        CHECK_FALSE(result.ErrorValue().message.empty());
        CHECK_FALSE(expected.remediationHint.empty());
    }
}  // namespace Horo::Tests
