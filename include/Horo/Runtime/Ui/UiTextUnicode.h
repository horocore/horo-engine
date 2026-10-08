#pragma once

/** @file UiTextUnicode.h
 * @brief Explicit headless Unicode data lifetime and bounded paragraph preparation.
 */

#include "Horo/Runtime/Ui/UiTextShaping.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace Horo::Runtime::Ui {
    /** @brief Explicit host-owned lifetime for the private Unicode strategy and packaged data. */
    class UiTextUnicodeRuntime final {
    public:
        /**
         * @brief Validates and copies the qualified package, then initializes the private Unicode runtime.
         * @param data Exact installed ICU 78.2 little-endian data bytes.
         * @return Runtime or typed payload, lifecycle, or allocation failure.
         * @pre Called at the host load boundary before any text preparation.
         * @details Only one runtime is admitted per process. The host explicitly calls Shutdown after retained native/result
         *          leases drain; canvas reload and result destruction never reset global data. A retired runtime cannot restart.
         *          The host serializes Create/Close/Shutdown with preparation admission and joins preparation before Shutdown.
         *          Abandoning the host without successful Shutdown retains the process data pin until process exit.
         */
        [[nodiscard]] static Result<UiTextUnicodeRuntime> Create(std::span<const std::byte> data);
        /** @brief Closes admission; retained native owners and immutable results remain alive. */
        ~UiTextUnicodeRuntime();
        /** @brief Transfers host ownership. @param other Runtime to transfer. */
        UiTextUnicodeRuntime(UiTextUnicodeRuntime &&other) noexcept;
        /** @brief Closes this owner before transfer. @param other Runtime to transfer. @return This owner. */
        UiTextUnicodeRuntime &operator=(UiTextUnicodeRuntime &&other) noexcept;
        UiTextUnicodeRuntime(const UiTextUnicodeRuntime &) = delete;
        UiTextUnicodeRuntime &operator=(const UiTextUnicodeRuntime &) = delete;
        /** @brief Closes shared preparation admission while retaining this owner lease and global Unicode state. */
        void Close() const noexcept;
        /**
         * @brief Closes admission and retires process data only after every native/result lease drains.
         * @return Success, or typed storage-exhausted evidence while dependent owners are still alive.
         * @pre Called by the host at its shutdown safe point, never from frame execution.
         */
        [[nodiscard]] Result<void> Shutdown();
        /** @brief Checks admission. @return Whether new preparation is available. */
        [[nodiscard]] bool IsActive() const noexcept;

    private:
        struct Storage;
        friend class UiTextUnicodeAnalyzer;
        friend class UiTextUnicodeAnalysis;
        explicit UiTextUnicodeRuntime(std::shared_ptr<Storage> storage) noexcept;
        std::shared_ptr<Storage> storage_;
    };

    /** @brief Explicit paragraph base policy; Auto uses Unicode first-strong with an LTR default. */
    enum class UiTextParagraphDirection : std::uint8_t {
        Auto,
        LeftToRight,
        RightToLeft,
        Count
    };

    /** @brief Immutable source and resolved paragraph evidence for one Unicode scalar. */
    struct UiTextUnicodeScalar final {
        std::uint32_t byteStart{};                               /**< Inclusive UTF-8 source offset. */
        std::uint32_t byteEnd{};                                 /**< Exclusive UTF-8 source offset. */
        std::uint32_t utf16Start{};                              /**< Private algorithm coordinate exposed only as integer evidence. */
        std::uint32_t utf16End{};                                /**< End of the complete scalar, never a surrogate boundary. */
        std::uint8_t level{};                                    /**< Resolved UAX #9 paragraph embedding level. */
        UiTextUnicodeBreak breakAfter{UiTextUnicodeBreak::None}; /**< Locale line break after this scalar. */
        bool graphemeEnd{}; /**< Unicode-version-coherent extended grapheme boundary after this scalar. */
    };

    /** @brief Immutable bounded Unicode result; copies retain a prepared slot. */
    class UiTextUnicodeAnalysis final {
    public:
        /** @brief Returns the exact original source. @return Borrowed UTF-8, valid for this lease. */
        [[nodiscard]] std::string_view Text() const noexcept;
        /** @brief Returns source-ordered Unicode evidence. @return Borrowed scalar table. */
        [[nodiscard]] std::span<const UiTextUnicodeScalar> Scalars() const noexcept;
        /** @brief Returns the explicit analyzed locale. @return Normalized language tag. */
        [[nodiscard]] UiTextLanguage Locale() const noexcept;
        /** @brief Returns content lineage. @return Exact caller revision. */
        [[nodiscard]] UiTextContentRevision Content() const noexcept;
        /** @brief Checks that this lease contains published evidence. @return True for a prepared result. */
        [[nodiscard]] bool IsValid() const noexcept;

    private:
        struct Storage;
        friend class UiTextUnicodeAnalyzer;
        explicit UiTextUnicodeAnalysis(std::shared_ptr<const Storage> storage) noexcept;
        std::shared_ptr<const Storage> storage_;
    };

    /**
     * @brief Owner-thread Unicode preparation and line reordering with bounded result slots.
     * @details Changed-content preparation may allocate bounded ICU scratch. Cache hits and immutable queries do not perform
     *          I/O or initialize services. Calls are serialized by the owning Runtime UI preparation thread.
     */
    class UiTextUnicodeAnalyzer final {
    public:
        /**
         * @brief Reserves paragraph/line scratch and immutable result slots.
         * @param runtime Explicit active host Unicode runtime.
         * @param limits Existing text byte/scalar/run/result capacities.
         * @return Analyzer or typed capacity/lifecycle/allocation failure.
         */
        [[nodiscard]] static Result<UiTextUnicodeAnalyzer> Create(const UiTextUnicodeRuntime &runtime, const UiTextShaperLimits &limits);
        /** @brief Closes native objects before releasing the Unicode runtime lease. */
        ~UiTextUnicodeAnalyzer();
        /** @brief Transfers preparation ownership. @param other Analyzer to transfer. */
        UiTextUnicodeAnalyzer(UiTextUnicodeAnalyzer &&other) noexcept;
        /** @brief Replaces ownership. @param other Analyzer to transfer. @return This analyzer. */
        UiTextUnicodeAnalyzer &operator=(UiTextUnicodeAnalyzer &&other) noexcept;
        UiTextUnicodeAnalyzer(const UiTextUnicodeAnalyzer &) = delete;
        UiTextUnicodeAnalyzer &operator=(const UiTextUnicodeAnalyzer &) = delete;
        /**
         * @brief Prepares Unicode bidi and locale break evidence, or retains the exact last cached candidate.
         * @param text UTF-8 source, never reordered in place.
         * @param content Exact nonzero content revision.
         * @param locale Explicit non-Auto normalized tag borrowed for this call and copied into the owned result cache.
         * @param direction Paragraph base direction policy.
         * @return Immutable result or typed input, capacity, slot, native, or lifecycle failure.
         */
        [[nodiscard]] Result<UiTextUnicodeAnalysis> Analyze(std::string_view text, UiTextContentRevision content,
                                                            const UiTextLanguage &locale, UiTextParagraphDirection direction);
        /**
         * @brief Applies Unicode line-specific reset and reordering after wrapping.
         * @param analysis Exact prepared source lease from this analyzer.
         * @param byteStart Inclusive source scalar boundary.
         * @param byteEnd Exclusive source scalar boundary, inside one paragraph.
         * @param visualToLogical Caller storage for source scalar indexes in visual order.
         * @param lineLevels Optional parallel output for line-resolved levels, including whitespace reset.
         * @return Number of written complete scalar indexes, or typed invalid/range/capacity failure.
         */
        [[nodiscard]] Result<std::uint32_t> OrderLine(const UiTextUnicodeAnalysis &analysis, std::uint32_t byteStart, std::uint32_t byteEnd,
                                                      std::span<std::uint32_t> visualToLogical, std::span<std::uint8_t> lineLevels = {});
        /** @brief Closes native admission; already published analysis leases remain valid. */
        void Close() noexcept;

    private:
        struct Storage;
        explicit UiTextUnicodeAnalyzer(std::unique_ptr<Storage> storage) noexcept;
        std::unique_ptr<Storage> storage_;
    };
}  // namespace Horo::Runtime::Ui
