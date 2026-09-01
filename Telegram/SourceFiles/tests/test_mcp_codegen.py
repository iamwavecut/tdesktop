#!/usr/bin/env python3

import importlib.util
import json
import pathlib
import sys


ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "SourceFiles" / "codegen" / "mcp" / "codegen_mcp_schema.py"
API = ROOT / "SourceFiles" / "mtproto" / "scheme" / "api.tl"
MTPROTO = ROOT / "SourceFiles" / "mtproto" / "scheme" / "mtproto.tl"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def load_generator():
    spec = importlib.util.spec_from_file_location("codegen_mcp_schema", SCRIPT)
    require(spec is not None and spec.loader is not None, "generator cannot load")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def main() -> None:
    generator = load_generator()
    first = generator.parse_schema([MTPROTO, API])
    second = generator.parse_schema([MTPROTO, API])
    require(first == second, "schema generation is not deterministic")
    require(first["layer"] == 229, "wrong API layer")
    require(len(first["constructors"]) == 1658, "wrong constructor count")
    require(len(first["methods"]) == 813, "wrong method count")
    require(len(first["transport_constructors"]) == 40,
            "mtproto.tl constructors were not embedded")
    require(len(first["transport_methods"]) == 10,
            "mtproto.tl methods were not embedded")
    methods = {entry["method"]: entry for entry in first["methods"]}
    send = methods["messages.sendMessage"]
    require(send["type"] == "Updates", "sendMessage result type changed")
    params = {entry["name"]: entry["type"] for entry in send["params"]}
    require(params["flags"] == "#", "sendMessage flags were not parsed")
    require(params["peer"] == "InputPeer", "sendMessage peer was not parsed")
    require(params["random_id"] == "long", "sendMessage random_id was not parsed")
    invoke = methods["invokeAfterMsg"]
    require(invoke["generic_params"] == ["X"], "generic X was not preserved")
    require(invoke["params"][1] == {"name": "query", "type": "!X"},
            "generic query parameter was not parsed")
    encoded = json.dumps(first, ensure_ascii=False, sort_keys=True,
                         separators=(",", ":"))
    require('"messages.sendMessage"' in encoded, "encoded schema lost methods")


if __name__ == "__main__":
    main()
