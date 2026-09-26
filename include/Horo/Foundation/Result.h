#pragma once

#include "Horo/Foundation/Assertions.h"
#include "Horo/Foundation/ErrorCode.h"

#include <optional>
#include <utility>
#include <variant>

namespace Horo {
    /** @brief Typed result for operations whose expected failure is represented by Error. */
    template <typename T> class Result {
    public:
        [[nodiscard]] static Result Success(T value) {
            return Result(std::move(value));
        }

        [[nodiscard]] static Result Failure(Error error) {
            return Result(std::move(error));
        }

        [[nodiscard]] bool HasValue() const noexcept {
            return std::holds_alternative<T>(m_value);
        }

        [[nodiscard]] bool HasError() const noexcept {
            return !HasValue();
        }

        /**
         * @brief Returns the successful value by immutable reference.
         * @pre HasValue() is true.
         * @return Borrowed value.
         */
        [[nodiscard]] const T &Value() const & {
            HORO_INVARIANT_MSG(HasValue(), "Result::Value requires a successful result.");
            return std::get<T>(m_value);
        }

        /**
         * @brief Transfers the successful value from an expiring result.
         * @pre HasValue() is true.
         * @return Owned value for immediate move.
         */
        [[nodiscard]] T &&Value() && {
            HORO_INVARIANT_MSG(HasValue(), "Result::Value requires a successful result.");
            return std::move(std::get<T>(m_value));
        }

        /**
         * @brief Returns the stored error by immutable reference.
         * @pre HasError() is true.
         * @return Borrowed error.
         */
        [[nodiscard]] const Error &ErrorValue() const & {
            HORO_INVARIANT_MSG(HasError(), "Result::ErrorValue requires a failed result.");
            return std::get<Error>(m_value);
        }

        /**
         * @brief Transfers the stored error from an expiring result.
         * @pre HasError() is true.
         * @return Owned error reference for immediate move.
         */
        [[nodiscard]] Error &&ErrorValue() && {
            HORO_INVARIANT_MSG(HasError(), "Result::ErrorValue requires a failed result.");
            return std::move(std::get<Error>(m_value));
        }

    private:
        explicit Result(T value) : m_value(std::move(value)) {}

        explicit Result(Error error) : m_value(std::move(error)) {}

        std::variant<T, Error> m_value;
    };

    /** @brief Result specialization for successful operations without a payload. */
    template <> class Result<void> {
    public:
        [[nodiscard]] static Result Success() noexcept {
            return Result();
        }

        [[nodiscard]] static Result Failure(Error error) {
            return Result(std::move(error));
        }

        [[nodiscard]] bool HasValue() const noexcept {
            return !m_error.has_value();
        }

        [[nodiscard]] bool HasError() const noexcept {
            return !HasValue();
        }

        /**
         * @brief Returns the stored error by immutable reference.
         * @pre HasError() is true.
         * @return Borrowed error.
         */
        [[nodiscard]] const Error &ErrorValue() const & {
            HORO_INVARIANT_MSG(HasError(), "Result::ErrorValue requires a failed result.");
            return *m_error;
        }

        /**
         * @brief Transfers the stored error from an expiring result.
         * @pre HasError() is true.
         * @return Owned error reference for immediate move.
         */
        [[nodiscard]] Error &&ErrorValue() && {
            HORO_INVARIANT_MSG(HasError(), "Result::ErrorValue requires a failed result.");
            return std::move(*m_error);
        }

    private:
        Result() noexcept = default;

        explicit Result(Error error) : m_error(std::move(error)) {}

        std::optional<Error> m_error;
    };
}  // namespace Horo
