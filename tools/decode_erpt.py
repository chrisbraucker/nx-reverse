#!/usr/bin/env python3

import argparse
import hashlib
import json
import pathlib
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass
from typing import Any


SUMMARY_FIELDS = (
    "ErrorCode",
    "InvalidErrorCode",
    "ServerErrorCode",
    "NifmErrorCode",
    "ProgramId",
    "ApplicationID",
    "ApplicationTitle",
    "RunningApplicationTitle",
    "ThreadName",
    "AbortFlag",
    "FatalFlag",
    "CrashReportFlag",
    "SystemAbortFlag",
    "ApplicationAbortFlag",
    "AbortType",
    "CreateProcessFailureFlag",
    "PscTransitionCurrentState",
    "PscTransitionPreviousState",
)


class DecodeError(ValueError):
    pass


@dataclass
class BinaryValue:
    data: bytes

    def summary(self) -> dict[str, Any]:
        return {
            "type": "bin",
            "size": len(self.data),
            "sha256": hashlib.sha256(self.data).hexdigest(),
            "preview": self.data[:32].hex(),
        }


class MsgpackReader:
    def __init__(self, data: bytes) -> None:
        self.data = data

    def require(self, offset: int, size: int) -> None:
        if offset + size > len(self.data):
            raise DecodeError(f"truncated msgpack at 0x{offset:x}, need {size} bytes")

    def read_u8(self, offset: int) -> tuple[int, int]:
        self.require(offset, 1)
        return self.data[offset], offset + 1

    def read_be(self, offset: int, size: int, signed: bool = False) -> tuple[int, int]:
        self.require(offset, size)
        return int.from_bytes(self.data[offset : offset + size], "big", signed=signed), offset + size

    def read_bytes(self, offset: int, size: int) -> tuple[bytes, int]:
        self.require(offset, size)
        return self.data[offset : offset + size], offset + size

    def read_str(self, offset: int, size: int) -> tuple[str, int]:
        raw, offset = self.read_bytes(offset, size)
        return raw.decode("utf-8", errors="replace"), offset

    def parse(self, offset: int = 0) -> tuple[Any, int]:
        tag, offset = self.read_u8(offset)

        if tag <= 0x7F:
            return tag, offset
        if tag >= 0xE0:
            return tag - 0x100, offset
        if 0x80 <= tag <= 0x8F:
            return self.parse_map(offset, tag & 0x0F)
        if 0x90 <= tag <= 0x9F:
            return self.parse_array(offset, tag & 0x0F)
        if 0xA0 <= tag <= 0xBF:
            return self.read_str(offset, tag & 0x1F)

        if tag == 0xC2:
            return False, offset
        if tag == 0xC3:
            return True, offset
        if tag == 0xC4:
            size, offset = self.read_be(offset, 1)
            raw, offset = self.read_bytes(offset, size)
            return BinaryValue(raw), offset
        if tag == 0xC5:
            size, offset = self.read_be(offset, 2)
            raw, offset = self.read_bytes(offset, size)
            return BinaryValue(raw), offset
        if tag == 0xCC:
            return self.read_be(offset, 1)
        if tag == 0xCD:
            return self.read_be(offset, 2)
        if tag == 0xCE:
            return self.read_be(offset, 4)
        if tag == 0xCF:
            return self.read_be(offset, 8)
        if tag == 0xD0:
            return self.read_be(offset, 1, signed=True)
        if tag == 0xD1:
            return self.read_be(offset, 2, signed=True)
        if tag == 0xD2:
            return self.read_be(offset, 4, signed=True)
        if tag == 0xD3:
            return self.read_be(offset, 8, signed=True)
        if tag == 0xD9:
            size, offset = self.read_be(offset, 1)
            return self.read_str(offset, size)
        if tag == 0xDA:
            size, offset = self.read_be(offset, 2)
            return self.read_str(offset, size)
        if tag == 0xDC:
            count, offset = self.read_be(offset, 2)
            return self.parse_array(offset, count)
        if tag == 0xDE:
            count, offset = self.read_be(offset, 2)
            return self.parse_map(offset, count)

        raise DecodeError(f"unsupported msgpack tag 0x{tag:02x} at 0x{offset - 1:x}")

    def parse_array(self, offset: int, count: int) -> tuple[list[Any], int]:
        values = []
        for _ in range(count):
            value, offset = self.parse(offset)
            values.append(value)
        return values, offset

    def parse_map(self, offset: int, count: int) -> tuple[dict[str, Any], int]:
        values: dict[str, Any] = {}
        for _ in range(count):
            key, offset = self.parse(offset)
            value, offset = self.parse(offset)
            values[str(key)] = value
        return values, offset


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Decode and summarize Atmosphere/Horizon ERPT report files"
    )
    parser.add_argument("paths", nargs="+", help="ERPT report files or directories")
    parser.add_argument("--json", action="store_true", help="Emit JSON instead of text")
    parser.add_argument("--dump", action="store_true", help="Include decoded fields for each parsed report")
    parser.add_argument("--include-errors", action="store_true", help="Include files that failed to decode")
    parser.add_argument("--max-value-len", type=int, default=160, help="Maximum scalar string length in text output")
    return parser.parse_args()


def iter_files(paths: list[str]) -> list[pathlib.Path]:
    files: list[pathlib.Path] = []
    for raw in paths:
        path = pathlib.Path(raw)
        if path.is_dir():
            files.extend(p for p in path.rglob("*") if p.is_file())
        elif path.is_file():
            files.append(path)
        else:
            print(f"warning: path not found: {path}", file=sys.stderr)
    return sorted(files)


