"""Preserve literal include probing, search order and native filesystem failures."""
from pathlib import Path
import sys
from unittest.mock import patch

import pytest

from test_quality_preflight import AnalysisError, sonar


def test_header_context_does_not_canonicalize_unresolved_search_paths(tmp_path: Path):
    source = tmp_path / "source.cpp"
    source.write_text('#include <nested/header.h>\n', encoding="utf-8")
    missing = tmp_path / "missing search \u03a9"
    not_directory = tmp_path / "regular-file"
    not_directory.write_text("not an include directory", encoding="utf-8")
    available = tmp_path / "available search \u03a9"
    header = available / "nested/header.h"
    header.parent.mkdir(parents=True)
    header.write_text("int value;\n", encoding="utf-8")
    entry = {"source": source, "includes": [missing, not_directory, available], "target": "source.o"}
    with patch.object(Path, "resolve", autospec=True, side_effect=Path.resolve) as resolves:
        assert sonar.header_context([entry], [header], tmp_path) == {header: entry}
        resolved = [call.args[0] for call in resolves.call_args_list]
    assert header in resolved
    for directory in (tmp_path, missing, not_directory):
        assert directory / "nested/header.h" not in resolved


def test_header_context_keeps_first_existing_include_even_when_later_copy_is_selected(tmp_path: Path):
    source = tmp_path / "source.cpp"
    source.write_text('#include <header.h>\n', encoding="utf-8")
    first, second = [tmp_path / name / "header.h" for name in ("first", "second")]
    for header in (first, second):
        header.parent.mkdir()
        header.write_text("int value;\n", encoding="utf-8")
    entry = {"source": source, "includes": [first.parent, second.parent], "target": "source.o"}
    with pytest.raises(AnalysisError, match="No translation unit"):
        sonar.header_context([entry], [second], tmp_path)
    first.unlink()
    assert sonar.header_context([entry], [second], tmp_path) == {second: entry}


@pytest.mark.parametrize("error", [PermissionError("include probe denied"), OSError("include probe denied")])
def test_header_context_does_not_hide_include_search_errors(tmp_path: Path, error: OSError):
    source = tmp_path / "source.cpp"
    source.write_text('#include <header.h>\n', encoding="utf-8")
    inaccessible = tmp_path / "inaccessible" / "header.h"
    header = tmp_path / "available" / "header.h"
    header.parent.mkdir()
    header.write_text("int value;\n", encoding="utf-8")
    entry = {"source": source, "includes": [inaccessible.parent, header.parent], "target": "source.o"}
    original = Path.lstat

    def probe(path, *args, **kwargs):
        if path == inaccessible:
            raise error
        return original(path, *args, **kwargs)

    with patch.object(Path, "lstat", autospec=True, side_effect=probe):
        with pytest.raises(type(error), match="include probe denied"):
            sonar.header_context([entry], [header], tmp_path)
        result = sonar.guarded(sonar.header_context, [entry], [header], tmp_path)
    assert result["status"] == "incomplete"
    assert "include probe denied" in result["error"]


def test_header_context_resolves_existing_symlinks_and_rejects_loops(tmp_path: Path):
    source = tmp_path / "source.cpp"
    source.write_text('#include "alias.h"\n', encoding="utf-8")
    header = tmp_path / "selected.h"
    header.write_text("int value;\n", encoding="utf-8")
    alias = tmp_path / "alias.h"
    try:
        alias.symlink_to(header)
    except OSError as error:
        if sys.platform == "win32" and getattr(error, "winerror", None) == 1314:
            pytest.skip("Windows account lacks native symlink creation privilege")
        raise
    entry = {"source": source, "includes": [tmp_path], "target": "source.o"}
    assert sonar.header_context([entry], [header], tmp_path) == {header: entry}
    alias.unlink()
    alias.symlink_to(alias)
    with pytest.raises((RuntimeError, OSError)):
        sonar.header_context([entry], [header], tmp_path)
