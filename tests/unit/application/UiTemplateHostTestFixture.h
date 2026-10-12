#pragma once

#include "Horo/Application/UiTemplateHost.h"
#include "Horo/UiTemplates/UiTemplateSource.h"
#include "../packages/PackageArchiveTestSupport.h"
#include "../ui_templates/UiTemplateOwnerTestFixture.h"

#include <fstream>

namespace Horo::Application::TemplateHostTests {
    /** @brief Real source/sidecar capture and exact semantic source revision for headless host qualification. */
    struct Fixture final {
        Horo::Tests::Packages::TemporaryDirectory temporary{"horo-template-host"};
        NativeDurableFileSystem files;
        Editor::ProjectMutationCoordinator mutations{files};
        Assets::AssetRegistry registry;
        Assets::AssetTypeId type{Assets::AssetTypeId::Parse("core.ui_template").Value()};
        std::shared_ptr<const UiTemplates::UiTemplateCatalog> catalog;
        std::unique_ptr<UiTemplates::UiTemplateDocumentOwner> owner;
        std::string sourceBytes;
        UiTemplates::UiTemplateReference reference{UiTemplates::TestSupport::Reference(1)};

        explicit Fixture(const UiTemplates::UiTemplateLimits &limits = {}) {
            using namespace UiTemplates::TestSupport;
            auto source = Source(1);
            const auto provisional = Catalog(source, limits);
            const auto encoded = UiTemplates::SerializeTemplateSource(*provisional, reference);
            REQUIRE(encoded.HasValue());
            sourceBytes = encoded.Value();
            reference.revision = ComputeSha256(std::as_bytes(std::span{sourceBytes}));
            source.dependency.revision = reference.revision;
            catalog = Catalog(std::move(source), limits);
            REQUIRE(UiTemplates::SerializeTemplateSource(*catalog, reference).Value() == sourceBytes);
            auto document = Document();
            document.instances.front().accepted = reference;
            auto created = UiTemplates::UiTemplateDocumentOwner::Create(std::move(document), catalog);
            REQUIRE(created.HasValue());
            owner = std::move(created).Value();
            WriteSource(sourceBytes);
            Write("assets/template.uitmpl.horo", nlohmann::json{{"schemaVersion", 1}, {"assetId", reference.asset.asset.ToString()},
                                                               {"assetType", type.Value()}}.dump());
            REQUIRE(registry.Publish({{reference.asset.asset, type, ProjectPath::Parse("assets/template.uitmpl").Value(),
                                       ProjectPath::Parse("assets/template.uitmpl.horo").Value()}}).status ==
                    Assets::AssetRegistryBuildStatus::Complete);
        }

        void Write(const std::string_view relative, const std::string_view bytes) const {
            const auto path = temporary.Path() / relative;
            std::filesystem::create_directories(path.parent_path());
            std::ofstream stream{path, std::ios::binary | std::ios::trunc};
            stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            stream.close();
            REQUIRE(stream.good());
        }

        void WriteSource(const std::string_view bytes) const {
            Write("assets/template.uitmpl", bytes);
        }

        UiTemplateInputLease Capture() const {
            auto captured = UiTemplateInputLease::Capture(temporary.Path(), registry.Snapshot(), mutations, nullptr);
            REQUIRE(captured.HasValue());
            return std::move(captured).Value();
        }
    };
}
