"""Validate the reviewed lexer patch against populated, pinned dependency bytes."""
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]
PATCH = ROOT / "cmake/patches/HoroJsonLexerAllocation.cmake"
RELATIVE = Path("include/nlohmann/detail/input/lexer.hpp")
REPLACEMENTS = (
    ("explicit lexer(InputAdapterType&& adapter, bool ignore_comments_ = false) noexcept",
     "explicit lexer(InputAdapterType&& adapter, bool ignore_comments_ = false)"),
    ("std::vector<char_type> token_string {};",
     "std::vector<char_type> token_string = std::vector<char_type>(std::size_t{0});"),
    ("string_t token_buffer {};",
     "string_t token_buffer = string_t(std::size_t{0}, typename string_t::value_type{});"),
)
FILES = {
    "lexer.hpp": REPLACEMENTS,
    "json_sax.hpp": (
        ("std::vector<BasicJsonType*> ref_stack {};",
         "std::vector<BasicJsonType*> ref_stack = std::vector<BasicJsonType*>(std::size_t{0});"),
        ("std::vector<bool> keep_stack {};",
         "std::vector<bool> keep_stack = std::vector<bool>(std::size_t{0});"),
        ("std::vector<bool> key_keep_stack {};",
         "std::vector<bool> key_keep_stack = std::vector<bool>(std::size_t{0});"),
    ),
    "parser.hpp": (("std::vector<bool> states;", "std::vector<bool> states(std::size_t{0});"),),
    "json.hpp": (
        ("create<array_t>()", "create<array_t>(std::size_t{0}, basic_json{})"),
        ("if (t == value_t::array || t == value_t::object)",
         "if ((t == value_t::array && !array->empty()) || (t == value_t::object && !object->empty()))"),
    ),
}


def paths(source):
    return {name: source / (Path("include/nlohmann") if name == "json.hpp" else RELATIVE.parent) / name
            for name in FILES}


@pytest.fixture
def source(tmp_path):
    candidates = [ROOT / ".cmake/fetchcontent/nlohmann_json-src"]
    candidates.extend((ROOT / "build").glob("*/_deps/nlohmann_json-src"))
    upstream = next((p for p in candidates if (p / RELATIVE).is_file()), None)
    if upstream is None:
        pytest.skip("Pinned JSON dependency must be populated before this test")
    for name, target in paths(tmp_path).items():
        text = paths(upstream)[name].read_text(encoding="utf-8")
        # The patched constructor spelling is also a prefix of the original.
        if FILES[name][0][0] not in text:
            for original, patched in FILES[name]:
                assert text.count(patched) > 0
                text = text.replace(patched, original)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8", newline="\n")
    return tmp_path


def apply(source):
    cmake = shutil.which("cmake")
    assert cmake is not None
    return subprocess.run(
        [cmake, f"-DHORO_JSON_SOURCE_DIR={source}", "-P", str(PATCH)],
        capture_output=True, text=True, check=False,
    )


@pytest.mark.parametrize("crlf", [False, True])
def test_patch_preserves_all_other_bytes_and_is_idempotent(source, crlf):
    expected = {}
    for name, path in paths(source).items():
        original = path.read_text(encoding="utf-8")
        expected[name] = original
        for before, after in FILES[name]:
            assert expected[name].count(before) > 0
            expected[name] = expected[name].replace(before, after)
        if crlf:
            path.write_bytes(original.encode().replace(b"\n", b"\r\n"))
    result = apply(source)
    assert result.returncode == 0, result.stderr
    assert {name: path.read_text(encoding="utf-8") for name, path in paths(source).items()} == expected
    first = {name: path.read_bytes() for name, path in paths(source).items()}
    assert apply(source).returncode == 0
    assert {name: path.read_bytes() for name, path in paths(source).items()} == first


@pytest.mark.parametrize("name", FILES)
def test_patch_rejects_drift_without_modifying_any_source(source, name):
    path = paths(source)[name]
    path.write_bytes(path.read_bytes() + b"\n// unreviewed drift\n")
    before = {name: path.read_bytes() for name, path in paths(source).items()}
    result = apply(source)
    assert result.returncode != 0
    assert "source drift" in result.stderr
    assert {name: path.read_bytes() for name, path in paths(source).items()} == before
