"""Inspect real 64-bit Android ELF inputs before packaging, without executing target code."""

from __future__ import annotations

from pathlib import Path
import hashlib
import struct

from android_contract import AndroidError, bounded_bytes


def unpack(data: bytes, format_string: str, offset: int) -> tuple:
    size = struct.calcsize(format_string)
    if offset < 0 or offset + size > len(data):
        raise AndroidError("Truncated ELF metadata; rebuild the native dependency.")
    return struct.unpack_from(format_string, data, offset)


def programs(data: bytes) -> list[tuple]:
    header = unpack(data, "<HHIQQQIHHHHHH", 16)
    offset, entry_size, count = header[4], header[8], header[9]
    if entry_size != 56 or count > 1024:
        raise AndroidError("Unsupported ELF program table; rebuild with the declared NDK.")
    return [unpack(data, "<IIQQQQQQ", offset + index * entry_size) for index in range(count)]


def dynamic_entries(data: bytes, headers: list[tuple]) -> list[tuple]:
    dynamic = next((header for header in headers if header[0] == 2), None)
    if dynamic is None or dynamic[5] > 65536 or dynamic[5] % 16:
        raise AndroidError("Missing or invalid ELF dynamic metadata.")
    result = []
    for offset in range(dynamic[2], dynamic[2] + dynamic[5], 16):
        entry = unpack(data, "<qQ", offset)
        if entry[0] == 0:
            return result
        result.append(entry)
    raise AndroidError("Unterminated ELF dynamic metadata.")


def string_table(data: bytes, headers: list[tuple], entries: list[tuple]) -> bytes:
    strings = next((value for tag, value in entries if tag == 5), None)
    size = next((value for tag, value in entries if tag == 10), 0)
    if strings is None or size > 1024 * 1024:
        raise AndroidError("Invalid ELF string table.")
    for segment in headers:
        if segment[0] != 1:
            continue
        if segment[3] <= strings < segment[3] + segment[5]:
            return segment_strings(data, segment, strings, size)
    raise AndroidError("ELF string table has no mapped load segment.")


def segment_strings(data: bytes, segment: tuple, strings: int, size: int) -> bytes:
    offset = segment[2] + strings - segment[3]
    if offset + size > len(data) or strings + size > segment[3] + segment[5]:
        raise AndroidError("Truncated ELF string table.")
    return data[offset:offset + size]


def needed_names(data: bytes, headers: list[tuple]) -> list[str]:
    entries = dynamic_entries(data, headers)
    if any(tag in (15, 29) for tag, _ in entries):
        raise AndroidError("Native RPATH/RUNPATH is forbidden; package an explicit Android dependency closure.")
    table = string_table(data, headers, entries)
    return [library_name(table, value) for tag, value in entries if tag == 1]


def android_api(data: bytes, headers: list[tuple]) -> int:
    for header in headers:
        if header[0] == 4:
            api = note_api(data, header[2], header[5])
            if api is not None:
                return api
    raise AndroidError("Native dependency has no Android API provenance note; rebuild with the admitted NDK.")


def note_api(data: bytes, offset: int, size: int) -> int | None:
    end = offset + size
    if end > len(data):
        raise AndroidError("Truncated ELF note metadata.")
    while offset + 12 <= end:
        name_size, description_size, kind = unpack(data, "<III", offset)
        name_offset = offset + 12
        description_offset = name_offset + ((name_size + 3) & ~3)
        next_offset = description_offset + ((description_size + 3) & ~3)
        if next_offset > end:
            raise AndroidError("Truncated ELF note payload.")
        if kind == 1 and data[name_offset:name_offset + name_size].rstrip(b"\0") == b"Android":
            if description_size < 4:
                raise AndroidError("Invalid Android API note.")
            return unpack(data, "<I", description_offset)[0]
        offset = next_offset
    return None


def library_name(table: bytes, offset: int) -> str:
    end = table.find(b"\0", offset)
    if offset >= len(table) or end < 0:
        raise AndroidError("Invalid native dependency name.")
    try:
        name = table[offset:end].decode("ascii")
    except UnicodeDecodeError as error:
        raise AndroidError("Nonportable native dependency name.") from error
    if not name.startswith("lib") or not name.endswith(".so") or "/" in name or "\\" in name:
        raise AndroidError("Native dependency names must be ordinary library basenames.")
    return name


def validate_load_alignment(headers: list[tuple]) -> None:
    loads = [header for header in headers if header[0] == 1]
    if not loads or any(header[7] < 16384 for header in loads):
        raise AndroidError("Native dependency lacks 16 KiB load alignment; rebuild with the pinned Android linker policy.")


def inspect_elf(path: Path, machine: int, minimum_api: int = 29) -> dict:
    data = bounded_bytes(path)
    if len(data) < 64 or data[:7] != b"\x7fELF\x02\x01\x01":
        raise AndroidError("Native input is not a little-endian ELF64 artifact.")
    elf_type, actual_machine = unpack(data, "<HH", 16)
    if elf_type != 3 or actual_machine != machine:
        raise AndroidError("Native dependency ABI is incompatible; rebuild each declared ABI independently.")
    headers = programs(data)
    validate_load_alignment(headers)
    api = android_api(data, headers)
    if not 21 <= api <= minimum_api:
        raise AndroidError("Native dependency requires a newer API than minSdk; rebuild for the profile API floor.")
    return {"name": path.name, "sha256": hashlib.sha256(data).hexdigest(), "size": len(data),
            "minimumApi": api, "needed": sorted(needed_names(data, headers))}


def native_closure(directory: Path, abi: str, tools: dict, entry_library: str) -> list[dict]:
    if directory.is_symlink():
        raise AndroidError("Native ABI input directories cannot be links.")
    libraries = sorted(directory.glob("*.so"))
    if not libraries or len(libraries) > 256:
        raise AndroidError("Missing or oversized native library set; provide the full declared ABI closure.")
    names = {path.name for path in libraries}
    if f"lib{entry_library}.so" not in names or "libc++_shared.so" not in names:
        raise AndroidError("Package needs its entry library and exactly one pinned shared C++ runtime per ABI.")
    records = [inspect_elf(path, tools["abis"][abi], tools["minimumApi"]) for path in libraries]
    validate_dependency_closure(records, names | set(tools["systemLibraries"]))
    return records


def validate_dependency_closure(records: list[dict], allowed: set[str]) -> None:
    missing = sorted({name for record in records for name in record["needed"] if name not in allowed})
    if missing:
        raise AndroidError(f"Missing native dependencies: {', '.join(missing)}; supply the complete ABI closure.")
