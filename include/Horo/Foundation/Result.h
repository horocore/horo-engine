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

        /**
         * @brief Constructs the successful value directly in its result storage.
         * @param args Arguments forwarded to the value constructor.
         * @return A successful result owning the constructed value, without intermediate value moves.
         * @throws Any exception raised by the value constructor.
         */
        template <typename... Args> [[nodiscard]] static Result SuccessInPlace(Args &&...args) {
            return Result(std::in_place_type<T>, std::forward<Args>(args)...);
        }

        /**
         * @brief Prepares a failure in final return storage and synchronously completes that result in place.
         * @param fallback Initial failure copied into final storage before completion starts.
         * @param complete Synchronous completion receiving a call-scoped reference to the final result.
         * @return The completed result without moving it after completion; return this factory as a prvalue.
         * @throws Exceptions from preparing the initial failure or running completion; partial result storage is destroyed.
         * @details Completion must not retain its result reference. It owns exception containment and the
         * allocation contracts of its state transitions; this factory performs no implicit failure translation.
         */
        template <typename Completion>
            requires requires(Completion &&complete, Result &result) { std::forward<Completion>(complete)(result); }
        [[nodiscard]] static Result CompleteInPlace(const Error &fallback, Completion &&complete) {
            return Result(InPlaceCompletion{}, fallback, std::forward<Completion>(complete));
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
        struct InPlaceCompletion final {};

        template <typename Completion>
        Result(InPlaceCompletion, const Error &fallback, Completion &&complete) : m_value(std::in_place_type<Error>, fallback) {
            std::forward<Completion>(complete)(*this);
        }

        template <typename... Args>
        explicit Result(std::in_place_type_t<T>, Args &&...args) : m_value(std::in_place_type<T>, std::forward<Args>(args)...) {}

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

        /** @brief Prepares a failure in final return storage, then synchronously invokes completion.
         * @param fallback Initial failure copied into final storage before completion starts.
         * @param complete Synchronous completion; must not retain the call-scoped result reference.
         * @return Completed result without a subsequent result move; return this factory as a prvalue.
         * @throws Exceptions preparing the initial failure or running completion; partial result storage is destroyed.
         * @details Completion owns exception containment and all allocation contracts of its state transitions.
         */
        template <typename Completion>
            requires requires(Completion &&complete, Result &result) { std::forward<Completion>(complete)(result); }
        [[nodiscard]] static Result CompleteInPlace(const Error &fallback, Completion &&complete) {
            return Result(InPlaceCompletion{}, fallback, std::forward<Completion>(complete));
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
        struct InPlaceCompletion final {};

        template <typename Completion> Result(InPlaceCompletion, const Error &fallback, Completion &&complete) : m_error(fallback) {
            std::forward<Completion>(complete)(*this);
        }

        Result() noexcept = default;

        explicit Result(Error error) : m_error(std::move(error)) {}

        std::optional<Error> m_error;
    };
}  // namespace Horo
