#pragma once
/** @file ColorOutputCookInternal.h @brief Private bounded cold source/identity helpers shared by this product cook. */
#include "ColorOutputCook.h"
#include "ColorOutputShaderRecipe.h"

#include <stdexcept>
#include <utility>

namespace Horo::Editor::Offline::ColorCookDetail {
    using Bytes = std::vector<std::uint8_t>;

    /** @brief Preserves original typed failures across the synchronous cold decoder boundary. */
    struct Failure final {
        Error error;
    };

    /** @brief Preserves one actual failing owner result without manufacturing success metadata. */
    template <class T> T Take(Result<T> result) {
        if (result.HasError())
            throw Failure{result.ErrorValue()};
        return std::move(result).Value();
    }

    /** @brief Preserves a failing synchronous validation result. @param result Actual owner result. */
    inline void Take(const Result<void> &result) {
        if (result.HasError())
            throw Failure{result.ErrorValue()};
    }

    /** @brief Rejects inconsistent cold data before publishing anything. @param valid Required predicate. */
    void Require(bool valid);
    /** @brief Creates the product's bounded malformed-input failure. @return Typed shader-tool output failure. */
    [[nodiscard]] Error Invalid();
    /** @brief Validates an absolute ordinary source root. @param root Explicit directory. @return Canonical actual root. */
    [[nodiscard]] std::filesystem::path Root(const std::filesystem::path &root);
    /** @brief Reads one exact child while rejecting symlinks/escapes. @param root Canonical directory.
     * @param relative Fixed relative file. @param maximum Finite byte limit. @return Actual bytes or preserved failure.
     */
    [[nodiscard]] Bytes Read(const std::filesystem::path &root, std::string_view relative, std::size_t maximum);
    /** @brief Hashes actual source bytes. @param bytes Exact data. @return SHA256 integrity, never trust. */
    [[nodiscard]] Sha256Digest Hash(const Bytes &bytes) noexcept;

    /** @brief Actual compile family plus original source and analytic ownership; no native execution is implied. */
    struct Family final {
        Render::ShaderTargetRequirement target;
        std::array<Render::ShaderManifest, 3> manifests;
        std::array<Render::ShaderCompilationBatch, 3> compiled;
        ColorOutputAnalyticBundle analytic;
        std::array<Bytes, 3> wrappers;
        std::array<Bytes, 3> sources;
        std::size_t programCount{2};
    };

    /** @brief Publishes a complete genuine family through the shared atomic writer.
     * @param destination New directory. @param family Actual compile/source generation. @param host Independently authenticated host.
     * @return Success after decoding the exact written generation, otherwise original typed failure.
     */
    [[nodiscard]] Result<void> Publish(const std::filesystem::path &destination, const Family &family,
                                       const Application::Internal::AuthenticatedMetalCookConfiguration &host);
}  // namespace Horo::Editor::Offline::ColorCookDetail
