#pragma once

#include "Horo/Foundation/ErrorCode.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Horo::Render::detail {
    /** @brief Defines the common exported descriptors for a render transfer error namespace. */
#define HORO_DETAIL_DEFINE_TRANSFER_COMMON_ERRORS(catalog)                                                                                 \
    const ErrorCodeDescriptor InvalidConfiguration = (catalog).invalidConfiguration;                                                       \
    const ErrorCodeDescriptor InvalidDescriptor = (catalog).invalidDescriptor;                                                             \
    const ErrorCodeDescriptor CapacityExceeded = (catalog).capacityExceeded;                                                               \
    const ErrorCodeDescriptor InvalidRequest = (catalog).invalidRequest;                                                                   \
    const ErrorCodeDescriptor InvalidTransition = (catalog).invalidTransition;                                                             \
    const ErrorCodeDescriptor ResultPending = (catalog).resultPending;                                                                     \
    const ErrorCodeDescriptor Cancelled = (catalog).cancelled;                                                                             \
    const ErrorCodeDescriptor TimedOut = (catalog).timedOut;                                                                               \
    const ErrorCodeDescriptor Stopped = (catalog).stopped;                                                                                 \
    const ErrorCodeDescriptor WrongThread = (catalog).wrongThread

    [[nodiscard]] inline ErrorCodeDescriptor MakeTransferErrorDescriptor(const ErrorDomainId &domain, const std::string_view suffix,
                                                                         const ErrorSeverity severity, const std::string_view summary,
                                                                         const std::string_view remediation, const bool retryable = false) {
        std::string code = domain.Value();
        code.push_back('.');
        code.append(suffix);
        return {.domain = domain,
                .code = ErrorCode{std::move(code)},
                .defaultSeverity = severity,
                .summary = summary,
                .remediationHint = remediation,
                .retryable = retryable};
    }

    enum class TransferErrorProfile {
        Query,
        Upload,
        Readback,
    };

    struct TransferErrorCatalog final {
        ErrorCodeDescriptor invalidConfiguration;
        ErrorCodeDescriptor invalidDescriptor;
        ErrorCodeDescriptor capacityExceeded;
        ErrorCodeDescriptor invalidRequest;
        ErrorCodeDescriptor invalidTransition;
        ErrorCodeDescriptor resultPending;
        ErrorCodeDescriptor cancelled;
        ErrorCodeDescriptor timedOut;
        ErrorCodeDescriptor stopped;
        ErrorCodeDescriptor wrongThread;
        std::optional<ErrorCodeDescriptor> unsupported;
        std::optional<ErrorCodeDescriptor> timestampInvalid;
        std::optional<ErrorCodeDescriptor> payloadSizeMismatch;
        std::optional<ErrorCodeDescriptor> mappingSizeMismatch;
    };

    struct TransferCommonErrorText final {
        std::string_view configurationSummary;
        std::string_view configurationRemediation;
        std::string_view descriptorSummary;
        std::string_view descriptorRemediation;
        std::string_view capacitySummary;
        std::string_view capacityRemediation;
        std::string_view invalidRequestSummary;
        std::string_view invalidRequestRemediation;
        std::string_view transitionSummary;
        std::string_view transitionRemediation;
        std::string_view pendingSummary;
        std::string_view pendingRemediation;
        std::string_view cancelledSummary;
        std::string_view cancelledRemediation;
        std::string_view timedOutSummary;
        std::string_view timedOutRemediation;
        std::string_view stoppedSummary;
        std::string_view stoppedRemediation;
        std::string_view wrongThreadSummary;
        std::string_view wrongThreadRemediation;
    };

    struct TransferSpecificErrorText final {
        std::string_view unsupportedSummary{};
        std::string_view unsupportedRemediation{};
        std::string_view timestampSummary{};
        std::string_view timestampRemediation{};
        std::string_view payloadSummary{};
        std::string_view payloadRemediation{};
        std::string_view mappingSummary{};
        std::string_view mappingRemediation{};
    };

    struct TransferErrorText final {
        TransferCommonErrorText common;
        TransferSpecificErrorText specific;
    };

    [[nodiscard]] inline TransferCommonErrorText CommonTransferErrorText() noexcept {
        return {.configurationSummary = "Renderer transfer configuration is invalid.",
                .configurationRemediation = "Provide a live renderer identity and finite compatible limits.",
                .descriptorSummary = "Renderer transfer descriptor is invalid.",
                .descriptorRemediation = "Use a live transfer source or destination, bounded range, alignment, and positive timeout.",
                .capacitySummary = "Renderer transfer capacity is exhausted.",
                .capacityRemediation = "Complete, acquire, consume, cancel, or retire prior transfer requests before retrying.",
                .invalidRequestSummary = "Renderer transfer identity is not live in this queue.",
                .invalidRequestRemediation = "Use a request issued by the active renderer transfer queue.",
                .transitionSummary = "Renderer transfer lifecycle transition is invalid.",
                .transitionRemediation = "Follow pending, submitted, completion, and acknowledgement order.",
                .pendingSummary = "Renderer transfer result is not ready.",
                .pendingRemediation = "Poll readiness later without blocking the renderer owner thread.",
                .cancelledSummary = "Renderer transfer was cancelled.",
                .cancelledRemediation = "Discard the acknowledged terminal request.",
                .timedOutSummary = "Renderer transfer exceeded its caller-owned deadline.",
                .timedOutRemediation = "Retire submitted backend work and discard the terminal request.",
                .stoppedSummary = "Renderer transfer admission is stopped.",
                .stoppedRemediation = "Do not submit new transfers during shutdown.",
                .wrongThreadSummary = "Renderer transfer lifecycle access used the wrong thread.",
                .wrongThreadRemediation = "Call lifecycle methods on the creating render-capable owner thread."};
    }

    [[nodiscard]] inline TransferErrorText TransferErrorTextFor(const TransferErrorProfile profile) noexcept {
        using enum TransferErrorProfile;
        const TransferCommonErrorText common = CommonTransferErrorText();
        switch (profile) {
            case Query:
                return {.common = common,
                        .specific = {.unsupportedSummary = "The selected renderer backend does not support timestamp queries.",
                                     .unsupportedRemediation = "Select a backend with timestamp-query capability; no CPU fallback is used.",
                                     .timestampSummary = "Renderer timestamp value is invalid.",
                                     .timestampRemediation = "Publish a finite non-negative monotonic timestamp."}};
            case Upload:
                return {.common = common,
                        .specific = {.payloadSummary = "Renderer upload payload size does not match its descriptor.",
                                     .payloadRemediation = "Provide exactly the admitted number of source bytes."}};
            case Readback:
                return {.common = common,
                        .specific = {.mappingSummary = "Renderer readback mapping size does not match its request.",
                                     .mappingRemediation = "Map exactly the admitted byte range before publication."}};
        }
        return {.common = common};
    }

    [[nodiscard]] inline TransferErrorCatalog MakeTransferErrorCatalog(const ErrorDomainId &domain, const TransferErrorProfile profile) {
        using enum ErrorSeverity;
        const TransferErrorText text = TransferErrorTextFor(profile);
        TransferErrorCatalog
            catalog{.invalidConfiguration =
                        MakeTransferErrorDescriptor(domain, "invalid_configuration", Error, text.common.configurationSummary,
                                                    text.common.configurationRemediation),
                    .invalidDescriptor = MakeTransferErrorDescriptor(domain, "invalid_descriptor", Error, text.common.descriptorSummary,
                                                                     text.common.descriptorRemediation),
                    .capacityExceeded = MakeTransferErrorDescriptor(domain, "capacity_exceeded", Warning, text.common.capacitySummary,
                                                                    text.common.capacityRemediation, true),
                    .invalidRequest = MakeTransferErrorDescriptor(domain, "invalid_request", Error, text.common.invalidRequestSummary,
                                                                  text.common.invalidRequestRemediation),
                    .invalidTransition = MakeTransferErrorDescriptor(domain, "invalid_transition", Error, text.common.transitionSummary,
                                                                     text.common.transitionRemediation),
                    .resultPending = MakeTransferErrorDescriptor(domain, "result_pending", Info, text.common.pendingSummary,
                                                                 text.common.pendingRemediation, true),
                    .cancelled = MakeTransferErrorDescriptor(domain, "cancelled", Info, text.common.cancelledSummary,
                                                             text.common.cancelledRemediation),
                    .timedOut = MakeTransferErrorDescriptor(domain, "timed_out", Warning, text.common.timedOutSummary,
                                                            text.common.timedOutRemediation),
                    .stopped =
                        MakeTransferErrorDescriptor(domain, "stopped", Warning, text.common.stoppedSummary, text.common.stoppedRemediation),
                    .wrongThread = MakeTransferErrorDescriptor(domain, "thread_affinity_violation", Error, text.common.wrongThreadSummary,
                                                               text.common.wrongThreadRemediation)};
        if (profile == TransferErrorProfile::Query) {
            catalog.unsupported = MakeTransferErrorDescriptor(domain, "unsupported", Info, text.specific.unsupportedSummary,
                                                              text.specific.unsupportedRemediation);
            catalog.timestampInvalid = MakeTransferErrorDescriptor(domain, "timestamp_invalid", Error, text.specific.timestampSummary,
                                                                   text.specific.timestampRemediation);
        } else if (profile == TransferErrorProfile::Upload) {
            catalog.payloadSizeMismatch = MakeTransferErrorDescriptor(domain, "payload_size_mismatch", Error, text.specific.payloadSummary,
                                                                      text.specific.payloadRemediation);
        } else {
            catalog.mappingSizeMismatch = MakeTransferErrorDescriptor(domain, "mapping_size_mismatch", Error, text.specific.mappingSummary,
                                                                      text.specific.mappingRemediation);
        }
        return catalog;
    }
}  // namespace Horo::Render::detail
