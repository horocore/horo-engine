#include "Horo/Foundation/Sha256.h"
#include "editor/document/SceneDocumentPersistenceInternal.h"

namespace Horo::Editor::ScenePersistenceDetail {
    Json SceneJson(const SceneDocumentSnapshot &snapshot) {
        return SceneSource::Detail::SceneJson({snapshot.objects, snapshot.prefabInstances});
    }

    [[nodiscard]] std::vector<std::byte> Bytes(const std::string_view value) {
        const auto *begin = reinterpret_cast<const std::byte *>(value.data());
        return {begin, begin + value.size()};
    }

    [[nodiscard]] std::filesystem::path RecoveryPath(const std::filesystem::path &absoluteProjectRoot) {
        return absoluteProjectRoot / ".horo/local/recovery/default-scene.hororecovery";
    }

    [[nodiscard]] std::string SceneChecksum(const Json &scene) {
        const std::string canonical = scene.dump();
        return FormatSha256(
            ComputeSha256(std::span<const std::byte>{reinterpret_cast<const std::byte *>(canonical.data()), canonical.size()}));
    }

    [[nodiscard]] SceneFileFingerprint Fingerprint(const std::string_view bytes) {
        return SceneFileFingerprint{
            .exists = true,
            .byteSize = bytes.size(),
            .checksum =
                FormatSha256(ComputeSha256(std::span<const std::byte>{reinterpret_cast<const std::byte *>(bytes.data()), bytes.size()})),
        };
    }
}  // namespace Horo::Editor::ScenePersistenceDetail
