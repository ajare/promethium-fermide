#!/usr/bin/env python3
"""Audit the promethium-fermide-core shared-library export surface (#976).

promethium-fermide-core is SHARED (#962). On Windows MSVC only symbols that
carry PF_CORE_API cross the DLL boundary; on Linux the macro is a no-op because
the build never sets -fvisibility=hidden, so a missing annotation links happily
and stays invisible there.

This script reads the *actual* undefined-symbol set of every ELF target that
lists libpromethium-fermide-core in its NEEDED entries, resolves each symbol to
the core declaration that must supply it, and reports the ones whose core
declaration lacks PF_CORE_API. It is the repeatable form of the audit that found
the five types missed by the name-based pass behind #960 decision 1.

Usage:
    python3 scripts/audit_core_export_surface.py [--build-dir DIR]

Exit status: 0 when every externally-required core symbol is annotated,
1 when unannotated ones are found, 2 when the build cannot be inspected.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
CORE_INCLUDE = REPO_ROOT / "core" / "include"
CORE_LIBRARY_NAME = "libpromethium-fermide-core.so"

# `nm --undefined-only` prints a symbol-type column, e.g. `         U foo::bar()`.
SYMBOL_TYPE_COLUMN_RE = re.compile(r"^\s*[A-Za-z?]\s+")

# Demangled thunks, vtables and typeinfo name a class rather than a member.
IMPLICIT_RE = re.compile(
    r"^(?:(?:non-)?virtual thunk to |construction vtable for |vtable for |"
    r"typeinfo name for |typeinfo for )(.+?)\.?$"
)

CLASS_DECL_RE = re.compile(
    r"^(?:template\s*<.*>\s*)?"
    r"(?:class|struct)\s+"
    r"(?P<api>PF_CORE_API\s+)?"
    r"(?P<name>[A-Za-z_]\w*)\b"
)

ACCESS_SPECIFIER_RE = re.compile(r"^(?:public|private|protected)\s*:\s*")

NAMESPACE_LINE_RE = re.compile(
    r"^namespace\s+(?P<name>[A-Za-z_]\w*(?:::[A-Za-z_]\w*)*)?\s*(\{)?\Z")

FREE_FN_RE = re.compile(
    r"^(?:(?:inline|constexpr|explicit|extern)\s+)*"
    r"(?P<api>PF_CORE_API\s+)?"
    r"(?:\[\[nodiscard\]\]\s+)?"
    r"(?P<ret>[A-Za-z_][\w:<>,*&\s]*?)\s+"
    r"(?P<name>[A-Za-z_]\w*)\s*\("
)

KEYWORDS = {
    "return", "if", "while", "for", "else", "switch", "catch", "do", "case",
    "static_assert", "using", "typedef", "operator", "delete", "new",
}


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", text)


def join_qual(prefix: str, name: str) -> str:
    return f"{prefix}::{name}" if prefix else name


def scan_header(path: Path) -> tuple[dict[str, bool], dict[str, bool]]:
    """Return (qualified types, qualified free functions) declared in one header.

    Scope-aware: nested types are recorded as `Outer::Inner` so a nested
    `struct Agent` cannot be confused with `core::Agent`, and class methods are
    never mistaken for namespace-scope free functions. Values say whether the
    declaration carries PF_CORE_API.
    """
    types: dict[str, bool] = {}
    functions: dict[str, bool] = {}
    # (qualified prefix, kind, body depth); kind is 'ns' or 'type'.
    scopes: list[tuple[str, str, int]] = []
    depth = 0
    buf = ""

    def prefix() -> str:
        return scopes[-1][0] if scopes else ""

    def in_namespace() -> bool:
        return not scopes or scopes[-1][1] == "ns"

    def close_scopes() -> None:
        while scopes and scopes[-1][2] > depth:
            scopes.pop()

    for raw in strip_comments(path.read_text(encoding="utf-8")).splitlines():
        line = raw.strip()
        if not line:
            continue
        if line.startswith("#"):
            buf = ""  # a preprocessor directive never continues a declaration
            continue
        buf = ACCESS_SPECIFIER_RE.sub("", (buf + " " + line).strip())
        if ";" not in buf and "{" not in buf and "}" not in buf:
            continue  # declaration continues on the next line

        before = depth
        depth += buf.count("{") - buf.count("}")
        # Only an unbalanced brace opens a scope; `= {}` default arguments and
        # member initializers that both open and close inside this declaration
        # do not.
        opened = buf.count("{") > buf.count("}")

        if buf.startswith("friend ") or buf.startswith("enum"):
            buf = ""
            close_scopes()
            continue

        ns = NAMESPACE_LINE_RE.match(buf)
        if ns and opened:
            scopes.append((join_qual(prefix(), ns.group("name") or ""),
                          "ns", before + 1))
            buf = ""
            continue

        decl = CLASS_DECL_RE.match(buf)
        if decl:
            name = decl.group("name")
            qual = join_qual(prefix(), name)
            if opened:
                scopes.append((qual, "type", before + 1))
            else:
                # `class Foo;` forward-declares and defines no symbols; a base
                # list (`class Foo :`) only appears on the definition.
                rest = buf[buf.index(name) + len(name):].strip()
                if not rest.startswith(":"):
                    buf = ""
                    close_scopes()
                    continue
            # A definition site wins over a bare re-declaration of the name.
            annotated = bool(decl.group("api"))
            if qual not in types or annotated:
                types[qual] = annotated
            buf = ""
            close_scopes()
            continue

        # Namespace-scope free function declaration (never a class method).
        if in_namespace() and not opened:
            fn = FREE_FN_RE.match(buf)
            if fn and fn.group("name") not in KEYWORDS:
                qual = join_qual(prefix(), fn.group("name"))
                annotated = bool(fn.group("api"))
                if qual not in functions or annotated:
                    functions[qual] = annotated

        buf = ""
        close_scopes()

    return types, functions


def collect_core_declarations() -> tuple[dict[str, bool], dict[str, bool]]:
    types: dict[str, bool] = {}
    functions: dict[str, bool] = {}
    for header in sorted(CORE_INCLUDE.rglob("*.h")):
        if header.name == "Platform.h":
            continue
        h_types, h_functions = scan_header(header)
        for qual, annotated in h_types.items():
            if qual not in types or annotated:
                types[qual] = annotated
        for qual, annotated in h_functions.items():
            if qual not in functions or annotated:
                functions[qual] = annotated
    return types, functions


def readelf_needed(path: Path) -> list[str]:
    proc = subprocess.run(
        ["readelf", "-d", str(path)], capture_output=True, text=True, check=False)
    libs = []
    for line in proc.stdout.splitlines():
        if "(NEEDED)" in line:
            m = re.search(r"\[([^\]]+)\]", line)
            if m:
                libs.append(m.group(1))
    return libs


def symbol_head(symbol: str) -> str:
    """The qualified name part of a demangled symbol, decorations removed."""
    implicit = IMPLICIT_RE.match(symbol)
    head = implicit.group(1) if implicit else symbol.split("(")[0]
    head = re.sub(r"\[[^\]]*\]", "", head)  # [abi:cxx11]
    head = re.sub(r"<[^<>]*>", "", head)    # template arguments
    return head.strip()


def resolve_owner(head: str, types: dict[str, bool]) -> str | None:
    """Longest qualified core type that owns this symbol, if any."""
    parts = [p for p in head.split("::") if p]
    for end in range(len(parts), 0, -1):
        qual = "::".join(parts[:end])
        if qual in types:
            return qual
    return None


def dynamic_exports(path: Path) -> set[str]:
    """Mangled symbols a shared object actually publishes.

    Lines look like `<address> <type> <symbol>`; take the last field.
    """
    proc = subprocess.run(
        ["nm", "-D", "--defined-only", str(path)],
        capture_output=True, text=True, check=False)
    return {parts[-1] for line in proc.stdout.splitlines()
            if len(parts := line.split()) >= 2}


def mangled_undefined(path: Path) -> set[str]:
    """Mangled undefined symbols of a target (no -C, so they can be matched).

    Undefined lines carry no address: `U <symbol>`, i.e. two fields.
    """
    proc = subprocess.run(
        ["nm", "--undefined-only", str(path)],
        capture_output=True, text=True, check=False)
    return {parts[-1] for line in proc.stdout.splitlines()
            if len(parts := line.split()) >= 2}


def check_published_surface(build_dir: Path, core_types: set[str]) -> list[str]:
    """Cross-check the published surface, not just the annotated declarations.

    A consumer's undefined core symbols must appear in core's dynamic symbol
    table. With default GCC visibility this holds trivially; against a build
    configured with -fvisibility=hidden it is the real boundary, and anything
    missing is exactly what MSVC would report as LNK2019. `core_types` is the
    set of qualified core type names, used to ignore non-core symbols.
    """
    core = build_dir / "bin" / "x64" / "Release" / CORE_LIBRARY_NAME
    if not core.is_file():
        return [f"core library not found at {core}"]
    published = dynamic_exports(core)
    type_lookup = {t: True for t in core_types}
    problems: list[str] = []
    for target in find_core_targets(build_dir):
        undefined = sorted(mangled_undefined(target))
        # One c++filt call per target: it maps its stdin lines to demangled
        # output in order, so the two lists stay aligned.
        proc = subprocess.run(["c++filt"], input="\n".join(undefined),
                             capture_output=True, text=True, check=False)
        demangled = proc.stdout.splitlines()
        if len(demangled) != len(undefined):
            return [f"c++filt output length {len(demangled)} != input "
                   f"{len(undefined)} for {target}"]
        for mangled, name in zip(undefined, demangled):
            if resolve_owner(symbol_head(name), type_lookup) is None:
                continue  # not a core-owned symbol
            if mangled not in published:
                problems.append(f"{target.relative_to(build_dir)}: {name}")
    return problems


def find_core_targets(build_dir: Path) -> list[Path]:
    targets = []
    for path in sorted(build_dir.rglob("*")):
        if not path.is_file() or path.is_symlink() or path.name == CORE_LIBRARY_NAME:
            continue
        try:
            with path.open("rb") as handle:
                if handle.read(4) != b"\x7fELF":
                    continue
        except OSError:
            continue
        if CORE_LIBRARY_NAME in readelf_needed(path):
            targets.append(path)
    return targets


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=REPO_ROOT / "build-linux")
    parser.add_argument("--published-surface", action="store_true",
                       help="also cross-check each consumer's undefined core "
                            "symbols against core's dynamic symbol table; only "
                            "meaningful against a -fvisibility=hidden build")
    args = parser.parse_args()
    build_dir = args.build_dir.resolve()
    if not build_dir.is_dir():
        print(f"Build directory not found: {build_dir}", file=sys.stderr)
        return 2

    types, functions = collect_core_declarations()
    print(f"core/include: {len(types)} type declarations, "
          f"{len(functions)} free-function declarations scanned.")

    targets = find_core_targets(build_dir)
    if not targets:
        print(f"No target links {CORE_LIBRARY_NAME}; nothing to audit.", file=sys.stderr)
        return 2
    print(f"Auditing {len(targets)} target(s) that link {CORE_LIBRARY_NAME}.")

    missing: dict[str, set[str]] = {}
    resolved = 0
    for target in targets:
        proc = subprocess.run(
            ["nm", "-C", "--undefined-only", str(target)],
            capture_output=True, text=True, check=False)
        if proc.returncode != 0:
            print(f"nm failed on {target}: {proc.stderr.strip()}", file=sys.stderr)
            return 2
        label = str(target.relative_to(build_dir))
        for raw in proc.stdout.splitlines():
            symbol = SYMBOL_TYPE_COLUMN_RE.sub("", raw).strip()
            if not symbol:
                continue
            head = symbol_head(symbol)
            owner = resolve_owner(head, types)
            if owner is not None:
                resolved += 1
                if not types[owner]:
                    missing.setdefault(label, set()).add(f"{owner}  <-  {symbol}")
            elif head in functions and not functions[head]:
                resolved += 1
                missing.setdefault(label, set()).add(f"{head}()  <-  {symbol}")

    print(f"Resolved {resolved} undefined symbol(s) to core declarations.\n")
    if resolved == 0:
        print("SUSPECT: no undefined symbol resolved to core, so the audit is "
              "not reading the binaries correctly.", file=sys.stderr)
        return 2

    failed = bool(missing)
    if not missing:
        print("PASS: every externally-required core symbol carries PF_CORE_API.")
    else:
        print("FAIL: core symbols required across the boundary without PF_CORE_API:\n")
        for label in sorted(missing):
            print(f"--- {label} ---")
            for item in sorted(missing[label]):
                print(f"  {item}")
            print()
        owners = sorted({i.split("  <-  ")[0] for v in missing.values() for i in v})
        print(f"{len(owners)} unannotated core declaration(s):")
        for owner in owners:
            print(f"  {owner}")

    if args.published_surface:
        print("\nCross-checking the published dynamic surface of core...")
        problems = check_published_surface(build_dir, set(types))
        if problems:
            failed = True
            print(f"FAIL: {len(problems)} core symbol(s) a consumer needs but "
                  f"core does not publish:")
            for problem in sorted(problems):
                print(f"  {problem}")
        else:
            print("PASS: every core symbol a consumer needs is published by core.")

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())