#pragma once

/** @file SimulatorProjectArtifacts.h
 * @brief Cold simulator artifact I/O composed from an existing admitted project root.
 */
#include "Horo/Editor/XRSimulatorModel.h"
#include "Horo/Platform/ProjectReadFiles.h"

#include <filesystem>

namespace Horo {
    class DurableFileSystem;
}

namespace Horo::Application::Internal {
    /** @brief Capabilities derived together by the actual project-open composition.
     * @details Source and publication must retain the same admitted root. The caller supplies
     * its existing identity/generation; this adapter never opens a root or invents authority.
     */
    struct SimulatorProjectArtifactsComposition final {
        std::shared_ptr<const Platform::IProjectReadFiles> source;
        std::shared_ptr<DurableFileSystem> publication;
        Platform::ProjectReadContext context; /**< Identity, generation, cancellation and live stop view; deadline is per operation. */
        std::filesystem::path authorizedRoot; /**< Original absolute normalized spelling, used only as a logical publication path. */
    };

    /** @brief Single-lane cold I/O with terminal thread-safe revocation and bounded synthetic capture publication. */
    class SimulatorProjectArtifacts final : public Editor::IXRSimulatorArtifacts {
    public:
        static constexpr std::string_view PublicationDirectory = ".horo/xr-simulator/captures";

        /** @brief Retain already admitted capabilities without reopening the project root.
         * @param composition Actual project-open owners and source context.
         * @return Adapter or typed invalid/stopped authority failure.
         * @throws std::bad_alloc When cold owner storage cannot be allocated.
         */
        [[nodiscard]] static Result<std::shared_ptr<SimulatorProjectArtifacts>> Create(SimulatorProjectArtifactsComposition composition);
        ~SimulatorProjectArtifacts() override;
        /** @copydoc Editor::IXRSimulatorArtifacts::ReadScenario */
        [[nodiscard]] Result<std::vector<std::byte>> ReadScenario(std::string_view relativePath) override;
        /** @copydoc Editor::IXRSimulatorArtifacts::WriteCapture
         * @details Accepts only bounded encoded recorded simulator interaction. A private sibling stage
         * is exclusively created under a destination writer lease. Before-commit failures preserve the
         * destination; after-commit errors retain the caller's true replacement receipt. Existing stages
         * are never removed. Cleanup occurs before releasing the writer lease and only for our stage.
         */
        [[nodiscard]] Result<void> WriteCapture(std::string_view name, std::span<const std::byte> bytes,
                                                AtomicFileReplacementReceipt &receipt) override;
        /** @copydoc Editor::IXRSimulatorArtifacts::PublicationNamespace */
        [[nodiscard]] std::string_view PublicationNamespace() const noexcept override;
        /** @copydoc Editor::IXRSimulatorArtifacts::Revoke
         * @details May be called from any thread. Cold I/O remains on the creation lane; revocation
         * is checked between synchronous operations and immediately before irreversible publication.
         * An already committed replacement cannot be rolled back by cancellation or revocation.
         */
        void Revoke() noexcept override;

    private:
        struct Impl;
        explicit SimulatorProjectArtifacts(std::unique_ptr<Impl> impl) noexcept;
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Application::Internal
