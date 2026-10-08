#pragma once

/** @file NavigationDefinitionSerialization.h
 * @brief Sole canonical NAVDEF01 semantic codec within the existing HNAV source envelope.
 */

#include "Horo/Navigation/NavigationDefinition.h"

namespace Horo::Navigation {
    /** @brief Encodes one validated definition into a required, nonopaque NAVDEF01 record.
     * @param definition Complete immutable authored definition.
     * @param recordId Owner-issued durable identity, not the sidecar AssetId.
     * @return Owned record with exact payload version 1.0, or a typed identity/capacity failure.
     * @note Performs no file access, migration, asset registration or cache publication.
     */
    [[nodiscard]] Result<NavigationAuthoredRecord> EncodeNavigationDefinitionRecord(const NavigationDefinition &definition,
                                                                                    NavigationAuthoredRecordId recordId);

    /** @brief Decodes and validates one exact production semantic record without legacy reinterpretation.
     * @param record Required NAVDEF01 payload 1.0, bounded to the compiled source-record ceiling.
     * @return Detached immutable definition, or a typed type/version/canonical-byte/semantic diagnostic.
     * @note Missing fields, invalid UTF-8, duplicate IDs, trailing bytes, reordered descriptors and
     * noncanonical negative zero reject. No default profile or policy is synthesized during decode.
     */
    [[nodiscard]] Result<NavigationDefinition> DecodeNavigationDefinitionRecord(const NavigationAuthoredRecord &record);
}  // namespace Horo::Navigation
