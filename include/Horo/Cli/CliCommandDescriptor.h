#pragma once

/**
 * @file CliCommandDescriptor.h
 * @brief Inert typed metadata for CLI command registration and discovery.
 */

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Horo::Cli {
    /** @brief Semantic version of the descriptor contract understood by a CLI host. */
    struct CliContractVersion final {
        std::uint32_t major{}; /**< Breaking contract generation. */
        std::uint32_t minor{}; /**< Backward-compatible feature generation. */
        std::uint32_t patch{}; /**< Backward-compatible correction generation. */

        [[nodiscard]] auto operator<=>(const CliContractVersion &) const = default;
    };

    /** @brief Stable namespaced identity of an application capability required by a command. */
    struct CliCapabilityId final {
        std::string value; /**< Canonical lowercase identity such as `horo.asset.cook`. */

        [[nodiscard]] auto operator<=>(const CliCapabilityId &) const = default;
    };

    /** @brief Hierarchical command path whose segments follow shell command order. */
    struct CommandPath final {
        std::vector<std::string> segments; /**< Canonical lowercase segments, such as `asset`, `cook`. */

        [[nodiscard]] auto operator<=>(const CommandPath &) const = default;
    };

    /** @brief CLI executable compositions that may activate a descriptor. */
    enum class CliHostKind : std::uint8_t {
        HoroEngine,
        HoroPak,
    };

    /** @brief Bit set describing every CLI host on which a descriptor may be activated. */
    enum class CliHostAvailability : std::uint8_t {
        None = 0,
        HoroEngine = 1U << 0U,
        HoroPak = 1U << 1U,
        All = 3,
    };

    /** @brief Combines host availability bits. @param lhs First bit set. @param rhs Second bit set. @return Combined set. */
    [[nodiscard]] constexpr CliHostAvailability operator|(const CliHostAvailability lhs, const CliHostAvailability rhs) noexcept {
        return static_cast<CliHostAvailability>(static_cast<std::uint8_t>(lhs) | static_cast<std::uint8_t>(rhs));
    }

    /** @brief Intersects host availability bits. @param lhs Candidate set. @param rhs Required set. @return Common bits. */
    [[nodiscard]] constexpr CliHostAvailability operator&(const CliHostAvailability lhs, const CliHostAvailability rhs) noexcept {
        return static_cast<CliHostAvailability>(static_cast<std::uint8_t>(lhs) & static_cast<std::uint8_t>(rhs));
    }

    /** @brief Value grammar declared by one long-form command option. */
    enum class CliOptionValueKind : std::uint8_t {
        Flag,
        String,
        SignedInteger,
        FloatingPoint,
        Enumeration,
        Path,
    };

    /** @brief Optional closed numeric interval attached to an integer or floating-point CLI value. */
    struct CliNumericRange final {
        std::optional<std::int64_t> minimumInteger; /**< Inclusive integer lower bound. */
        std::optional<std::int64_t> maximumInteger; /**< Inclusive integer upper bound. */
        std::optional<double> minimumNumber;        /**< Inclusive floating-point lower bound. */
        std::optional<double> maximumNumber;        /**< Inclusive floating-point upper bound. */
    };

    /** @brief Complete inert schema for one command option. */
    struct CliOptionDescriptor final {
        std::string name;                            /**< Canonical long name without the leading `--`. */
        std::optional<char> shortName;               /**< Optional single-character alias without `-`. */
        std::string summary;                         /**< Concise help text. */
        CliOptionValueKind valueKind{};              /**< Typed value grammar. */
        bool required{};                             /**< Whether callers must provide the option. */
        bool repeatable{};                           /**< Whether more than one occurrence is accepted. */
        bool sensitive{};                            /**< Whether presentation layers must redact its value. */
        std::optional<std::string> defaultValue;     /**< Canonical default spelling, when present. */
        std::vector<std::string> enumerationValues;  /**< Closed value set for `Enumeration` options. */
        CliNumericRange numericRange;                /**< Optional range for numeric values. */
        std::optional<std::string> configurationKey; /**< Setting key used when invocation input is absent. */
    };

    /** @brief Typed positional input declaration; sensitive values are deliberately unsupported. */
    struct CliPositionalDescriptor final {
        std::string name;                           /**< Stable diagnostic/help name. */
        std::string summary;                        /**< Concise help text. */
        CliOptionValueKind valueKind{};             /**< Typed value grammar; `Flag` is invalid here. */
        bool required{};                            /**< Whether this position must be present. */
        bool repeatable{};                          /**< Whether this final position consumes the remainder. */
        bool sensitive{};                           /**< Must remain false; credentials cannot be positional. */
        std::vector<std::string> enumerationValues; /**< Closed value set for `Enumeration` inputs. */
        CliNumericRange numericRange;               /**< Optional range for numeric values. */
    };

    /** @brief Output encodings a command promises to support. */
    enum class CliOutputFormat : std::uint8_t {
        None = 0,
        Human = 1U << 0U,
        Json = 1U << 1U,
        JsonLines = 1U << 2U,
    };

    /** @brief Combines output-format bits. @param lhs First bit set. @param rhs Second bit set. @return Combined set. */
    [[nodiscard]] constexpr CliOutputFormat operator|(const CliOutputFormat lhs, const CliOutputFormat rhs) noexcept {
        return static_cast<CliOutputFormat>(static_cast<std::uint8_t>(lhs) | static_cast<std::uint8_t>(rhs));
    }

    /** @brief Intersects output-format bits. @param lhs Candidate set. @param rhs Required set. @return Common bits. */
    [[nodiscard]] constexpr CliOutputFormat operator&(const CliOutputFormat lhs, const CliOutputFormat rhs) noexcept {
        return static_cast<CliOutputFormat>(static_cast<std::uint8_t>(lhs) & static_cast<std::uint8_t>(rhs));
    }

    /** @brief Versioned machine-output declaration paired with human output support. */
    struct CliOutputSchema final {
        std::string id;            /**< Stable lowercase namespaced schema identity. */
        std::uint32_t version{};   /**< Non-zero schema version. */
        CliOutputFormat formats{}; /**< Human plus at least one machine-readable format. */
        bool progressRecords{};    /**< Declares version-one progress records in JSONL; terminal summaries are always required. */
    };

    /** @brief Whether the command may request terminal interaction. */
    enum class CliInteractivePolicy : std::uint8_t {
        Forbidden,
        Optional,
        Required,
    };

    /** @brief Observable side-effect category used by host admission policy. */
    enum class CliSideEffectPolicy : std::uint8_t {
        None,
        ReadsState,
        MutatesState,
        WritesFiles,
        StartsExternalProcess,
    };

    /** @brief Cooperative cancellation behavior promised by the command adapter. */
    enum class CliCancellationPolicy : std::uint8_t {
        Unsupported,
        Cooperative,
    };

    /** @brief Bounded default and maximum execution deadline declared by a command. */
    struct CliTimeoutPolicy final {
        std::uint64_t defaultMilliseconds{}; /**< Default host deadline; zero disables timeouts. */
        std::uint64_t maximumMilliseconds{}; /**< Largest caller-selected deadline; zero disables overrides. */

        [[nodiscard]] auto operator<=>(const CliTimeoutPolicy &) const = default;
    };

    /** @brief Standard-input grammar a command is allowed to consume. */
    enum class CliStdinPolicy : std::uint8_t {
        None,
        JsonDocument,
        JsonLines,
        BinaryStream,
    };

    /** @brief Origin metadata retained for auditing without changing descriptor validation. */
    enum class CliCommandOrigin : std::uint8_t {
        BuiltIn,
        Contribution,
    };

    /** @brief Complete inert declaration consumed by the host-owned CLI command registry. */
    struct CliCommandDescriptor final {
        CommandPath path;                                        /**< Hierarchical command identity. */
        std::string summary;                                     /**< One-line help and discovery summary. */
        std::vector<CliOptionDescriptor> options;                /**< Typed option schema. */
        std::vector<CliPositionalDescriptor> positionals;        /**< Typed ordered positional schema. */
        std::vector<CliCapabilityId> requiredCapabilities;       /**< Capabilities required before activation. */
        CliOutputSchema output;                                  /**< Human and machine output contract. */
        CliInteractivePolicy interactive{};                      /**< Prompt admission policy. */
        CliHostAvailability hosts{};                             /**< Executable compositions allowed to activate it. */
        CliContractVersion contractVersion{};                    /**< CLI descriptor contract authored against. */
        CliSideEffectPolicy sideEffects{};                       /**< Highest side-effect category performed. */
        CliCancellationPolicy cancellation{};                    /**< Cooperative cancellation support. */
        CliTimeoutPolicy timeout{};                              /**< Bounded execution deadline policy. */
        CliStdinPolicy stdinPolicy{};                            /**< Explicit stdin consumption policy. */
        std::optional<std::string> interactiveAlternativeOption; /**< Option replacing prompts when interaction is unavailable. */
        CliCommandOrigin origin{};                               /**< Built-in or approved contribution provenance. */
        std::string ownerId;                                     /**< Canonical module or extension identity. */
    };
}  // namespace Horo::Cli
