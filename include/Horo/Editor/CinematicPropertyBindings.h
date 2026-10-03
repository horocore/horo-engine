#pragma once

/** @file CinematicPropertyBindings.h
 * @brief Inspector property discovery and cinematic Problems/source-navigation adapters. */

#include "Horo/Cinematic/PropertyTrackRuntime.h"
#include "Horo/Foundation/BuildOutputStore.h"

namespace Horo::Editor {
    /** @brief Source position of an authored track in its sequence asset. */
    struct PropertyTrackSource final {
        Cinematic::TrackId track;
        std::uint32_t line{};
        std::uint32_t column{};
    };

    /**
     * @brief Projects cinematic failures into shared Problems history and the existing navigable diagnostic store.
     * @note Host composition owns this adapter, the canonical source path and track positions.
     * Services must outlive the adapter, which must outlive controllers borrowing Sink(). No UI or runtime mutation occurs here.
     */
    class CinematicPropertyProblems final : public Cinematic::IPropertyDiagnosticSink {
    public:
        /**
         * @brief Binds the editor's existing diagnostic capabilities and source snapshot.
         * @param diagnostics Shared editor Problems history.
         * @param output Existing source-navigable diagnostic store.
         * @param sourcePath Canonical absolute sequence source path validated by the host.
         * @param sources Track source positions from the immutable authoring snapshot.
         */
        CinematicPropertyProblems(DiagnosticsEngine &diagnostics, BuildOutputStore &output, std::string sourcePath,
                                  std::span<const PropertyTrackSource> sources);
        CinematicPropertyProblems(const CinematicPropertyProblems &) = delete;
        CinematicPropertyProblems &operator=(const CinematicPropertyProblems &) = delete;
        CinematicPropertyProblems(CinematicPropertyProblems &&) = delete;
        CinematicPropertyProblems &operator=(CinematicPropertyProblems &&) = delete;
        /** @brief Returns a borrowed mandatory reporting seam. @return Sink backed by this adapter. */
        [[nodiscard]] Cinematic::PropertyDiagnosticSink Sink() noexcept;
        /** @brief Surfaces one track-specific failure and preserves the owner error. @param event Typed cinematic evidence. */
        void Publish(const Cinematic::PropertyBindingDiagnosticEvent &event) override;

    private:
        DiagnosticsEngine &diagnostics_;
        BuildOutputStore &output_;
        std::string sourcePath_;
        std::vector<PropertyTrackSource> sources_;
    };

    /** @brief Inspector/recording view of the exact registry consumed by cinematic plans.
     * @note The frozen registry outlives this view; returned descriptors remain registry-owned. */
    class InspectorPropertyBindings final {
    public:
        /** @brief Borrows the host's shared scene registry. @param registry Frozen inspector/runtime snapshot. */
        explicit InspectorPropertyBindings(const Runtime::PropertyBindingRegistry &registry) noexcept;
        /**
         * @brief Enumerates animatable properties for one typed component without copying a registry.
         * @param componentType Inspector's selected component type.
         * @param output Caller storage for borrowed descriptor pointers.
         * @return Count or typed registry/capacity failure; read-only properties are excluded.
         */
        [[nodiscard]] Result<std::size_t> Enumerate(const Gameplay::ComponentTypeId &componentType,
                                                    std::span<const Runtime::PropertyBindingDescriptor *> output) const;
        /**
         * @brief Reads a property for inspector recording using its owner getter.
         * @param target Current editor-owner component snapshot, borrowed only for this call.
         * @return Typed value or original validating accessor failure.
         */
        [[nodiscard]] Result<Runtime::PropertyBindingValue> Read(const Cinematic::PropertyBindingTargetSnapshot &target) const;

    private:
        const Runtime::PropertyBindingRegistry &registry_;
    };
}  // namespace Horo::Editor
