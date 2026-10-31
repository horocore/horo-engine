#pragma once
#include "Horo/Editor/PrefabProjectSettingsSession.h"
#include "Horo/Prefab/PrefabErrors.h"
#include "NativePublicationFiles.h"
#include "OwnedTestDirectory.h"

#include <fstream>
#include <stdexcept>

namespace Horo::Tests::PrefabSettingsSupport {
    class Files final : public TestSupport::NativePublicationFiles {
    public:
        enum class Fault {
            None,
            BeforeReplace,
            AfterReplace,
            LockException
        } fault{};
        std::size_t replacements{};

        Result<ExclusiveFileLock> TryAcquireExclusive(const std::filesystem::path &path, const std::string_view owner) override {
            if (fault == Fault::LockException && owner == "horo.prefab.settings")
                throw std::runtime_error("injected lock callback failure");
            return native.TryAcquireExclusive(path, owner);
        }

        Result<void> WritePrivateDurable(const std::filesystem::path &path, std::span<const std::byte> bytes, bool &created,
                                         std::filesystem::perms mode) override {
            return native.WritePrivateDurable(path, bytes, created, mode);
        }

        Result<void> AtomicReplaceTracked(const std::filesystem::path &a, const std::filesystem::path &b,
                                          AtomicFileReplacementReceipt &receipt) override {
            if (fault == Fault::BeforeReplace)
                return Result<void>::Failure(MakeError(Prefab::PrefabErrors::AdmissionRejected));
            auto result = native.AtomicReplaceTracked(a, b, receipt);
            if (result.HasError())
                return result;
            ++replacements;
            if (fault == Fault::AfterReplace)
                return Result<void>::Failure(MakeError(Prefab::PrefabErrors::AdmissionRejected));
            return result;
        }
    };

    struct Fixture final {
        OwnedTestDirectory directory{"prefab-settings"};
        std::filesystem::path root{directory.Path() / std::filesystem::path{u8"çalışma alanı"}};
        Files files;
        Editor::ProjectMutationCoordinator mutations{files};
        std::shared_ptr<Editor::PrefabProjectSettingsSession> owner;

        Fixture() {
            REQUIRE(std::filesystem::create_directories(root / ".horo"));
            auto opened = Editor::PrefabProjectSettingsSession::Open(root, files, mutations);
            REQUIRE(opened.HasValue());
            owner = opened.Value();
        }

        Result<bool> Command(Editor::PrefabSettingsCommandKind kind,
                             Application::PrefabLimitField field = Application::PrefabLimitField::ObjectCount, std::int64_t value = 0) {
            return owner->Execute({kind, owner->Snapshot().Value()->revision, field, value});
        }

        std::string Bytes() const {
            std::ifstream file(root / ".horo/prefab.json", std::ios::binary);
            return {std::istreambuf_iterator<char>{file}, {}};
        }

        void Write(std::string_view bytes) {
            std::ofstream(root / ".horo/prefab.json", std::ios::binary) << bytes;
        }
    };
}  // namespace Horo::Tests::PrefabSettingsSupport
