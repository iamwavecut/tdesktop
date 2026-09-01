#!/usr/bin/env python3

import json
import pathlib
import re
import sys


sys.dont_write_bytecode = True

ENTRY = re.compile(
    r"^(?P<name>[^\s#]+)#(?P<id>[0-9a-fA-F]+)"
    r"(?:\s+(?P<body>.*?))?\s*=\s*(?P<type>[^;]+);$"
)
LAYER = re.compile(r"^//\s*LAYER\s+(\d+)\s*$")


def signed_id(value: str) -> str:
    number = int(value, 16)
    if number >= 1 << 31:
        number -= 1 << 32
    return str(number)


def parse_entry(line: str, method: bool):
    match = ENTRY.match(line)
    if not match:
        return None
    params = []
    generic_params = []
    for token in (match.group("body") or "").split():
        if token.startswith("{") and token.endswith("}"):
            generic = token[1:-1].split(":", 1)
            if len(generic) == 2 and generic[1] == "Type":
                generic_params.append(generic[0])
            continue
        if ":" not in token:
            continue
        name, value_type = token.split(":", 1)
        params.append({"name": name, "type": value_type})
    result = {
        "id": signed_id(match.group("id")),
        "params": params,
        "type": match.group("type").strip(),
    }
    result["method" if method else "predicate"] = match.group("name")
    if generic_params:
        result["generic_params"] = generic_params
    return result


def parse_file(path):
    result = {"layer": 0, "constructors": [], "methods": []}
    functions = False
    with path.open(encoding="utf-8") as source:
        for raw in source:
            line = raw.strip()
            layer = LAYER.match(line)
            if layer:
                result["layer"] = int(layer.group(1))
                continue
            if line == "---functions---":
                functions = True
                continue
            if line == "---types---":
                functions = False
                continue
            if not line or line.startswith("//"):
                continue
            entry = parse_entry(line, functions)
            if entry:
                result["methods" if functions else "constructors"].append(entry)
    return result


def parse_schema(paths):
    named = {pathlib.Path(path).name: pathlib.Path(path) for path in paths}
    if set(named) != {"api.tl", "mtproto.tl"}:
        raise ValueError("exactly one api.tl and one mtproto.tl are required")
    result = parse_file(named["api.tl"])
    if not result["layer"]:
        raise ValueError("api.tl has no layer marker")
    transport = parse_file(named["mtproto.tl"])
    result["transport_constructors"] = transport["constructors"]
    result["transport_methods"] = transport["methods"]
    return result


def write_if_changed(path: pathlib.Path, content: str) -> None:
    if path.exists() and path.read_text(encoding="utf-8") == content:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8", newline="\n")


def generate(output: pathlib.Path, paths) -> None:
    encoded = json.dumps(
        parse_schema(paths),
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
    )
    header = """/*
This file is generated from the Telegram TL schema.
*/
#pragma once

class QByteArray;

namespace Core::Mcp {

[[nodiscard]] const QByteArray &GeneratedSchemaJson();

} // namespace Core::Mcp
"""
    source = f'''/*
This file is generated from the Telegram TL schema.
*/
#include "{output.name}.h"

#include <QtCore/QByteArray>

namespace Core::Mcp {{

const QByteArray &GeneratedSchemaJson() {{
\tstatic const auto result = QByteArray(R"MCP({encoded})MCP");
\treturn result;
}}

}} // namespace Core::Mcp
'''
    write_if_changed(output.with_suffix(".h"), header)
    write_if_changed(output.with_suffix(".cpp"), source)


def main(argv) -> None:
    output = None
    inputs = []
    for argument in argv:
        if argument.startswith("-o"):
            output = pathlib.Path(argument[2:])
        else:
            inputs.append(pathlib.Path(argument))
    if output is None or not inputs:
        raise SystemExit("usage: codegen_mcp_schema.py -o<path> <schemas...>")
    generate(output, inputs)


if __name__ == "__main__":
    main(sys.argv[1:])
