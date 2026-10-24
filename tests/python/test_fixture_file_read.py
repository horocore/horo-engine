"""Compile the real fixture reader and verify complete, bounded binary reads."""

from pathlib import Path
import os
import shutil
# Only a resolved compiler and the fixed generated driver run, both without a shell.
import subprocess  # nosec B404

import pytest


ROOT = Path(__file__).resolve().parents[2]
DRIVER = r'''
#include "FixtureFileRead.h"
#include <iostream>

template <typename Char> int Run(int argc, Char **argv) {
    try {
        const std::filesystem::path path{argv[1]};
        const auto bytes = argc == 2 ? Horo::Tests::ReadFixtureBytes(path)
            : Horo::Tests::ReadFixtureBytes(path, std::stoull(std::filesystem::path{argv[2]}.string()));
        constexpr char hex[] = "0123456789abcdef";
        for (const unsigned char byte : bytes) {
            std::cout.put(hex[byte >> 4]);
            std::cout.put(hex[byte & 15]);
        }
        return 0;
    } catch (const std::filesystem::filesystem_error &error) {
        std::cerr << (error.code() == std::errc::file_too_large ? "too-large" : "io-error");
        return 3;
    }
}
#ifdef _WIN32
int wmain(int argc, wchar_t **argv) { return Run(argc, argv); }
#else
int main(int argc, char **argv) { return Run(argc, argv); }
#endif
'''


@pytest.fixture(scope="module")
def reader(tmp_path_factory):
    compiler = shutil.which("cl" if os.name == "nt" else "c++")
    if compiler is None:
        pytest.skip("Fixture reader qualification requires a C++20 compiler")
    compiler = str(Path(compiler).resolve(strict=True))
    directory = tmp_path_factory.mktemp("fixture reader ü")
    source = directory / "reader.cpp"
    source.write_text(DRIVER, encoding="utf-8")
    binary = directory / ("reader.exe" if os.name == "nt" else "reader")
    include = ROOT / "tests/support"
    # executable selects the absolute binary; argv[0] only selects its driver name.
    # Source, flags and paths are argv elements and never form a shell command.
    if os.name == "nt":
        subprocess.run(  # nosec B607
            ["cl", "/nologo", "/std:c++20", "/EHsc", f"/I{include}", str(source), f"/Fe:{binary}"],
            executable=compiler, cwd=directory, capture_output=True, text=True, timeout=60,
            check=True, shell=False,  # nosec B603
        )
    else:
        subprocess.run(  # nosec B607
            ["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-I", str(include), str(source), "-o", str(binary)],
            executable=compiler, cwd=directory, capture_output=True, text=True, timeout=60,
            check=True, shell=False,  # nosec B603
        )
    return binary


def read(reader, path, *bounds):
    # Execute only the driver compiled by this module; paths remain argv elements.
    return subprocess.run(  # nosec B607
        ["reader", str(path), *map(str, bounds)], executable=str(reader.resolve(strict=True)),
        capture_output=True, timeout=30, check=False, shell=False,  # nosec B603
    )


@pytest.mark.parametrize("payload", [b"", b"\0\r\n\xff", bytes(range(256)) * 4096],
                         ids=["empty", "binary", "large"])
def test_complete_binary_read_preserves_every_byte(reader, tmp_path, payload):
    path = tmp_path / "fixture ü with spaces.bin"
    path.write_bytes(payload)
    result = read(reader, path)
    assert result.returncode == 0, result.stderr
    assert bytes.fromhex(result.stdout.decode("ascii")) == payload


@pytest.mark.parametrize("bound,expected", [(0, 3), (3, 3), (4, 0)])
def test_size_bound_is_exact_and_rejects_before_reading(reader, tmp_path, bound, expected):
    path = tmp_path / "bounded.bin"
    path.write_bytes(b"data")
    result = read(reader, path, bound)
    assert result.returncode == expected
    if expected:
        assert result.stderr == b"too-large"


def test_zero_bound_accepts_an_empty_fixture(reader, tmp_path):
    path = tmp_path / "empty.bin"
    path.write_bytes(b"")
    result = read(reader, path, 0)
    assert result.returncode == 0, result.stderr
    assert result.stdout == b""


def test_default_bound_rejects_an_oversized_sparse_fixture(reader, tmp_path):
    path = tmp_path / "oversized.bin"
    with path.open("wb") as stream:
        stream.truncate(256 * 1024 * 1024 + 1)
    result = read(reader, path)
    assert result.returncode == 3
    assert result.stderr == b"too-large"


@pytest.mark.parametrize("directory", [False, True], ids=["missing", "directory"])
def test_invalid_input_is_an_error_instead_of_empty_contents(reader, tmp_path, directory):
    path = tmp_path / "unreadable ü"
    if directory:
        path.mkdir()
    result = read(reader, path)
    assert result.returncode == 3
    assert result.stderr == b"io-error"
