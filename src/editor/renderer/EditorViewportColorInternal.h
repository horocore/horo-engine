#pragma once
/** @file EditorViewportColorInternal.h @brief Private synchronous cold resource preparation with preserved typed failures. */
#include "EditorRendererErrors.h"
#include "EditorViewportColorPipeline.h"

#include <utility>

namespace Horo::Editor::ViewportColorDetail {
    /** @brief Preserves a cold frontend failure across one transactional initialization boundary. */
    struct Failure final {
        Error error;
    };

    /** @brief Extracts an actual owner result or preserves its original typed failure. */
    template <class T> T Take(Result<T> result) {
        if (result.HasError())
            throw Failure{result.ErrorValue()};
        return std::move(result).Value();
    }

    /** @brief Preserves one actual void result without manufacturing success. */
    inline void Take(Result<void> result) {
        if (result.HasError())
            throw Failure{result.ErrorValue()};
    }

    /** @brief Retains the first original teardown failure while allowing independent retirement attempts. */
    inline void Remember(std::optional<Error> &first, const Result<void> &result) {
        if (result.HasError() && !first)
            first = result.ErrorValue();
    }

    /** @brief Attempts one resource once, preserving its handle when the owner refuses retirement. */
    template <class Handle, class Release> void Retire(Handle &handle, Release release, std::optional<Error> &first) {
        if (!handle.IsValid())
            return;
        const auto result = release(handle);
        Remember(first, result);
        if (result.HasValue())
            handle = {};
    }

    /** @brief Returns a retained original teardown failure or actual complete retirement admission. */
    inline Result<void> Outcome(const std::optional<Error> &first) {
        return first ? Result<void>::Failure(*first) : Result<void>::Success();
    }

    /** @brief Requires an exact cold program/resource invariant. */
    inline void Require(bool condition) {
        if (!condition)
            throw Failure{MakeError(RendererErrors::ViewportShaderContractInvalid)};
    }
}  // namespace Horo::Editor::ViewportColorDetail
