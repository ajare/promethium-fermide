#!/usr/bin/env python3
"""Audit the WorldRuntime read surface against the public read set on World (#981).

#966 defined the read surface by a rule rather than a list: `const` is the
compiler-checked definition of "read" in this codebase, so every read `World`
answers through a `const` member is answered by `game::WorldRuntime`. That
rule has a blind spot this script closes. A `static` read has no const
qualification to get wrong, so nothing about it fails to compile when it is
never forwarded: `isValidTransportStopShortName` sat on #960's list of six
read-shaped methods, was never made const because it never needed to be, and
the const work in #963/#974 passed it by. It was reachable as a `World` read
and absent as a runtime read.

So the audited set is the public `const` method set on `World` *plus* the
public `static` methods that read no state. Every one of them must be answered
by `WorldRuntime`, under the same name or under a documented rename, except
for the exclusions listed below - and each exclusion is itself checked rather
than trusted, so an exclusion that stops being true shows up as a gap.

Usage:
    python3 scripts/audit_read_surface_completeness.py [--json]

Exit status: 0 when the forwarded set covers the audited set, 1 when a gap or
a stale exclusion is found, 2 when the sources cannot be parsed.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
WORLD_HEADER = REPO_ROOT / "core" / "include" / "core" / "World.h"
RUNTIME_HEADER = REPO_ROOT / "pf-game-runtime" / "include" / "game" / "WorldRuntime.h"
AUTHORING_HEADER = REPO_ROOT / "pf-game-authoring" / "include" / "game" / "WorldAuthoring.h"

TYPE_KEYWORDS = re.compile(r"^(class|struct|union|enum)\b")
ACCESS_SPECIFIER = re.compile(r"^(public|private|protected)\s*:")

# Exclusion 1: `const` only because its cache is `mutable`. It writes, so it
# is not a read and has no place on a read surface (#966).
WRITES_BEHIND_CONST = {"invalidateSimulationSnapshot"}

# Exclusion 2: the resource-name reads, which the runtime answers under the
# `WorldAuthoring`-side naming rather than the `get...` spelling `World` uses
# (#969). Each maps to the runtime method that must actually exist.
RENAMED_READS = {
    "getAgentTagRegistryResourceName": "agentTagRegistryResourceName",
    "getAgentBehaviourRegistryResourceName": "agentBehaviourRegistryResourceName",
}


def strip_comments(text: str) -> str:
    """Blank out comments so they cannot be mistaken for declarations."""
    out: list[str] = []
    i, n = 0, len(text)
    while i < n:
        char = text[i]
        if char == "/" and i + 1 < n and text[i + 1] == "*":
            end = text.find("*/", i + 2)
            i = n if end == -1 else end + 2
            continue
        if char == "/" and i + 1 < n and text[i + 1] == "/":
            end = text.find("\n", i)
            i = n if end == -1 else end
            continue
        if char in "\"'":
            quote = char
            out.append(char)
            i += 1
            while i < n:
                if text[i] == "\\":
                    out.append(text[i : i + 2])
                    i += 2
                    continue
                out.append(text[i])
                i += 1
                if text[i - 1] == quote:
                    break
            continue
        out.append(char)
        i += 1
    return "".join(out)


def class_body_members(text: str, class_name: str) -> list[tuple[str, str]]:
    """Return (kind, declaration) pairs from the direct body of `class_name`.

    `kind` is ``method`` for a declaration or definition of a member
    function - including one defined inline with a body - and ``other`` for
    nested types, fields, friends and using-declarations, which are skipped
    wholesale but must not confuse the access tracking.
    """
    match = re.search(
        r"\bclass\s+(?:PF_[A-Z_]*_API\s+)?" + class_name + r"\b[^;{]*\{", text
    )
    if not match:
        raise ValueError(f"class {class_name} not found")

    i = match.end()
    depth = 1
    access = "private"  # `class` defaults to private
    members: list[tuple[str, str]] = []
    buf = ""

    def record(raw: str) -> None:
        nonlocal access
        decl = " ".join(raw.split())
        spec = ACCESS_SPECIFIER.match(decl)
        if spec:
            access = spec.group(1)
            decl = decl[spec.end():].strip()
        if not decl:
            return
        kind = "method" if ("(" in decl and not TYPE_KEYWORDS.match(decl)) else "other"
        members.append((kind + ":" + access, decl))

    while i < len(text):
        char = text[i]
        if char == "{" and depth == 1:
            decl = " ".join(buf.split())
            # Skip the whole nested body, but keep the declaration so an
            # inline method definition is audited and an access specifier
            # that opened with the brace is honoured.
            j, inner = i + 1, 1
            while j < len(text) and inner > 0:
                if text[j] == "{":
                    inner += 1
                elif text[j] == "}":
                    inner -= 1
                j += 1
            record(decl)
            i = j
            while i < len(text) and text[i] in " \t\n;":
                i += 1
            buf = ""
            continue
        if char == "}":
            depth -= 1
            if depth == 0:
                if buf.strip():
                    record(buf)
                break
        elif char == ";":
            if buf.strip():
                record(buf)
            buf = ""
            i += 1
            continue
        buf += char
        i += 1
    return members


def parse_method(decl: str) -> dict | None:
    """Describe a member declaration: name, static, const."""
    if decl.startswith(("friend ", "using ", "typedef ", "static_assert")):
        return None
    if "(" not in decl:
        return None
    open_paren = decl.find("(")
    end = open_paren - 1
    while end >= 0 and (decl[end].isalnum() or decl[end] == "_"):
        end -= 1
    name = decl[end + 1 : open_paren]
    if not name or TYPE_KEYWORDS.match(decl):
        return None
    depth = 0
    close = open_paren
    while close < len(decl):
        if decl[close] == "(":
            depth += 1
        elif decl[close] == ")":
            depth -= 1
            if depth == 0:
                break
        close += 1
    tail = decl[close + 1 :]
    return {
        "name": name,
        "static": re.search(r"\bstatic\b", decl[:open_paren]) is not None,
        "const": re.match(r"^\s*(?:noexcept\s+)?const\b", tail) is not None,
        "decl": decl,
    }


def public_reads(path: Path, class_name: str) -> list[dict]:
    members = class_body_members(strip_comments(path.read_text()), class_name)
    reads = []
    for kind, decl in members:
        if not kind.endswith(":public"):
            continue
        method = parse_method(decl)
        if method is None:
            continue
        # A read is a const member, or a static that reads no state. Statics
        # are the half the const pass could not see (#981).
        if method["const"] or method["static"]:
            reads.append(method)
    return reads


def public_names(path: Path, class_name: str) -> set[str]:
    members = class_body_members(strip_comments(path.read_text()), class_name)
    names = set()
    for kind, decl in members:
        if not kind.endswith(":public"):
            continue
        method = parse_method(decl)
        if method is not None:
            names.add(method["name"])
    return names


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--json", action="store_true", help="emit the audit as JSON")
    args = parser.parse_args()

    try:
        world_reads = public_reads(WORLD_HEADER, "World")
        runtime_names = public_names(RUNTIME_HEADER, "WorldRuntime")
        authoring_names = public_names(AUTHORING_HEADER, "WorldAuthoring")
    except (OSError, ValueError) as fault:
        print(f"audit could not read the sources: {fault}", file=sys.stderr)
        return 2

    const_reads = sorted({m["name"] for m in world_reads if m["const"]})
    static_reads = sorted({m["name"] for m in world_reads if m["static"]})

    gaps: list[str] = []
    excluded: dict[str, str] = {}

    for name in sorted(set(const_reads) | set(static_reads)):
        if name in runtime_names:
            continue
        if name in WRITES_BEHIND_CONST:
            excluded[name] = "const only because its cache is mutable: it writes"
            continue
        if name in RENAMED_READS:
            target = RENAMED_READS[name]
            if target in runtime_names:
                excluded[name] = f"answered as {target}()"
            else:
                gaps.append(f"{name} (renamed to {target}(), which is also missing)")
            continue
        # Exclusion 3: the authoring families. A `can...` guard or a
        # `plan...` edit plan is a read of what an author may do, and it is
        # answered through WorldAuthoring, never through the runtime (#967).
        # The exclusion is only real while WorldAuthoring actually answers it.
        if name.startswith(("can", "plan")) and name in authoring_names:
            excluded[name] = "authoring read, answered by WorldAuthoring"
            continue
        gaps.append(name)

    stale = []
    for name in WRITES_BEHIND_CONST | set(RENAMED_READS):
        if name not in set(const_reads) | set(static_reads):
            stale.append(name)
    for name in RENAMED_READS.values():
        if name not in runtime_names:
            stale.append(f"{name} (rename target missing from WorldRuntime)")

    report = {
        "worldPublicConstReads": len(const_reads),
        "worldPublicStaticReads": len(static_reads),
        "worldPublicStaticNames": static_reads,
        "runtimePublicMethods": len(runtime_names),
        "excluded": excluded,
        "gaps": gaps,
        "staleExclusions": stale,
    }

    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print(
            f"World public const reads: {len(const_reads)}, "
            f"public static reads: {len(static_reads)} "
            f"({', '.join(static_reads)}), "
            f"WorldRuntime public methods: {len(runtime_names)}"
        )
        print(f"Documented exclusions: {len(excluded)}")
        if gaps:
            print(f"GAPS ({len(gaps)}):")
            for gap in gaps:
                print(f"  - {gap}")
        else:
            print("GAPS: none - the runtime covers every public World read")
        if stale:
            print(f"STALE EXCLUSIONS ({len(stale)}):")
            for entry in stale:
                print(f"  - {entry}")

    if gaps or stale:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
