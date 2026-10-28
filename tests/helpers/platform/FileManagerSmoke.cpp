#include "Horo/Platform/FileManager.h"

#include <filesystem>
#include <iostream>

namespace {
    /** @brief Explicit manual desktop check; never registered as an unattended test. */
    int Reveal(const std::filesystem::path &path) {
        const auto result = Horo::RevealInFileManager(path);
        if (result.HasError()) {
            std::cerr << result.ErrorValue().code.Value() << ": " << result.ErrorValue().message << '\n';
            return 1;
        }
        std::cout << "File Manager accepted the request.\n";
        return 0;
    }
}  // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t **argv) {
#else
int main(int argc, char **argv) {
#endif
    if (argc != 2) {
        std::cerr << "Usage: HoroFileManagerSmoke <existing-absolute-path>\n";
        return 2;
    }
    return Reveal(std::filesystem::path{argv[1]});
}
