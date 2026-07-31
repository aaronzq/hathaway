#!/usr/bin/env python3
"""Slice code blocks out of the old and new firmware so the golden test
compares the *real* code, not a hand-transcribed copy of it."""
import pathlib
import sys

HERE = pathlib.Path(__file__).parent
ROOT = HERE.parent


def slice_between(lines, start_sub, end_pred, what):
    out, on = [], False
    for ln in lines:
        if not on and start_sub in ln:
            on = True
        if on:
            out.append(ln)
            if len(out) > 1 and end_pred(ln):
                return out
    sys.exit(f"extract: could not slice {what!r}")


def brace_fn(lines, start_sub, what):
    """A top-level function: from its signature to the first line that is '}'."""
    return slice_between(lines, start_sub, lambda l: l.rstrip() == "}", what)


def decl(lines, start_sub, end_sub, what):
    return slice_between(lines, start_sub, lambda l: end_sub in l, what)


old = (HERE / "old" / "hathaway_old.ino").read_text().splitlines(keepends=True)
new = (ROOT / "hathaway.ino").read_text().splitlines(keepends=True)

old_parts = (
    decl(old, "static const ParamSpec PARAM_TABLE[]", "PARAM_COUNT =", "old PARAM_TABLE")
    + brace_fn(old, "static const char *paramName", "old paramName")
    + brace_fn(old, "static int formatTelem", "old formatTelem")
    + brace_fn(old, "static float paramValue", "old paramValue")
    + brace_fn(old, "static void handleCommandLine", "old handleCommandLine")
)

cmd_table = decl(new, "static const CmdSpec CMD_TABLE[]", "CMD_COUNT =",
                 "new CMD_TABLE")

new_parts = (
    decl(new, "enum : uint8_t {", "};", "new telemetry ids")
    + decl(new, "static const TelemSpec TELEM_TABLE[]", "TELEM_COUNT =", "new TELEM_TABLE")
    + cmd_table
)


def make_stubs(rows, task_header):
    """Generate the globals and apply-function stubs CMD_TABLE refers to.

    Doing this automatically is what lets you add a parameter or an action to
    hathaway.ino without also hand-editing the test.
    """
    import re
    src = "".join(rows)
    defaults = {}
    for m in re.finditer(r"^\s*(unsigned long|float)\s+(\w+)\s*=\s*([^;]+);",
                         task_header, re.M):
        defaults[m.group(2)] = (m.group(1), m.group(3).strip())

    out, fns = [], []
    for ctype, var, fn in re.findall(
            r"PARAM_(U32|F32)\(\s*(\w+)\s*,[^,]+,[^,]+,\s*(\w+|nullptr)\s*\)", src):
        decl_type, value = defaults.get(
            var, ("unsigned long" if ctype == "U32" else "float", "0"))
        out.append(f"{decl_type} {var} = {value};\n")
        fns.append(fn)
    for name, fn in re.findall(
            r"ACTION(?:_QUIET)?\(\s*(\w+)\s*,\s*(\w+|nullptr)\s*\)", src):
        fns.append(fn)

    seen = set()
    for fn in fns:
        if fn != "nullptr" and fn not in seen:
            seen.add(fn)
            out.append(f"static void {fn}(float) {{}}\n")
    return out


stubs = make_stubs(cmd_table,
                   (ROOT / "behavior_task.h").read_text())

(HERE / "old" / "old_extracted.inc").write_text("".join(old_parts))
(HERE / "new_extracted.inc").write_text("".join(new_parts))
(HERE / "new_stubs.inc").write_text("".join(stubs))
print(f"extracted {len(old_parts)} old lines, {len(new_parts)} new lines, "
      f"{len(stubs)} generated stubs")
