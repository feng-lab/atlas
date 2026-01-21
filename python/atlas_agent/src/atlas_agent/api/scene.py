"""Typed wrappers around SceneClient for Python scripts.

These helpers raise exceptions on failure and return native Python types.
"""

from pathlib import Path

from ..scene_rpc import SceneClient


class SceneAPI:
    def __init__(self, client: SceneClient):
        self._c = client

    # Load
    def load_sources(
        self,
        sources: list[str],
        *,
        network_timeout_sec: float | None = None,
        set_visible: bool = True,
        task_timeout_sec: float = 120.0,
        wait_ready: bool = True,
    ) -> dict:
        return self._c.load_sources(
            sources,
            network_timeout_sec=network_timeout_sec,
            set_visible=set_visible,
            task_timeout_sec=task_timeout_sec,
            wait_ready=wait_ready,
        )

    def start_load_task(
        self,
        sources: list[str],
        *,
        network_timeout_sec: float | None = None,
        set_visible: bool = True,
    ) -> int:
        return self._c.start_load_task(
            sources,
            network_timeout_sec=network_timeout_sec,
            set_visible=set_visible,
        )

    def wait_task(
        self,
        task_id: int,
        *,
        timeout_sec: float = 30.0,
        poll_interval_sec: float = 0.2,
    ) -> dict:
        return self._c.wait_task(
            task_id,
            timeout_sec=timeout_sec,
            poll_interval_sec=poll_interval_sec,
        )

    def list_objects(self) -> list[dict]:
        resp = self._c.list_objects()
        out = []
        for o in getattr(resp, "objects", []):
            out.append(
                {
                    "id": int(getattr(o, "id", 0)),
                    "type": getattr(o, "type", ""),
                    "name": getattr(o, "name", ""),
                    "path": getattr(o, "path", ""),
                    "visible": bool(getattr(o, "visible", False)),
                }
            )
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
    def list_keys(
        self,
        *,
        animation_id: int,
        id: int,
        json_key: str | None = None,
        include_values: bool = False,
    ):
        return self._c.list_keys(
            animation_id=int(animation_id),
            target_id=int(id),
            json_key=json_key,
            include_values=include_values,
        )

    def set_time(
        self, *, animation_id: int, seconds: float, cancel_rendering: bool = False
    ) -> None:
        ok = self._c.set_time(
            animation_id=int(animation_id),
            seconds=float(seconds),
            cancel_rendering=cancel_rendering,
        )
        if not ok:
            raise RuntimeError("SetTime failed")
