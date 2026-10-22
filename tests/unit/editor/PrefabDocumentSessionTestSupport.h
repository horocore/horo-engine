#pragma once

#include "PrefabHierarchyTestSupport.h"
#include "editor/screens/workspace/prefab/PrefabDocumentSession.h"

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace Horo::Editor::PrefabDocumentSessionTestSupport {
    using namespace PrefabHierarchyTestSupport;

    /** @brief Owns real canonical files, shared identity registry and one source authority. */
    struct Fixture final {
        Project project;
        DocumentIdentityRegistry identities;
        Prefab::PrefabLimitProfile limits{Prefab::PrefabLimitProfile::Create({}).Value()};
        std::unique_ptr<PrefabDocumentSession> document;

        Fixture() {
            // Native publication leases require canonical parent spellings, including Windows short-name aliases.
            std::error_code error;
            project.directory.path = std::filesystem::canonical(project.directory.path, error);
            REQUIRE_FALSE(error);
            project.source = std::filesystem::canonical(project.source, error);
            REQUIRE_FALSE(error);
            auto key = DocumentOpenKey{DocumentKind::Prefab, SourceDocumentId::Parse(project.record.sourcePath.String()).Value()};
            auto opened = identities.Open(key);
            REQUIRE(opened.HasValue());
            auto session = PrefabDocumentSession::Open(project.directory.path, project.source, project.asset, opened.Value().identity,
                                                       PrefabAsset(9).Asset(), limits);
            REQUIRE(session.HasValue());
            document = std::move(session).Value();
        }

        void Rename(const std::string &name) {
            REQUIRE(document->Execute(document->Snapshot().fence, RenamePrefabObject{{0}, name}).HasValue());
        }

        void Autosave() {
            const auto saved = document->Autosave(project.mutations, project.files);
            INFO("autosave code: " << (saved.HasError() ? saved.ErrorValue().code.Value() : std::string_view{}));
            INFO("autosave detail: " << (saved.HasError() ? saved.ErrorValue().message : std::string{}));
            REQUIRE(saved.HasValue());
        }

        auto Capture() {
            const std::array roots{project.asset};
            auto captured = project.Capture(project.registry.Snapshot(), roots, limits, {});
            REQUIRE(captured.HasValue());
            return std::move(captured).Value();
        }
    };
}  // namespace Horo::Editor::PrefabDocumentSessionTestSupport
