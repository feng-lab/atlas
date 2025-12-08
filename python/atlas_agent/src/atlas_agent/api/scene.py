"""Typed wrappers around SceneClient for Python scripts.

These helpers raise exceptions on failure and return native Python types.
"""

from pathlib import Path

from ..scene_rpc import SceneClient


class SceneAPI:
    def __init__(self, client: SceneClient):
        self._c = client

    # Load
    def ensure_loaded(self, files: list[str]) -> dict:
        return self._c.ensure_loaded(files)

    def list_objects(self) -> list[dict]:
        resp = self._c.list_objects()
        out = []
        for o in getattr(resp, "objects", []):
            out.append({
                "id": int(getattr(o, "id", 0)),
                "type": getattr(o, "type", ""),
                "name": getattr(o, "name", ""),
                "path": getattr(o, "path", ""),
                "visible": bool(getattr(o, "visible", False)),
            })
        return out

    # Scene params (stateless)
    def list_params(self, *, id: int):
        return self._c.list_params(id=int(id))

    def get_values(self, *, id: int, json_keys: list[str] | None = None) -> dict:
        return self._c.get_param_values(id=int(id), json_keys=json_keys)

    def validate_apply(self, set_params: list[dict]) -> dict:
        res = self._c.validate_apply(set_params)
        if not res.get("ok", False):
            return res
        return res

    def apply_params(self, set_params: list[dict]) -> None:
        ok = self._c.apply_params(set_params)
        if not ok:
            raise RuntimeError("ApplySceneParams failed")

    def save_scene(self, path: str | Path) -> None:
        ok = self._c.save_scene(Path(path))
        if not ok:
            raise RuntimeError("SaveScene failed")

    def make_alias(self, ids: list[int]) -> dict:
        """Create alias objects for the given source ids using the live scene.

        Returns a dict {"ok": bool, "aliases": [{"src_id", "alias_id"}], "error"?: str}.
        """
        return self._c.make_alias(ids)

    # Timeline (selected helpers)
    def list_keys(self, *, id: int, json_key: str | None = None, include_values: bool = False):
        return self._c.list_keys(id=int(id), json_key=json_key, include_values=include_values)

    def set_time(self, seconds: float, cancel_rendering: bool = False) -> None:
        ok = self._c.set_time(seconds, cancel_rendering=cancel_rendering)
        if not ok:
            raise RuntimeError("SetTime failed")
