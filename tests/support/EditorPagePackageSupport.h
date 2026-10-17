#pragma once
#include "OwnedTestDirectory.h"

#include <filesystem>
#include <fstream>

namespace Horo::Tests {
    /** @brief Real native C11 package with settings/root/child declarations; no synthetic provider session replaces the loader. */
    struct EditorPagePackage final {
        OwnedTestDirectory directory{"horo108 native page package"};
        const std::filesystem::path root = directory.Path();

        EditorPagePackage() {
            const std::filesystem::path library{HORO_EDITOR_PAGE_FIXTURE};
            std::filesystem::copy_file(library, root / library.filename());
            std::ofstream manifest{root / "extension.json"};
            manifest
                << R"({"id":"fixture.package","version":"1.0.0","modules":[{"id":"fixture.module","version":"1.0.0","kind":"native","roles":["editor-presentation"],"entry":")"
                << library.filename().string()
                << R"(","abi":{"major":1,"minimumMinor":5},"requiredCapabilities":["editor.page"]}],"contributions":[{"type":"editor.settings_page","id":"fixture.settings","module":"fixture.module"},{"type":"editor.modal","id":"fixture.modal","module":"fixture.module"},{"type":"editor.modal_page","id":"fixture.child","module":"fixture.module"}]})";
        }
    };
}  // namespace Horo::Tests
