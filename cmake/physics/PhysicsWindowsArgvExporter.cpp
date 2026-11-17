/** @file PhysicsWindowsArgvExporter.cpp @brief Same-toolchain MSVC CRT argv observation; never invokes a compiler. */
#include <cstdio>

namespace {
    /** @brief Emits exact UTF-16 code units as ASCII JSON escapes, including surrogate pairs and empty arguments.
     * @param value One non-null CRT-owned terminated argument. */
    void ExportArgument(const wchar_t *value) {
        std::fputc('"', stdout);
        for (; *value != L'\0'; ++value)
            std::fprintf(stdout, "\\u%04x", static_cast<unsigned>(*value));
        std::fputc('"', stdout);
    }
}  // namespace

/** @brief Observes authentic configured CRT startup arguments without interpreting or executing their contents.
 * @param argc Actual CRT argument count. @param argv Actual CRT-parsed UTF-16 arguments, including the original compiler argv0.
 * @return Zero only when the complete bounded JSON argument array was written successfully.
 */
int wmain(int argc, wchar_t **argv) {
    if (argc < 1 || argc > 32'767)
        return 2;
    std::fputc('[', stdout);
    for (int index = 0; index < argc; ++index) {
        if (index != 0)
            std::fputc(',', stdout);
        ExportArgument(argv[index]);
    }
    std::fputs("]\n", stdout);
    return std::fflush(stdout) == 0 && std::ferror(stdout) == 0 ? 0 : 3;
}