def to_jsonable(value: Any) -> Any:
    if isinstance(value, BinaryValue):
        return value.summary()
    if isinstance(value, list):
        return [to_jsonable(v) for v in value]
    if isinstance(value, dict):
        return {str(k): to_jsonable(v) for k, v in value.items()}
    return value


def shorten(value: Any, max_len: int) -> str:
    if isinstance(value, BinaryValue):
        summary = value.summary()
        return f"<bin size={summary['size']} sha256={summary['sha256'][:16]} preview={summary['preview']}>"
    if isinstance(value, list):
        text = json.dumps(to_jsonable(value), separators=(",", ":"))
    else:
        text = str(value)
    if len(text) > max_len:
        return text[: max_len - 3] + "..."
    return text


def pick(fields: dict[str, Any], names: tuple[str, ...]) -> Any:
    for name in names:
        value = fields.get(name)
        if value not in (None, ""):
            return value
    return ""


def decode_file(path: pathlib.Path) -> dict[str, Any]:
    data = path.read_bytes()
    reader = MsgpackReader(data)
    decoded, offset = reader.parse(0)
    if not isinstance(decoded, dict):
        raise DecodeError(f"top-level object is {type(decoded).__name__}, expected map")
    if offset != len(data):
        trailing = len(data) - offset
    else:
        trailing = 0

    fields = {str(k): v for k, v in decoded.items()}
    selected = {name: to_jsonable(fields[name]) for name in SUMMARY_FIELDS if name in fields}
    identity = {
        "error_code": to_jsonable(pick(fields, ("ErrorCode", "InvalidErrorCode", "ServerErrorCode", "NifmErrorCode"))),
        "program_id": to_jsonable(pick(fields, ("ProgramId", "ApplicationID"))),
        "title": to_jsonable(pick(fields, ("ApplicationTitle", "RunningApplicationTitle"))),
        "thread": to_jsonable(pick(fields, ("ThreadName",))),
        "abort": to_jsonable(pick(fields, ("AbortFlag", "SystemAbortFlag", "ApplicationAbortFlag"))),
        "fatal": to_jsonable(pick(fields, ("FatalFlag", "CrashReportFlag"))),
        "abort_type": to_jsonable(pick(fields, ("AbortType",))),
    }
    fingerprint_basis = json.dumps(identity, sort_keys=True, separators=(",", ":"))
    return {
        "path": str(path),
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "field_count": len(fields),
        "trailing_bytes": trailing,
        "identity": identity,
        "fingerprint": hashlib.sha256(fingerprint_basis.encode("utf-8")).hexdigest(),
        "selected": selected,
        "fields": to_jsonable(fields),
    }


def print_text(results: list[dict[str, Any]], errors: list[dict[str, str]], max_value_len: int, dump: bool) -> None:
    groups: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for result in results:
        groups[result["fingerprint"]].append(result)

    print(f"parsed={len(results)} errors={len(errors)} groups={len(groups)}")
    for index, (fingerprint, items) in enumerate(sorted(groups.items(), key=lambda kv: (-len(kv[1]), kv[0])), start=1):
        first = items[0]
        ident = first["identity"]
        print(
            f"\n[{index}] count={len(items)} fp={fingerprint[:16]} "
            f"error={shorten(ident['error_code'], max_value_len)} "
            f"program={shorten(ident['program_id'], max_value_len)} "
            f"title={shorten(ident['title'], max_value_len)} "
            f"thread={shorten(ident['thread'], max_value_len)} "
            f"fatal={shorten(ident['fatal'], max_value_len)} "
            f"abort={shorten(ident['abort'], max_value_len)}"
        )
        field_counter = Counter()
        for item in items:
            field_counter.update(item["fields"].keys())
        common_fields = ",".join(name for name, _ in field_counter.most_common(16))
        print(f"    fields={common_fields}")
        for item in items[:8]:
            print(f"    file={item['path']} size={item['size']} sha256={item['sha256'][:16]}")
        if len(items) > 8:
            print(f"    ... {len(items) - 8} more")

    if dump:
        print("\nDecoded reports:")
        for item in results:
            print(f"\n== {item['path']} ==")
            for key in sorted(item["fields"]):
                print(f"{key}: {shorten(item['fields'][key], max_value_len)}")

    if errors:
        print("\nDecode errors:")
        for error in errors[:32]:
            print(f"{error['path']}: {error['error']}")
        if len(errors) > 32:
            print(f"... {len(errors) - 32} more")


def main() -> int:
    args = parse_args()
    results: list[dict[str, Any]] = []
    errors: list[dict[str, str]] = []

    for path in iter_files(args.paths):
        try:
            results.append(decode_file(path))
        except Exception as exc:
            errors.append({"path": str(path), "error": str(exc)})

    payload: dict[str, Any] = {
        "parsed_count": len(results),
        "error_count": len(errors),
        "reports": results if args.dump else [{k: v for k, v in r.items() if k != "fields"} for r in results],
    }
    if args.include_errors:
        payload["errors"] = errors

    if args.json:
        print(json.dumps(payload, indent=2, sort_keys=True))
    else:
        print_text(results, errors if args.include_errors else [], args.max_value_len, args.dump)

    return 0 if results or not errors else 1


if __name__ == "__main__":
    raise SystemExit(main())
