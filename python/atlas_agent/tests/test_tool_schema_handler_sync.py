from __future__ import annotations

import ast
import sys
from pathlib import Path


# Add src layout to sys.path for local runs
ROOT_DIR = Path(__file__).resolve().parents[2]
SRC_DIR = ROOT_DIR / "src"
if str(SRC_DIR) not in sys.path:
    sys.path.insert(0, str(SRC_DIR))


from atlas_agent.agent_team.tool_modules import ALL_MODULES, build_tools  # type: ignore  # noqa: E402


class _ArgsGetKeyVisitor(ast.NodeVisitor):
    def __init__(self) -> None:
        self.keys: set[str] = set()

    def visit_Call(self, node: ast.Call) -> None:
        # Match: args.get("key", ...)
        try:
            if isinstance(node.func, ast.Attribute) and node.func.attr == "get":
                if isinstance(node.func.value, ast.Name) and node.func.value.id == "args":
                    if node.args:
                        k0 = node.args[0]
                        if isinstance(k0, ast.Constant) and isinstance(k0.value, str):
                            self.keys.add(k0.value)
        except Exception:
            pass
        self.generic_visit(node)


def _extract_handle_args_get_keys(py_path: Path) -> dict[str, set[str]]:
    """Return mapping: tool_name -> {args.get(...) keys used in that branch}."""

    src = py_path.read_text(encoding="utf-8")
    tree = ast.parse(src, filename=str(py_path))

    handle_fn: ast.FunctionDef | None = None
    for node in tree.body:
        if isinstance(node, ast.FunctionDef) and node.name == "handle":
            handle_fn = node
            break
    if handle_fn is None:
        return {}

    out: dict[str, set[str]] = {}

    def _match_tool_name(test: ast.expr) -> str | None:
        # if name == "tool_name":
        if not isinstance(test, ast.Compare):
            return None
        if not (isinstance(test.left, ast.Name) and test.left.id == "name"):
            return None
        if len(test.ops) != 1 or not isinstance(test.ops[0], ast.Eq):
            return None
        if len(test.comparators) != 1:
            return None
        rhs = test.comparators[0]
        if isinstance(rhs, ast.Constant) and isinstance(rhs.value, str):
            return rhs.value
        return None

    def _walk_if_chain(node: ast.If) -> None:
        tname = _match_tool_name(node.test)
        if tname:
            visitor = _ArgsGetKeyVisitor()
            for stmt in node.body:
                visitor.visit(stmt)
            if visitor.keys:
                out.setdefault(tname, set()).update(visitor.keys)

        # elif chains are represented as nested If nodes in orelse.
        for stmt in node.orelse:
            if isinstance(stmt, ast.If):
                _walk_if_chain(stmt)

    for stmt in handle_fn.body:
        if isinstance(stmt, ast.If):
            _walk_if_chain(stmt)

    return out


def test_tool_schemas_cover_handle_args_get_keys() -> None:
    # Parse handler usage across all tool modules.
    used_by_tool: dict[str, set[str]] = {}
    for module in ALL_MODULES:
        mod_path = Path(getattr(module, "__file__", "") or "")
        if not mod_path.exists():
            continue
        mapping = _extract_handle_args_get_keys(mod_path)
        for name, keys in mapping.items():
            used_by_tool.setdefault(name, set()).update(keys)

    # Compare against advertised tool schemas.
    tools = build_tools()
    missing: dict[str, list[str]] = {}
    for tool in tools:
        used = used_by_tool.get(tool.name, set())
        if not used:
            continue
        props = {}
        try:
            props = tool.parameters_schema.get("properties") or {}
        except Exception:
            props = {}
        prop_keys = {str(k) for k in props.keys()} if isinstance(props, dict) else set()
        need = sorted(k for k in used if k not in prop_keys)
        if need:
            missing[tool.name] = need

    assert not missing, f"Tool schemas missing args keys used by handlers: {missing}"

