"""Native same-toolchain CRT oracle selfcheck, required by the Windows Physics facts build.

Microsoft command-line rules and examples:
https://learn.microsoft.com/en-us/cpp/c-language/parsing-c-command-line-arguments
This executes only the inert exporter, never the compiler named in argv[0].
"""
import argparse
import os
from pathlib import Path

from collect_physics_state_build_facts import windows_arguments
from generate_physics_state_build_facts import require


def check(exporter):
    require(os.name == 'nt', 'Native Windows CRT selfcheck requires Windows')
    # The intentionally nonexistent compiler also proves executable replacement.
    compiler = r'C:\nonexistent compiler\not-executed.exe'
    vectors = [
        (r'"a b c" d e', ['a b c', 'd', 'e']),
        (r'"ab\"c" "\\" d', ['ab"c', '\\', 'd']),
        (r'a\\\b d"e f"g h', [r'a\\\b', 'de fg', 'h']),
        (r'a\\\"b c d', ['a\\"b', 'c', 'd']),
        (r'a\\\\"b c" d e', [r'a\\b c', 'd', 'e']),
        (r'a"b"" c d', ['ab" c d']),
        ('"" "quoted  空間 🧪"', ['', 'quoted  空間 🧪']),
    ]
    for tail, expected in vectors:
        command = '"' + compiler + '" ' + tail
        actual = windows_arguments(command, exporter, str(Path(exporter).parent))
        require(actual == [compiler, *expected], 'Configured MSVC CRT argv selfcheck mismatch: ' + repr(tail))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exporter', type=Path, required=True)
    check(parser.parse_args().exporter)


if __name__ == '__main__':
    main()
