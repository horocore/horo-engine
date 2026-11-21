#include "Horo/Scene/SceneSource.h"

#include "SceneSourceInternal.h"

namespace Horo::SceneSource {
    namespace {
        /** @brief Shares the exact bounded source parser for both admission policies. */
        Result<SceneSourceDocument> Decode(const std::string_view contents, const Detail::SceneChildReadMode mode) {
            if (contents.empty() || contents.size() > Detail::kMaximumSceneBytes)
                return Result<SceneSourceDocument>::Failure(
                    Detail::PersistenceError(Detail::SceneInvalid, "Scene source is empty or exceeds the supported size limit."));
            return Detail::ParseScene(contents, mode);
        }
    }  // namespace

    /** @copydoc DecodeSceneSource */
    Result<SceneSourceDocument> DecodeSceneSource(const std::string_view contents) {
        return Decode(contents, Detail::SceneChildReadMode::Reject);
    }

    /** @copydoc DecodeSceneSourceWithChildReferences */
    Result<SceneSourceDocument> DecodeSceneSourceWithChildReferences(const std::string_view contents) {
        return Decode(contents, Detail::SceneChildReadMode::Activated);
    }

    /** @copydoc EncodeSceneSource */
    std::string EncodeSceneSource(const SceneSourceView &source) {
        return Detail::SceneJson(source).dump(2) + '\n';
    }
}  // namespace Horo::SceneSource
