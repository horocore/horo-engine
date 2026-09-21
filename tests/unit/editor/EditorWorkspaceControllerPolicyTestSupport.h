#pragma once

#include "Horo/Assets/MeshEditorPayload.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace HoroEditorWorkspaceControllerPolicyTests {
    using namespace Horo;
    using namespace Horo::Editor;

    inline const ErrorCodeDescriptor InjectedFilesystemFailure{
        .domain = ErrorDomainId{"test.content_browser"},
        .code = ErrorCode{"injected"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Injected Content Browser filesystem failure.",
    };

    inline void AppendU32(std::vector<std::uint8_t> &bytes, const std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            bytes.push_back(static_cast<std::uint8_t>(value >> shift & 0xffU));
    }

    inline void AppendFloat(std::vector<std::uint8_t> &bytes, const float value) {
        std::uint32_t raw{};
        std::memcpy(&raw, &value, sizeof(raw));
        AppendU32(bytes, raw);
    }

    inline void WriteTestMeshPayload(const std::filesystem::path &path) {
        std::vector<std::uint8_t> bytes;
        AppendU32(bytes, Assets::MeshEditorPayloadSchemaVersion);
        AppendU32(bytes, 3);
        AppendU32(bytes, 1);
        for (const float value : {-0.5F, 0.0F, -0.5F, 0.5F, 1.0F, 0.5F})
            AppendFloat(bytes, value);
        AppendU32(bytes, 36);
        AppendU32(bytes, 0);
        AppendU32(bytes, 0);
        for (const float value : {-0.5F, 0.0F, 0.0F, 0.5F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F})
            AppendFloat(bytes, value);
        AppendU32(bytes, 3);
        AppendU32(bytes, 0);
        AppendU32(bytes, 1);
        AppendU32(bytes, 2);
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    class SyncFailingFilesystem final : public DurableFileSystem {
    public:
        [[nodiscard]] Result<ExclusiveFileLock> TryAcquireExclusive(const std::filesystem::path &path,
                                                                    const std::string_view ownerMetadata) override {
            return native_.TryAcquireExclusive(path, ownerMetadata);
        }

        [[nodiscard]] Result<std::uint64_t> AvailableBytes(const std::filesystem::path &path) const override {
            return native_.AvailableBytes(path);
        }

        [[nodiscard]] Result<void> WriteDurable(const std::filesystem::path &path, const std::span<const std::byte> bytes) override {
            return native_.WriteDurable(path, bytes);
        }

        [[nodiscard]] Result<void> CopyDurable(const std::filesystem::path &source, const std::filesystem::path &destination) override {
            return native_.CopyDurable(source, destination);
        }

        [[nodiscard]] Result<void> AtomicReplace(const std::filesystem::path &prepared, const std::filesystem::path &destination) override {
            if (failReplace)
                return Result<void>::Failure(MakeError(InjectedFilesystemFailure));
            return native_.AtomicReplace(prepared, destination);
        }

        [[nodiscard]] Result<void> RemoveDurable(const std::filesystem::path &path) override {
            return native_.RemoveDurable(path);
        }

        [[nodiscard]] Result<void> SyncDirectory(const std::filesystem::path &path) override {
            if (failSync)
                return Result<void>::Failure(MakeError(InjectedFilesystemFailure));
            return native_.SyncDirectory(path);
        }

        bool failSync{false};
        bool failReplace{false};

    private:
        NativeDurableFileSystem native_;
    };

    class FocusedWorkspaceController final {
    public:
        explicit FocusedWorkspaceController(const std::filesystem::path &projectRoot, DiagnosticSourceNavigator diagnosticNavigator = {},
                                            SourceOpenNavigator sourceOpenNavigator = {})
            : controller_(projectRoot, runtimeScene_, {},
                          EditorWorkspaceDependencies{.sourceOpenNavigator = std::move(sourceOpenNavigator),
                                                      .diagnosticSourceNavigator = std::move(diagnosticNavigator)}) {
            REQUIRE((runtimeScene_.Startup(cancellation_.Token()).HasValue()));
            PumpLifecycleCommit();
        }

        void ProcessCommand(const EditorWorkspaceViewCommandData &command) {
            controller_.ProcessCommand(command);
            PumpLifecycleCommit();
        }

        [[nodiscard]] const EditorWorkspaceViewModel &ViewModel() const noexcept {
            return controller_.ViewModel();
        }

        void RefreshAssets(const Assets::AssetRegistrySnapshot &snapshot) {
            controller_.RefreshAssets(snapshot);
        }

    private:
        void PumpLifecycleCommit() {
            const Runtime::FrameContext context{1, {}, 0.0, 0, {}, false, cancellation_.Token()};
            REQUIRE((runtimeScene_.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, context).HasValue()));
            controller_.SynchronizeRuntimeScenePreview();
        }

        Runtime::RuntimeSceneService runtimeScene_;
        CancellationSource cancellation_;
        EditorWorkspaceController controller_;
    };
}  // namespace HoroEditorWorkspaceControllerPolicyTests
