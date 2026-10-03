#pragma once

/**
 * @file UiTemplateErrors.h
 * @brief Stable failures for load-time UI template dependency validation.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::UiTemplates::UiErrors {
    /** @brief A template graph snapshot or reference has malformed or duplicate metadata. */
    extern const ErrorCodeDescriptor TemplateGraphInvalid;
    /** @brief A referenced template is absent from the pinned catalog. */
    extern const ErrorCodeDescriptor TemplateMissing;
    /** @brief The exact accepted semantic template revision is unavailable. */
    extern const ErrorCodeDescriptor TemplateRevisionUnavailable;
    /** @brief The template schema or public interface is incompatible. */
    extern const ErrorCodeDescriptor TemplateVersionIncompatible;
    /** @brief Nested templates revisit an asset in the active lineage. */
    extern const ErrorCodeDescriptor TemplateDependencyCycle;
    /** @brief A required package is absent or incompatible with the pinned lock. */
    extern const ErrorCodeDescriptor TemplatePackageUnavailable;
    /** @brief A bounded template graph or traversal limit was exceeded. */
    extern const ErrorCodeDescriptor TemplateGraphBudgetExceeded;
    /** @brief Template graph resolution was attempted after shutdown. */
    extern const ErrorCodeDescriptor TemplateGraphShutdown;
}  // namespace Horo::UiTemplates::UiErrors
