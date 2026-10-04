#include "Horo/Scene/SceneSource.h"

#include "SceneSourceInternal.h"

namespace Horo::SceneSource {
    /** @copydoc DecodeSceneSource */
    Result<SceneSourceDocument> DecodeSceneSource(const std::string_view contents) {
        if (contents.empty() || contents.size() > Detail::kMaximumSceneBytes)
            return Result<SceneSourceDocument>::Failure(
                Detail::PersistenceError(Detail::SceneInvalid, "Scene source is empty or exceeds the supported size limit."));
        return Detail::ParseScene(contents);
    }

    /** @copydoc EncodeSceneSource */
    std::string EncodeSceneSource(const SceneSourceView &source) {
        return Detail::SceneJson(source).dump(2) + '\n';
    }
}  // namespace Horo::SceneSource
