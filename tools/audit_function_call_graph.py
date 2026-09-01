#!/usr/bin/env python3
"""Build a direct function graph and rank omitted gap owners near target routines."""

from __future__ import annotations

import argparse
import json
import struct
import sys
from bisect import bisect_right
from collections import deque
from pathlib import Path

from merge_function_config import ConfigError, load_game_config, sha256_file
from report_function_coverage import Span, executable_words, load_binding_addresses, load_map


def owner_at(spans: list[Span], starts: list[int], address: int) -> Span | None:
    index = bisect_right(starts, address) - 1
    if index >= 0 and address < spans[index].end:
        return spans[index]
    return None


def build_graph(
    words: dict[int, int], spans: list[Span], executable: set[int],
) -> tuple[dict[int, list[dict[str, object]]], list[dict[str, object]], dict[int, int]]:
    spans = sorted(spans)
    starts = [span.start for span in spans]
    graph: dict[int, list[dict[str, object]]] = {span.start: [] for span in spans}
    unresolved: list[dict[str, object]] = []
    indirect_counts: dict[int, int] = {}
    for span in spans:
        for address in range(span.start, span.end, 4):
            word = words.get(address)
            if word is None:
                continue
            opcode = word >> 26
            kind = None
            target = None
            if opcode in (2, 3):
                target = ((address + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
                kind = "call" if opcode == 3 else "tail_or_jump"
            elif opcode == 0 and (word & 0x3F) in (8, 9):
                indirect_counts[span.start] = indirect_counts.get(span.start, 0) + 1
            if target is None or target not in executable:
                continue
            target_owner = owner_at(spans, starts, target)
            if target_owner is not None and target_owner.start == span.start:
                continue
            edge = {
                "source": address,
                "target": target,
                "target_owner": target_owner.start if target_owner else None,
                "kind": kind,
            }
            graph[span.start].append(edge)
            if target_owner is None:
                unresolved.append({"owner": span.start, **edge})
    return graph, unresolved, indirect_counts


def shortest_path(
    graph: dict[int, list[dict[str, object]]], start: int, target: int,
) -> list[dict[str, object]] | None:
    pending = deque([start])
    previous: dict[int, tuple[int, dict[str, object]] | None] = {start: None}
    while pending:
        node = pending.popleft()
        if node == target:
            result: list[dict[str, object]] = []
            while previous[node] is not None:
                prior, edge = previous[node]
                result.append(edge)
                node = prior
            return list(reversed(result))
        for edge in graph.get(node, []):
            neighbor = edge["target_owner"]
            if isinstance(neighbor, int) and neighbor not in previous:
                previous[neighbor] = (node, edge)
                pending.append(neighbor)
    return None


def hex_edge(edge: dict[str, object]) -> dict[str, object]:
    return {
        key: f"0x{value:08X}" if key in ("source", "target", "target_owner")
        and isinstance(value, int) else value
        for key, value in edge.items()
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument("--map", required=True, type=Path)
    parser.add_argument("--gap-report", type=Path)
    parser.add_argument("--config", type=Path,
                        help="effective config whose runtime bindings satisfy direct edges")
    parser.add_argument("--target", action="append", default=[])
    parser.add_argument("--game", default=Path("config/game.toml"), type=Path)
    parser.add_argument("--json-out", type=Path)
    args = parser.parse_args(argv)
    try:
        game, _ = load_game_config(args.game)
        actual = sha256_file(args.elf)
        if actual != game["elf_sha256"]:
            raise ConfigError(
                f"ELF SHA-256 mismatch: expected {game['elf_sha256']}, found {actual}"
            )
        mapped = load_map(args.map)
        candidates: list[Span] = []
        if args.gap_report:
            gap_data = json.loads(args.gap_report.read_text(encoding="utf-8"))
            for item in gap_data.get("candidates", []):
                start, end = int(item["start"], 0), int(item["end"], 0)
                candidates.append(Span(start, end, f"gap_{start:08X}"))
        spans = sorted(mapped + candidates)
        words = dict(executable_words(args.elf))
        graph, unresolved, indirect_counts = build_graph(words, spans, set(words))
        bindings = load_binding_addresses(args.config) if args.config else {}
        binding_edges = [edge for edge in unresolved if edge["target"] in bindings]
        unresolved = [edge for edge in unresolved if edge["target"] not in bindings]
        span_by_start = {span.start: span for span in spans}
        starts = [span.start for span in spans]
        targets: list[int] = []
        for literal in args.target:
            requested = int(literal, 0)
            owner = owner_at(spans, starts, requested)
            if owner is None:
                raise ConfigError(f"target 0x{requested:08X} has no function owner")
            targets.append(owner.start)
        candidate_paths = []
        for candidate in candidates:
            for target in targets:
                path = shortest_path(graph, candidate.start, target)
                if path is not None:
                    candidate_paths.append({
                        "candidate": f"0x{candidate.start:08X}",
                        "candidate_end": f"0x{candidate.end:08X}",
                        "target_owner": f"0x{target:08X}",
                        "edge_count": len(path),
                        "path": [hex_edge(edge) for edge in path],
                    })
        candidate_paths.sort(key=lambda item: (item["edge_count"], item["candidate"], item["target_owner"]))
        report = {
            "mapped_function_count": len(mapped),
            "gap_candidate_count": len(candidates),
            "direct_edge_count": sum(len(edges) for edges in graph.values()),
            "intentional_binding_edge_count": len(binding_edges),
            "intentional_binding_edges": [hex_edge(edge) for edge in binding_edges],
            "unowned_direct_edge_count": len(unresolved),
            "unowned_direct_edges": [hex_edge(edge) for edge in unresolved],
            "indirect_site_count": sum(indirect_counts.values()),
            "gap_candidates_with_indirect_sites": [
                {"candidate": f"0x{start:08X}", "count": indirect_counts[start]}
                for start in sorted({candidate.start for candidate in candidates} & indirect_counts.keys())
            ],
            "candidate_path_count": len(candidate_paths),
            "candidate_paths": candidate_paths,
        }
    except (ConfigError, OSError, ValueError, json.JSONDecodeError, struct.error) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
