#pragma once
/** @file SceneDocumentPersistenceInternal.h
 * @brief Editor mutation/recovery adapters over the single lower-owned Scene codec.
 */
#include "SceneSourceInternal.h"
#include "editor/document/SceneDocumentPersistence.h"

namespace Horo::Editor::ScenePersistenceDetail {
    using namespace SceneSource::Detail;
    [[nodiscard]] Json SceneJson(const SceneDocumentSnapshot &snapshot);
    [[nodiscard]] std::vector<std::byte> Bytes(std::string_view value);
    [[nodiscard]] std::filesystem::path RecoveryPath(const std::filesystem::path &absoluteProjectRoot);
    [[nodiscard]] std::string SceneChecksum(const Json &scene);
    [[nodiscard]] SceneFileFingerprint Fingerprint(std::string_view bytes);
}  // namespace Horo::Editor::ScenePersistenceDetail
