#pragma once

/** @file CliOutputPresenter.h
 * @brief Single-owner human and versioned machine presentation of typed CLI results.
 */
#include "Horo/Cli/CliDispatcher.h"
#include "Horo/Hosts/ErrorTranslation.h"

#include <ostream>

namespace Horo::Cli {
    /** @brief Presentation status retaining any rejected original error for host diagnostics. */
    struct CliPresentationOutcome final {
        int exitCode{};                     /**< Stable exit category, including zero success and one host failure. */
        std::optional<Error> rejectedError; /**< Original untranslatable failure, owned without flattening or loss. */
    };

    /** @brief Invocation-confined owner of protocol output; streams must outlive the presenter. */
    class CliOutputPresenter final {
    public:
        CliOutputPresenter(const CliOutputPresenter &) = delete;
        CliOutputPresenter &operator=(const CliOutputPresenter &) = delete;
        CliOutputPresenter(CliOutputPresenter &&) = delete;
        CliOutputPresenter &operator=(CliOutputPresenter &&) = delete;
        /**
         * @brief Creates one presentation scope over accepted command metadata and an immutable translator.
         * @param descriptor Registry-admitted output and command contract; copied into the scope.
         * @param invocation Non-zero invocation identity shared with dispatch.
         * @param translator Owned registry-backed exact error mappings, including CliErrors::HostFailure.
         * @param output Exclusive result stream.
         * @param diagnostics Human diagnostics stream, separate from result output.
         * @param mode Requested admitted encoding.
         * @param terminal Whether human progress may use terminal decoration; ignored for machine encodings.
         */
        CliOutputPresenter(CliCommandDescriptor descriptor, CliInvocationId invocation, Hosts::ErrorTranslator translator,
                           std::ostream &output, std::ostream &diagnostics, CliProgressOutputMode mode, bool terminal);
        /**
         * @brief Emits only the descriptor-declared version-one progress record, or human stderr progress.
         * @param event Bounded progress from the dispatcher mailbox.
         * @return Success, or a typed protocol admission error without writing an invalid record.
         */
        [[nodiscard]] Result<void> Progress(const CliProgressEvent &event);
        /**
         * @brief Emits exactly one terminal document/summary and closes the presentation scope.
         * @param terminal Authoritative typed dispatch outcome and correlation.
         * @return Stable exit code and any original untranslatable error, retained for diagnostics.
         * @note Repeated completion returns the original outcome without additional output. Partial failures
         * retain successful fields and canonical per-item errors; their first error determines the exit category.
         */
        [[nodiscard]] CliPresentationOutcome Complete(const CliTerminalResult &terminal);
        /**
         * @brief Emits the reserved exit-one envelope for unexpected pre-initialization host failure.
         * @return Closed presentation outcome; normal application failures must use Complete.
         */
        [[nodiscard]] CliPresentationOutcome HostFailure();

    private:
        CliCommandDescriptor descriptor_;
        CliInvocationId invocation_;
        Hosts::ErrorTranslator translator_;
        std::ostream *output_;
        std::ostream *diagnostics_;
        CliProgressOutputMode mode_;
        bool terminal_;
        bool completed_{};
        bool progressRejected_{};
        CliPresentationOutcome outcome_;
        [[nodiscard]] CliPresentationOutcome Present(const CliTerminalResult &terminal, bool hostFailure);
    };
}  // namespace Horo::Cli
