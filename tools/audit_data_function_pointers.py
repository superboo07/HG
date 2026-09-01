#!/usr/bin/env python3
"""Report function-like uncovered targets referenced by pointer-dense ELF data."""

from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from pathlib import Path

from audit_function_boundaries import (
    JR_RA, is_stack_prologue, noncall_direct_target, reachable_addresses,
)
from merge_function_config import ConfigError, load_game_config, sha256_file
from report_function_coverage import contains, executable_words, load_map, merged_ranges


def short_leaf_end(words: dict[int, int], start: int, max_instructions: int = 64) -> int | None:
    """Return the conservative bound of a small frameless ``JR ra`` function.

    Vtable methods commonly perform one or two operations before returning, so
    requiring ``JR ra`` as the first word misses real methods such as setters.
    Local branches are permitted, but every direct transfer must remain inside
    the proposed range and indirect non-return jumps are rejected.
    """
    for index in range(max_instructions):
        address = start + index * 4
        word = words.get(address)
        if word is None:
            return None
        if word == JR_RA:
            if address + 4 not in words:
                return None
            end = address + 8
            reachable = reachable_addresses(words, start, end)
            if address not in reachable:
                continue
            rejected = False
            for reachable_address in reachable:
                reachable_word = words[reachable_address]
                target = noncall_direct_target(reachable_address, reachable_word)
                if target is not None and not (start <= target < end):
                    rejected = True
                    break
                if (
                    (reachable_word >> 26) == 0
                    and (reachable_word & 0x3F) == 8
                    and reachable_word != JR_RA
                ):
                    rejected = True
                    break
            if not rejected:
                return end
        if is_stack_prologue(word):
            return None
    return None


def native_starts(directory: Path | None) -> set[int]:
    if directory is None:
        return set()
    pattern = re.compile(r"_0x([0-9a-fA-F]+)$")
    result = set()
    for path in directory.glob("*.cpp"):
        match = pattern.search(path.stem)
        if match:
            result.add(int(match.group(1), 16))
    return result


def report(elf: Path, map_path: Path, native_dir: Path | None, minimum_density: int) -> dict:
    words = dict(executable_words(elf))
    owned = merged_ranges(load_map(map_path))
    native = native_starts(native_dir)
    pointer_words = {
        address: value for address, value in words.items()
        if value % 4 == 0 and value in words
    }
    candidates: dict[int, dict[str, object]] = {}
    for reference, target in pointer_words.items():
        if contains(owned, target):
            continue
        first = words[target]
        leaf_end = short_leaf_end(words, target)
        if leaf_end is None and not is_stack_prologue(first) and target not in native:
            continue
        density = sum(reference + offset in pointer_words for offset in range(-16, 17, 4))
        if density < minimum_density:
            continue
        candidate = candidates.setdefault(target, {
            "target": f"0x{target:08X}",
            "first_word": f"0x{first:08X}",
            "leaf_return": leaf_end is not None,
            "leaf_end": f"0x{leaf_end:08X}" if leaf_end is not None else None,
            "native_function_start": target in native,
            "max_local_pointer_density": 0,
            "references": [],
        })
        candidate["max_local_pointer_density"] = max(
            int(candidate["max_local_pointer_density"]), density
        )
        references = candidate["references"]
        assert isinstance(references, list)
        if len(references) < 32:
            references.append(f"0x{reference:08X}")
    ordered = sorted(
        candidates.values(),
        key=lambda item: (
            not bool(item["native_function_start"]),
            -int(item["max_local_pointer_density"]),
            str(item["target"]),
        ),
    )
    return {
        "pointer_word_count": len(pointer_words),
        "candidate_count": len(ordered),
        "candidates": ordered,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument("--map", required=True, type=Path)
    parser.add_argument("--game", default=Path("config/game.toml"), type=Path)
    parser.add_argument("--native-generated-dir", type=Path)
    parser.add_argument("--minimum-density", type=int, default=3)
    parser.add_argument("--json-out", type=Path)
    args = parser.parse_args(argv)
    try:
        game, _ = load_game_config(args.game)
        actual = sha256_file(args.elf)
        if actual != game["elf_sha256"]:
            raise ConfigError(
                f"ELF SHA-256 mismatch: expected {game['elf_sha256']}, found {actual}"
            )
        result = report(args.elf, args.map, args.native_generated_dir, args.minimum_density)
    except (ConfigError, OSError, struct.error) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
