#pragma once

/**
 * @file Assertions.h
 * @brief Build-aware programmer assertions and always-on invariants.
 */

#include <cstdint>
#include <source_location>
#include <string_view>

namespace Horo::AssertionPolicy {
    /** @brief The kind of failed condition passed to the fail-fast boundary. */
    enum class FailureKind : std::uint8_t {
        DebugAssertion,
        Invariant,
    };

#if defined(NDEBUG)
    /** @brief Whether programmer assertions are evaluated in this translation unit. */
    inline constexpr bool DebugAssertionsEnabled = false;
#else
    /** @brief Whether programmer assertions are evaluated in this translation unit. */
    inline constexpr bool DebugAssertionsEnabled = true;
#endif

    /**
     * @brief Emits an emergency assertion record and terminates the process.
     * @param kind Debug-only programmer assertion or always-on invariant.
     * @param expression Source expression that evaluated to false.
     * @param message Optional additional context.
     * @param location Source location of the failed condition.
     */
    [[noreturn]] void FailFast(FailureKind kind, std::string_view expression, std::string_view message,
                               std::source_location location = std::source_location::current()) noexcept;
}  // namespace Horo::AssertionPolicy

/**
 * @brief Checks programmer preconditions in assertion-enabled builds.
 * @param condition Boolean expression that must hold.
 * @param message Optional diagnostic context for @ref HORO_ASSERT_MSG, evaluated only on failure.
 * @note @ref HORO_ASSERT is compiled out when NDEBUG is defined.
 */
#if defined(NDEBUG)
#define HORO_ASSERT(condition)                                                                                                             \
    do {                                                                                                                                   \
        if (false) {                                                                                                                       \
            (void)(condition);                                                                                                             \
        }                                                                                                                                  \
    } while (false)
#define HORO_ASSERT_MSG(condition, message)                                                                                                \
    do {                                                                                                                                   \
        if (false) {                                                                                                                       \
            (void)(condition);                                                                                                             \
            (void)(message);                                                                                                               \
        }                                                                                                                                  \
    } while (false)
#else
#define HORO_ASSERT(condition)                                                                                                             \
    do {                                                                                                                                   \
        if (!(condition))                                                                                                                  \
            ::Horo::AssertionPolicy::FailFast(::Horo::AssertionPolicy::FailureKind::DebugAssertion, #condition, {},                        \
                                              std::source_location::current());                                                            \
    } while (false)
#define HORO_ASSERT_MSG(condition, message)                                                                                                \
    do {                                                                                                                                   \
        if (!(condition))                                                                                                                  \
            ::Horo::AssertionPolicy::FailFast(::Horo::AssertionPolicy::FailureKind::DebugAssertion, #condition, (message),                 \
                                              std::source_location::current());                                                            \
    } while (false)
#endif

/**
 * @brief Checks an internal invariant in every build configuration.
 * @param condition Boolean expression that must hold.
 */
#define HORO_INVARIANT(condition)                                                                                                          \
    do {                                                                                                                                   \
        if (!(condition))                                                                                                                  \
            ::Horo::AssertionPolicy::FailFast(::Horo::AssertionPolicy::FailureKind::Invariant, #condition, {},                             \
                                              std::source_location::current());                                                            \
    } while (false)

/**
 * @brief Checks an internal invariant in every build configuration with context.
 * @param condition Boolean expression that must hold.
 * @param message Additional diagnostic context, evaluated only on failure.
 */
#define HORO_INVARIANT_MSG(condition, message)                                                                                             \
    do {                                                                                                                                   \
        if (!(condition))                                                                                                                  \
            ::Horo::AssertionPolicy::FailFast(::Horo::AssertionPolicy::FailureKind::Invariant, #condition, (message),                      \
                                              std::source_location::current());                                                            \
    } while (false)
