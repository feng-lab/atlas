"""Typed camera helpers for Python scripts.

These wrappers prefer the typed planning/validation flow:
  - fit candidates → camera_solve (FIT/ORBIT/DOLLY/STATIC)
  - camera_validate with constraints/policies
  - write keys via client.set_key_camera or client.batch

They raise exceptions on failure and return Python-native dicts.
"""

from dataclasses import dataclass
from typing import Optional

from ..scene_rpc import SceneClient


@dataclass
class CameraAPI:
    client: SceneClient

    def fit_candidates(self) -> list[int]:
        return self.client.fit_candidates()

    def focus(self, ids: Optional[list[int]] = None, after_clipping: bool = True, min_radius: float = 0.0) -> dict:
        return self.client.camera_focus(ids=ids, after_clipping=after_clipping, min_radius=min_radius)

    def point_to(self, ids: Optional[list[int]] = None, after_clipping: bool = True) -> dict:
        return self.client.camera_point_to(ids=ids, after_clipping=after_clipping)

    def rotate(self, op: str, degrees: float = 90.0, base_value: Optional[dict] = None) -> dict:
        return self.client.camera_rotate(op=op, degrees=degrees, base_value=base_value)

    def reset_view(self, mode: str = "RESET", ids: Optional[list[int]] = None, after_clipping: bool = True, min_radius: float = 0.0) -> dict:
        return self.client.camera_reset_view(mode=mode, ids=ids, after_clipping=after_clipping, min_radius=min_radius)

    def solve(self, *, mode: str, ids: Optional[list[int]] = None, t0: float = 0.0, t1: float = 0.0, constraints: Optional[dict] = None, params: Optional[dict] = None) -> list[dict]:
        # Pass-through; ORBIT expects params { axis, degrees }
        p = dict(params or {})
        keys = self.client.camera_solve(mode=mode, ids=ids, t0=t0, t1=t1, constraints=constraints or {"keep_visible": True, "min_coverage": 0.95}, params=p)
        if not keys:
            raise RuntimeError("camera_solve returned no keys")
        return keys

    def validate(self, *, ids: list[int], times: list[float], values: Optional[list[dict]] = None, constraints: Optional[dict] = None, policies: Optional[dict] = None) -> dict:
        # values may be omitted; server fills by sampling current animation at times
        res = self.client.camera_validate(ids=ids, times=times, values=values or [], constraints=constraints or {"keep_visible": True, "min_coverage": 0.95}, policies=policies or {"adjust_fov": False, "adjust_distance": False, "adjust_clipping": False})
        return res

    def write_keys(self, *, keys: list[dict], easing: str = "Linear", commit: bool = True) -> None:
        set_keys = [{"id": 0, "time": float(k["time"]), "easing": easing, "value": k["value"]} for k in keys]
        ok = self.client.batch(set_keys=set_keys, remove_keys=[], commit=commit)
        if not ok:
            raise RuntimeError("Batch failed when writing camera keys")
