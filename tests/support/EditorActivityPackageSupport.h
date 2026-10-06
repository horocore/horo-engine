#pragma once
#include <filesystem>
#include <fstream>
#include <string_view>

namespace Horo::Tests {
    struct EditorActivityPackage final {
        std::filesystem::path root = std::filesystem::temp_directory_path() / "horo107 package fixture";

        EditorActivityPackage() {
            std::filesystem::remove_all(root);
            std::filesystem::create_directories(root / "icons");
            const std::filesystem::path library{HORO_EDITOR_ACTIVITY_FIXTURE};
            std::filesystem::copy_file(library, root / library.filename());
            std::ofstream manifest{root / "extension.json"};
            manifest
                << R"({"id":"fixture.package","version":"1.0.0","modules":[{"id":"fixture.module","version":"1.0.0","kind":"native","roles":["editor-presentation"],"entry":")"
                << library.filename().string()
                << R"(","abi":{"major":1,"minimumMinor":4},"requiredCapabilities":["editor.activity"]}],"contributions":[{"type":"editor.activity_item","id":"fixture.activity","module":"fixture.module"},{"type":"editor.panel","id":"fixture.drawer","module":"fixture.module"}]})";
            Icon(
                R"(<svg xmlns="http://www.w3.org/2000/svg" width="48" height="48" viewBox="0 0 48 48"><path fill="#ffffff" d="M4 4 H44 V44 H4 Z"/></svg>)");
        }

        ~EditorActivityPackage() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        void Icon(std::string_view text) const {
            std::ofstream file{root / "icons/tool.svg"};
            file << text;
        }
    };
}  // namespace Horo::Tests
