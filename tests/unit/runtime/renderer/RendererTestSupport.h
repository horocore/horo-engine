#pragma once

#include "Horo/Foundation/Result.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::Render::Testing {
    template <typename ValueT> void RequireError(const Result<ValueT> &result, const ErrorCodeDescriptor &expected) {
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().domain.Value() == expected.domain.Value());
        CHECK(result.ErrorValue().code.Value() == expected.code.Value());
    }
}  // namespace Horo::Render::Testing
