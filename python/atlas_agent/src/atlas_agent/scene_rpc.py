import json
import logging
import os
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Optional

import grpc  # type: ignore
import pkg_resources  # type: ignore
from google.protobuf import struct_pb2  # type: ignore
from google.protobuf.json_format import MessageToDict  # type: ignore
from grpc_tools import protoc  # type: ignore

from .repo import find_repo_root  # type: ignore


def _compile_proto(proto_path: Path, out_dir: Path) -> None:
    # Standard well-known types (e.g., google/protobuf/struct.proto) live here
    try:
        std_include = pkg_resources.resource_filename("grpc_tools", "_proto")
    except Exception:
        std_include = None
    args = [
        "protoc",
        f"-I{proto_path.parent}",
        *( [f"-I{std_include}"] if std_include else [] ),
        f"--python_out={out_dir}",
        f"--grpc_python_out={out_dir}",
        str(proto_path),
    ]
    if protoc.main(args) != 0:
        raise RuntimeError("Failed to compile scene.proto stubs")


def _load_stubs(repo_root: Path):
    proto = repo_root / "src/protos/scene.proto"
    if not proto.exists():
        raise FileNotFoundError(f"scene.proto not found at {proto}")
    td = tempfile.TemporaryDirectory()
    out_dir = Path(td.name)
    _compile_proto(proto, out_dir)
    sys.path.insert(0, str(out_dir))
    scene_pb2 = __import__("scene_pb2")
    scene_pb2_grpc = __import__("scene_pb2_grpc")
    return td, scene_pb2, scene_pb2_grpc


@dataclass
class SceneClient:
    address: str = "localhost:50051"
    _tmpdir: Any = None
    _pb2: Any = None
    _pb2_grpc: Any = None
    _channel: Any = None
    _stub: Any = None

    def __post_init__(self):
        # Configure logger (default INFO to stdout) once
        self._logger = logging.getLogger("atlas_agent.rpc")
        if not self._logger.handlers:
            h = logging.StreamHandler()
            fmt = logging.Formatter(fmt="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
                                     datefmt="%H:%M:%S")
            h.setFormatter(fmt)
            self._logger.addHandler(h)
            # Default to WARNING to reduce noise; override via ATLAS_AGENT_LOG
            lvl = os.environ.get("ATLAS_AGENT_LOG", "WARNING").upper()
            level = getattr(logging, lvl, logging.WARNING)
            self._logger.setLevel(level)
            self._logger.propagate = False
        # Discover repo root by sentinels to avoid path-depth assumptions
        repo_root = find_repo_root() or Path.cwd()
        self._tmpdir, self._pb2, self._pb2_grpc = _load_stubs(repo_root)
        self._channel = grpc.insecure_channel(self.address)
        self._stub = self._pb2_grpc.SceneStub(self._channel)

    def engine_ready(self) -> bool:
        req = self._pb2.Empty()
        resp = self._stub.EngineReady(req)
        self._log_rpc("EngineReady", req, resp)
        return bool(resp.ok)

    def ensure_view(self) -> bool:
        # Ask GUI to ensure a 3D window/canvas exists, then wait for readiness
        req = self._pb2.Empty()
        resp = self._stub.Ensure3DWindow(req)
        self._log_rpc("Ensure3DWindow", req, resp)
        # Even if ok, give EngineReady a chance to see the engine
        try:
            return self.engine_ready()
        except Exception:
            return False

    def _log_rpc(self, name: str, req: Any, resp: Any | None = None, error: Exception | None = None):
        def _safe(obj):
            try:
                return str(obj)
            except Exception:
                return f"<{type(obj).__name__}>"
        # Avoid spamming on frequent getters unless log level is DEBUG
        noisy = {"EngineReady", "Ensure3DWindow", "ListParams", "ListKeys", "GetTime", "Ping"}
        if error is not None:
            self._logger.error("%s req=%s error=%s", name, _safe(req), error)
        else:
            if name in noisy and self._logger.level > logging.DEBUG:
                self._logger.debug("%s ok", name)
            else:
                self._logger.info("%s req=%s resp=%s", name, _safe(req), _safe(resp))

    # Basic
    def ping(self) -> bool:
        req = self._pb2.PingRequest()
        resp = self._stub.Ping(req)
        self._log_rpc("Ping", req, resp)
        return bool(resp.ok)

    def load_files(self, files: Iterable[str]):
        exp: list[str] = []
        missing: list[str] = []
        for s in files:
            t = os.path.expanduser(os.path.expandvars(str(s)))
            # Normalize obvious Windows separators on POSIX (best-effort)
            if os.name != "nt" and "\\" in t and ":" in t[:3]:
                t = t.replace("\\", "/")
                # strip drive prefix like C:
                if ":" in t:
                    t = t.split(":", 1)[1]
            if not os.path.isabs(t):
                # leave relative paths as-is; let caller decide base dir
                pass
            if not os.path.exists(t):
                missing.append(t)
            else:
                exp.append(t)
        if missing:
            self._logger.error("LoadFiles: missing paths: %s", missing)
            raise FileNotFoundError(f"Not found: {missing}")
        req = self._pb2.FileList(files=exp)
        resp = self._stub.LoadFiles(req)
        self._log_rpc("LoadFiles", req, resp)
        return resp

    def ensure_loaded(self, files: Iterable[str]):
        """Idempotently ensure files are loaded: skip any that are already present.
        Returns a dict: {loaded: [...], skipped: [...], objects: [...]}.
        """
        # Snapshot current objects and build a set of normalized existing paths
        objs = self.list_objects()
        existing_paths = set()
        for o in getattr(objs, "objects", []):
            try:
                p = str(getattr(o, "path", "") or "").strip()
                if p:
                    existing_paths.add(os.path.normpath(os.path.expanduser(os.path.expandvars(p))))
            except Exception:
                pass
        to_load: list[str] = []
        skipped: list[str] = []
        expanded: list[str] = []
        for s in files:
            t = os.path.expanduser(os.path.expandvars(str(s)))
            if os.name != "nt" and "\\" in t and ":" in t[:3]:
                t = t.replace("\\", "/")
                if ":" in t:
                    t = t.split(":", 1)[1]
            t_norm = os.path.normpath(t)
            expanded.append(t_norm)
            if t_norm in existing_paths:
                skipped.append(t_norm)
            else:
                to_load.append(t_norm)
        loaded: list[str] = []
        if to_load:
            # Validate existence before sending to Atlas
            missing = [p for p in to_load if not os.path.exists(p)]
            if missing:
                # Do not raise; return missing so the agent can iterate
                self._logger.warning("ensure_loaded: missing paths skipped: %s", missing)
                to_load = [p for p in to_load if p not in missing]
            if to_load:
                req = self._pb2.FileList(files=to_load)
                resp = self._stub.LoadFiles(req)
                self._log_rpc("LoadFiles", req, resp)
                loaded = list(to_load)
                objs = resp
        # Return a compact summary structure
        out_objs = []
        for o in getattr(objs, "objects", []):
            out_objs.append({
                "id": int(getattr(o, "id", 0)),
                "type": getattr(o, "type", ""),
                "name": getattr(o, "name", ""),
                "path": getattr(o, "path", ""),
                "visible": bool(getattr(o, "visible", False)),
            })
        return {"loaded": loaded, "skipped": skipped, "objects": out_objs}

    # Snapshot current timeline keys for facts/verification
    def timeline_snapshot(self) -> dict:
        """Deprecated in favor of scene_facts(). Left for backward compatibility."""
        return self.scene_facts().get("keys", {})

    def scene_facts(self, *, include_values: bool = False, include_scene_values: bool = True) -> dict:
        """Return a structured snapshot of the scene for verification/description.

        Shape:
          {
            "objects_list": [{id, type, name, path, visible}, ...],
            "keys": {
              "camera": [times...],
              "objects": { id: { json_key: ([times...] | [ {time, value}? ]) } }
            },
            "scene_values?": { id: { json_key: value, ... }, ... }
          }
        """
        facts: dict[str, Any] = {"objects_list": [], "keys": {"camera": [], "objects": {}}}
        try:
            # Objects list
            objs = self.list_objects()
            for o in getattr(objs, "objects", []):
                facts["objects_list"].append({
                    "id": int(getattr(o, "id", 0)),
                    "type": getattr(o, "type", ""),
                    "name": getattr(o, "name", ""),
                    "path": getattr(o, "path", ""),
                    "visible": bool(getattr(o, "visible", False)),
                })
            # Camera keys
            lr = self.list_keys(id=0)
            cam_times = [k.time for k in getattr(lr, "keys", [])]
            if cam_times:
                facts["keys"]["camera"] = sorted(cam_times)
        except Exception:
            pass
        # Objects and per-param keys
        try:
            for o in facts["objects_list"]:
                oid = int(o.get("id", 0))
                try:
                    pl = self.list_params(id=oid)
                except Exception:
                    continue
                obj_map: dict[str, list[float]] = {}
                for p in getattr(pl, "params", []):
                    jk = getattr(p, "json_key", "")
                    if not jk:
                        continue
                    try:
                        lr = self.list_keys(id=oid, json_key=jk, include_values=bool(include_values))
                        if include_values:
                            entries = []
                            for k in getattr(lr, "keys", []) or []:
                                try:
                                    vj = getattr(k, "value_json", "") or ""
                                    val = __import__("json").loads(vj) if vj else None
                                except Exception:
                                    val = None
                                entries.append({"time": float(getattr(k, "time", 0.0)), **({"value": val} if val is not None else {})})
                            if entries:
                                # Sort by time
                                obj_map[jk] = sorted(entries, key=lambda e: e.get("time", 0.0))
                        else:
                            times = [k.time for k in getattr(lr, "keys", [])]
                            if times:
                                obj_map[jk] = sorted(times)
                    except Exception:
                        continue
                if obj_map:
                    facts["keys"]["objects"][str(oid)] = obj_map
        except Exception:
            pass
        # Optional: include current scene values for key engine scopes and objects (all params)
        if include_scene_values:
            try:
                sv: dict[str, dict[str, Any]] = {}
                # Engine scopes (stateless): 0=camera, 1=background, 2=axis, 3=global
                for scope_id in (0, 1, 2, 3):
                    try:
                        vals = self.get_param_values(id=int(scope_id), json_keys=[])
                        sv[str(scope_id)] = vals
                    except Exception:
                        continue
                for o in facts.get("objects_list", []) or []:
                    oid = int(o.get("id", 0))
                    try:
                        # When json_keys omitted, GetParamValues returns all values for the id
                        vals = self.get_param_values(id=oid, json_keys=[])
                        sv[str(oid)] = vals
                    except Exception:
                        continue
                facts["scene_values"] = sv
            except Exception:
                pass
        return facts

    def list_objects(self):
        req = self._pb2.Empty()
        resp = self._stub.ListObjects(req)
        self._log_rpc("ListObjects", req, resp)
        return resp

    # Animation/timeline
    def ensure_animation(self) -> bool:
        # Ensure the rendering engine exists (open 3D view if necessary)
        self.ensure_view()
        # Only create animation when at least one visual object is loaded
        try:
            objs = self.list_objects()
            has_visual = False
            for o in getattr(objs, "objects", []):
                t = (getattr(o, "type", "") or "").lower()
                if t and "animation3d" not in t:
                    has_visual = True
                    break
            if not has_visual:
                self._logger.info("EnsureAnimation: skipped (no visual objects loaded yet)")
                return False
        except Exception:
            pass
        req = self._pb2.EnsureAnimationRequest()
        resp = self._stub.EnsureAnimation(req)
        self._log_rpc("EnsureAnimation", req, resp)
        return bool(getattr(resp, "ok", False))

    def set_duration(self, seconds: float) -> bool:
        req = self._pb2.SetDurationRequest(duration=seconds)
        resp = self._stub.SetDuration(req)
        self._log_rpc("SetDuration", req, resp)
        return resp.ok

    def set_time(self, seconds: float, cancel_rendering: bool = False) -> bool:
        req = self._pb2.SetTimeRequest(seconds=seconds, cancel_rendering=cancel_rendering)
        resp = self._stub.SetTime(req)
        self._log_rpc("SetTime", req, resp)
        return resp.ok

    def save_animation(self, path: Path) -> bool:
        req = self._pb2.SaveRequest(path=str(path))
        resp = self._stub.SaveAnimation(req)
        self._log_rpc("SaveAnimation", req, resp)
        return resp.ok

    # Camera helpers
    def camera_fit(self, ids: Optional[list[int]] = None, all: bool = False, after_clipping: bool = False, min_radius: float = 0.0) -> list[dict]:
        self.ensure_view()
        req = self._pb2.CameraFitRequest(ids=ids or [], all=all, after_clipping=after_clipping, min_radius=min_radius)
        resp = self._stub.CameraFit(req)
        self._log_rpc("CameraFit", req, resp)
        return [MessageToDict(v) for v in resp.values]

    def camera_orbit(self, ids: Optional[list[int]] = None, axis: str = "y", degrees: float = 360.0) -> list[dict]:
        self.ensure_view()
        req = self._pb2.CameraOrbitSuggestRequest(ids=ids or [], axis=axis, degrees=float(degrees))
        resp = self._stub.CameraOrbitSuggest(req)
        self._log_rpc("CameraOrbitSuggest", req, resp)
        return [MessageToDict(v) for v in resp.values]

    def camera_dolly(self, ids: Optional[list[int]] = None, start_dist: float = 0.0, end_dist: float = 0.0) -> list[dict]:
        self.ensure_view()
        req = self._pb2.CameraDollySuggestRequest(ids=ids or [], start_dist=start_dist, end_dist=end_dist)
        resp = self._stub.CameraDollySuggest(req)
        self._log_rpc("CameraDollySuggest", req, resp)
        return [MessageToDict(v) for v in resp.values]

    # Camera operators (UI parity)
    def camera_focus(self, ids: Optional[list[int]] = None, after_clipping: bool = True, min_radius: float = 0.0) -> dict:
        self.ensure_view()
        req = self._pb2.CameraFocusRequest(ids=ids or [], after_clipping=bool(after_clipping), min_radius=float(min_radius))
        resp = self._stub.CameraFocus(req)
        self._log_rpc("CameraFocus", req, resp)
        vals = [MessageToDict(v) for v in resp.values]
        return vals[0] if vals else {}

    def camera_point_to(self, ids: Optional[list[int]] = None, after_clipping: bool = True) -> dict:
        self.ensure_view()
        req = self._pb2.CameraPointToRequest(ids=ids or [], after_clipping=bool(after_clipping))
        resp = self._stub.CameraPointTo(req)
        self._log_rpc("CameraPointTo", req, resp)
        vals = [MessageToDict(v) for v in resp.values]
        return vals[0] if vals else {}

    def camera_rotate(self, op: str, degrees: float = 90.0, base_value: Optional[dict] = None) -> dict:
        self.ensure_view()
        bv = None
        if base_value is not None:

            def _to_value(py: Any) -> struct_pb2.Value:
                v = struct_pb2.Value()
                if py is None:
                    v.null_value = 0
                elif isinstance(py, bool):
                    v.bool_value = bool(py)
                elif isinstance(py, (int, float)) and not isinstance(py, bool):
                    v.number_value = float(py)
                elif isinstance(py, str):
                    v.string_value = py
                elif isinstance(py, (list, tuple)):
                    lv = struct_pb2.ListValue()
                    for item in py:
                        lv.values.append(_to_value(item))
                    v.list_value.CopyFrom(lv)
                elif isinstance(py, dict):
                    st = struct_pb2.Struct()
                    for k, val in py.items():
                        st.fields[k].CopyFrom(_to_value(val))
                    v.struct_value.CopyFrom(st)
                else:
                    v.string_value = str(py)
                return v
            bv = _to_value(base_value)
        req = self._pb2.CameraRotateRequest(op=str(op), degrees=float(degrees), base_value=bv if bv is not None else None)
        resp = self._stub.CameraRotate(req)
        self._log_rpc("CameraRotate", req, resp)
        vals = [MessageToDict(v) for v in resp.values]
        return vals[0] if vals else {}

    def camera_reset_view(self, mode: str = "RESET", ids: Optional[list[int]] = None, after_clipping: bool = True, min_radius: float = 0.0) -> dict:
        self.ensure_view()
        req = self._pb2.CameraResetViewRequest(mode=str(mode), ids=ids or [], after_clipping=bool(after_clipping), min_radius=float(min_radius))
        resp = self._stub.CameraResetView(req)
        self._log_rpc("CameraResetView", req, resp)
        vals = [MessageToDict(v) for v in resp.values]
        return vals[0] if vals else {}

    # Typed camera planning and validation
    def fit_candidates(self) -> list[int]:
        self.ensure_view()
        resp = self._stub.FitCandidates(self._pb2.Empty())
        self._log_rpc("FitCandidates", self._pb2.Empty(), resp)
        return [int(v) for v in getattr(resp, "ids", [])]

    def camera_solve(
        self,
        *,
        mode: str,
        ids: Optional[list[int]] = None,
        t0: float = 0.0,
        t1: float = 0.0,
        constraints: Optional[dict] = None,
        params: Optional[dict] = None,
    ) -> list[dict]:
        self.ensure_view()
        # Build Structs for constraints/params
        cons = None
        if constraints:
            cons = self._pb2.CameraConstraints(
                keep_visible=bool(constraints.get("keep_visible", True)),
                margin=float(constraints.get("margin", 0.0)),
                min_coverage=float(constraints.get("min_coverage", 0.95)),
                fov_policy=str(constraints.get("fov_policy", "fixed")),
            )
        st = None
        if params:

            def _to_value(py: Any) -> struct_pb2.Value:
                v = struct_pb2.Value()
                if py is None:
                    v.null_value = 0
                elif isinstance(py, bool):
                    v.bool_value = bool(py)
                elif isinstance(py, (int, float)) and not isinstance(py, bool):
                    v.number_value = float(py)
                elif isinstance(py, str):
                    v.string_value = py
                elif isinstance(py, (list, tuple)):
                    lv = struct_pb2.ListValue()
                    for item in py:
                        lv.values.append(_to_value(item))
                    v.list_value.CopyFrom(lv)
                elif isinstance(py, dict):
                    st2 = struct_pb2.Struct()
                    for k2, val in py.items():
                        st2.fields[k2].CopyFrom(_to_value(val))
                    v.struct_value.CopyFrom(st2)
                else:
                    v.string_value = str(py)
                return v

            st = struct_pb2.Struct()
            for k, v in params.items():
                st.fields[k].CopyFrom(_to_value(v))
        req = self._pb2.CameraSolveRequest(
            mode=str(mode), ids=ids or [], t0=float(t0), t1=float(t1),
            constraints=cons if cons is not None else None,
            params=st if st is not None else None,
        )
        resp = self._stub.CameraSolve(req)
        self._log_rpc("CameraSolve", req, resp)
        out: list[dict] = []
        for k in getattr(resp, "keys", []):
            out.append({"time": float(getattr(k, "time", 0.0)), "value": MessageToDict(getattr(k, "value"))})
        return out

    def camera_validate(
        self,
        *,
        ids: Optional[list[int]] = None,
        times: list[float],
        values: list[dict],
        constraints: Optional[dict] = None,
        policies: Optional[dict] = None,
    ) -> dict:
        self.ensure_view()
        cons = None
        if constraints:
            cons = self._pb2.CameraConstraints(
                keep_visible=bool(constraints.get("keep_visible", True)),
                margin=float(constraints.get("margin", 0.0)),
                min_coverage=float(constraints.get("min_coverage", 0.95)),
                fov_policy=str(constraints.get("fov_policy", "fixed")),
            )
        pol = None
        if policies:
            pol = self._pb2.CameraPolicies(
                adjust_fov=bool(policies.get("adjust_fov", False)),
                adjust_distance=bool(policies.get("adjust_distance", False)),
                adjust_clipping=bool(policies.get("adjust_clipping", False)),
            )
        # Convert dicts to protobuf Values
        def _to_value(py: Any) -> struct_pb2.Value:
            v = struct_pb2.Value()
            if py is None:
                v.null_value = 0
            elif isinstance(py, bool):
                v.bool_value = bool(py)
            elif isinstance(py, (int, float)) and not isinstance(py, bool):
                v.number_value = float(py)
            elif isinstance(py, str):
                v.string_value = py
            elif isinstance(py, (list, tuple)):
                lv = struct_pb2.ListValue()
                for item in py:
                    lv.values.append(_to_value(item))
                v.list_value.CopyFrom(lv)
            elif isinstance(py, dict):
                st = struct_pb2.Struct()
                for k, val in py.items():
                    st.fields[k].CopyFrom(_to_value(val))
                v.struct_value.CopyFrom(st)
            else:
                v.string_value = str(py)
            return v
        req = self._pb2.CameraValidateRequest(
            ids=ids or [],
            times=[float(t) for t in times],
            values=[_to_value(v) for v in values],
            constraints=cons if cons is not None else None,
            policies=pol if pol is not None else None,
        )
        resp = self._stub.CameraValidate(req)
        self._log_rpc("CameraValidate", req, resp)
        results: list[dict] = []
        for r in getattr(resp, "results", []):
            row: dict[str, Any] = {
                "time": float(getattr(r, "time", 0.0)),
                "within_frame": bool(getattr(r, "within_frame", False)),
                "coverage": float(getattr(r, "coverage", 0.0)),
                "adjusted": bool(getattr(r, "adjusted", False)),
                "reason": str(getattr(r, "reason", "")),
            }
            try:
                if getattr(r, "adjusted", False):
                    row["adjusted_value"] = MessageToDict(getattr(r, "adjusted_value"))
            except Exception:
                pass
            results.append(row)
        return {"ok": bool(getattr(resp, "ok", False)), "results": results}

    # Keys
    def set_key_camera(self, time: float, easing: str, value: Any) -> bool:
        # Ensure engine/view exists before setting camera keys
        self.ensure_view()
        py = value
        def _to_value(py: Any) -> struct_pb2.Value:
            v = struct_pb2.Value()
            if py is None:
                v.null_value = 0
            elif isinstance(py, bool):
                v.bool_value = bool(py)
            elif isinstance(py, (int, float)) and not isinstance(py, bool):
                v.number_value = float(py)
            elif isinstance(py, str):
                v.string_value = py
            elif isinstance(py, (list, tuple)):
                lv = struct_pb2.ListValue()
                for item in py:
                    lv.values.append(_to_value(item))
                v.list_value.CopyFrom(lv)
            elif isinstance(py, dict):
                st = struct_pb2.Struct()
                for k, val in py.items():
                    st.fields[k].CopyFrom(_to_value(val))
                v.struct_value.CopyFrom(st)
            else:
                v.string_value = str(py)
            return v
        v = _to_value(py)
        req = self._pb2.SetKeyRequest(id=0, time=time, easing=easing, value=v)
        resp = self._stub.SetKey(req)
        self._log_rpc("SetKey(camera)", req, resp)
        return resp.ok

    def list_params(self, *, id: int):
        # Ensure engine is ready (and open a 3D window if necessary)
        self.ensure_view()
        req = self._pb2.ListParamsRequest(id=int(id))
        resp = self._stub.ListParams(req)
        self._log_rpc("ListParams", req, resp)
        return resp

    def clear_keys(self, *, id: int, json_key: Optional[str] = None) -> bool:
        self.ensure_view()
        req = self._pb2.ClearKeysRequest(id=int(id), json_key=json_key or "")
        resp = self._stub.ClearKeys(req)
        self._log_rpc("ClearKeys", req, resp)
        return resp.ok

    # Non-camera parameter key operations (id-based)
    def set_key_param(self, *, id: int, json_key: str, time: float, easing: str = "Linear", value: Any) -> bool:
        def _to_value(py: Any) -> struct_pb2.Value:
            v = struct_pb2.Value()
            if py is None:
                v.null_value = 0
            elif isinstance(py, bool):
                v.bool_value = bool(py)
            elif isinstance(py, (int, float)) and not isinstance(py, bool):
                v.number_value = float(py)
            elif isinstance(py, str):
                v.string_value = py
            elif isinstance(py, (list, tuple)):
                lv = struct_pb2.ListValue()
                for item in py:
                    lv.values.append(_to_value(item))
                v.list_value.CopyFrom(lv)
            elif isinstance(py, dict):
                st = struct_pb2.Struct()
                for k, val in py.items():
                    st.fields[k].CopyFrom(_to_value(val))
                v.struct_value.CopyFrom(st)
            else:
                v.string_value = str(py)
            return v
        v = _to_value(value)
        req = self._pb2.SetKeyRequest(id=int(id), json_key=json_key, time=float(time), easing=easing, value=v)
        resp = self._stub.SetKey(req)
        self._log_rpc("SetKey(param)", req, resp)
        return resp.ok

    def remove_key(self, *, id: int, json_key: str, time: float) -> bool:
        self.ensure_view()
        req = self._pb2.RemoveKeyRequest(id=int(id), json_key=json_key, time=float(time))
        resp = self._stub.RemoveKey(req)
        self._log_rpc("RemoveKey", req, resp)
        return resp.ok

    def batch(self, *, set_keys: list[dict] | None = None, remove_keys: list[dict] | None = None, commit: bool = True) -> bool:
        # Ensure engine/view exists before batch operations
        self.ensure_view()
        set_keys = set_keys or []
        remove_keys = remove_keys or []
        if not set_keys and not remove_keys:
            self._logger.error("Batch: refusing to execute with empty set/remove")
            return False
        # Helper to convert native Python values to google.protobuf.Value
        def _to_value(py: Any) -> struct_pb2.Value:
            v = struct_pb2.Value()
            if py is None:
                v.null_value = 0
            elif isinstance(py, bool):
                v.bool_value = bool(py)
            elif isinstance(py, (int, float)) and not isinstance(py, bool):
                v.number_value = float(py)
            elif isinstance(py, str):
                v.string_value = py
            elif isinstance(py, (list, tuple)):
                lv = struct_pb2.ListValue()
                for item in py:
                    lv.values.append(_to_value(item))
                v.list_value.CopyFrom(lv)
            elif isinstance(py, dict):
                st = struct_pb2.Struct()
                for k, val in py.items():
                    st.fields[k].CopyFrom(_to_value(val))
                v.struct_value.CopyFrom(st)
            else:
                v.string_value = str(py)
            return v
        # Construct protobuf requests (id-only requests)
        pb_set = []
        for s in set_keys:
            id = int(s.get("id", -1))
            val = s.get("value")
            if id == 0:
                pb_set.append(self._pb2.SetKeyRequest(id=id, time=float(s["time"]), easing=str(s.get("easing", "Linear")), value=_to_value(val)))
            else:
                pb_set.append(self._pb2.SetKeyRequest(id=id, json_key=str(s["json_key"]), time=float(s["time"]), easing=str(s.get("easing", "Linear")), value=_to_value(val)))
        pb_rem = []
        for r in remove_keys:
            id = int(r.get("id", -1))
            pb_rem.append(self._pb2.RemoveKeyRequest(id=id, json_key=str(r["json_key"]), time=float(r["time"])) )
        # Human-friendly payload log (sanitized)
        def _summarize_keys(keys: list[dict]):
            out: list[dict] = []
            for k in keys:
                id = int(k.get("id", -1))
                jk = k.get("json_key")
                t = float(k.get("time", 0.0))
                ez = k.get("easing", "")
                val = k.get("value")
                if not isinstance(val, str):
                    try:
                        val = json.dumps(val)
                    except Exception:
                        val = str(val)
                # Log full payloads for transparency (no truncation)
                out.append({"id": id, "json_key": jk, "time": t, "easing": ez, "value": val})
            return out
        self._logger.info("Batch(payload) %s", json.dumps({
            "commit": bool(commit),
            "set_keys": _summarize_keys(set_keys),
            "remove_keys": _summarize_keys(remove_keys),
        }))
        req = self._pb2.BatchRequest(set_keys=pb_set, remove_keys=pb_rem, commit=bool(commit))
        resp = self._stub.Batch(req)
        self._log_rpc("Batch", req, resp)

        # Verify that keys now exist at requested times; log discrepancies.
        try:
            missing: list[dict] = []
            for s in set_keys:
                id = int(s.get("id", -1))
                lr = self.list_keys(id=int(id), json_key=str(s.get("json_key", "")))
                target_times = [k.time for k in getattr(lr, "keys", [])]
                want_t = float(s.get("time", 0.0))
                if not any(abs(want_t - t) < 1e-6 for t in target_times):
                    missing.append({"id": id, "json_key": s.get("json_key"), "time": want_t})
            if missing:
                self._logger.warning("Batch verify: missing keys at times: %s", json.dumps(missing))
            else:
                self._logger.info("Batch verify: all keys present (%d)", len(set_keys))
        except Exception as e:
            self._log_rpc("BatchVerify", req, None, error=e)
        return bool(resp.ok)

    def list_keys(self, *, id: int, json_key: Optional[str] = None, include_values: bool = False):
        # Ensure engine/view exists. Do not force-create animation here to avoid
        # creating empty animations before objects are loaded.
        self.ensure_view()
        req = self._pb2.ListKeysRequest(id=int(id), json_key=json_key or "", include_values=bool(include_values))
        resp = self._stub.ListKeys(req)
        self._log_rpc("ListKeys", req, resp)
        return resp

    def get_time(self):
        req = self._pb2.Empty()
        resp = self._stub.GetTime(req)
        self._log_rpc("GetTime", req, resp)
        return resp

    def set_visibility(self, ids: list[int], on: bool) -> bool:
        self.ensure_view()
        req = self._pb2.VisibilityRequest(ids=ids, on=bool(on))
        resp = self._stub.SetVisibility(req)
        self._log_rpc("SetVisibility", req, resp)
        return resp.ok

    def make_alias(self, ids: list[int]) -> dict:
        """Create alias objects for the given source ids.

        Returns: {"ok": bool, "aliases": [{"src_id", "alias_id"}], "error"?: str}
        """
        self.ensure_view()
        req = self._pb2.MakeAliasRequest(ids=[int(i) for i in ids or []])
        resp = self._stub.MakeAlias(req)
        self._log_rpc("MakeAlias", req, resp)
        aliases: list[dict[str, int]] = []
        for r in getattr(resp, "aliases", []):
            aliases.append(
                {
                    "src_id": int(getattr(r, "src_id", 0)),
                    "alias_id": int(getattr(r, "alias_id", 0)),
                }
            )
        error = ""
        try:
            error = str(getattr(resp, "error", "") or "")
        except Exception:
            error = ""
        ok = bool(getattr(resp, "ok", bool(aliases)))
        out: dict[str, Any] = {"ok": ok, "aliases": aliases}
        if not ok and error:
            out["error"] = error
        return out

    # Placement roles removed by design: prefer list_params/capabilities/schema

    # Scene (stateless) parameter ops
    def get_param_values(self, *, id: int, json_keys: Optional[list[str]] = None) -> dict:
        req = self._pb2.GetParamValuesRequest(id=int(id), json_keys=json_keys or [])
        resp = self._stub.GetParamValues(req)
        self._log_rpc("GetParamValues", req, resp)
        # Convert Struct/Value map to native dict
        out: dict[str, Any] = {}
        for k, v in getattr(resp, "values", {}).items():
            # Use protobuf json MessageToDict to convert google.protobuf.Value → python
            out[k] = MessageToDict(v)
        return out

    def validate_apply(self, set_params: list[dict]) -> dict:
        """Validate a batch of scene parameter assignments.

        Each item: { id: int, json_key: str, value: any }
        Returns { ok: bool, results: [{json_key, ok, reason?, normalized_value?}] }
        """
        def _to_value(py: Any) -> struct_pb2.Value:
            v = struct_pb2.Value()
            if py is None:
                v.null_value = 0
            elif isinstance(py, bool):
                v.bool_value = bool(py)
            elif isinstance(py, (int, float)) and not isinstance(py, bool):
                v.number_value = float(py)
            elif isinstance(py, str):
                v.string_value = py
            elif isinstance(py, (list, tuple)):
                lv = struct_pb2.ListValue()
                for item in py:
                    lv.values.append(_to_value(item))
                v.list_value.CopyFrom(lv)
            elif isinstance(py, dict):
                st = struct_pb2.Struct()
                for k, val in py.items():
                    st.fields[k].CopyFrom(_to_value(val))
                v.struct_value.CopyFrom(st)
            else:
                v.string_value = str(py)
            return v
        pb_items = []
        for it in set_params:
            id = int(it.get("id"))
            pb_items.append(self._pb2.SetParam(id=id, json_key=str(it["json_key"]), value=_to_value(it.get("value"))))
        req = self._pb2.ValidateSceneParamsRequest(set_params=pb_items)
        resp = self._stub.ValidateSceneParams(req)
        self._log_rpc("ValidateSceneParams", req, resp)
        results: list[dict] = []
        for r in getattr(resp, "results", []):
            entry: dict[str, Any] = {"json_key": getattr(r, "json_key", ""), "ok": bool(getattr(r, "ok", False))}
            reason = getattr(r, "reason", "")
            if reason:
                entry["reason"] = reason
            nv = getattr(r, "normalized_value", None)
            if nv is not None:
                entry["normalized_value"] = MessageToDict(nv)
            results.append(entry)
        return {"ok": bool(getattr(resp, "ok", False)), "results": results}

    def apply_params(self, set_params: list[dict]) -> bool:
        """Apply a batch of scene parameter assignments atomically (no time/easing)."""
        def _to_value(py: Any) -> struct_pb2.Value:
            v = struct_pb2.Value()
            if py is None:
                v.null_value = 0
            elif isinstance(py, bool):
                v.bool_value = bool(py)
            elif isinstance(py, (int, float)) and not isinstance(py, bool):
                v.number_value = float(py)
            elif isinstance(py, str):
                v.string_value = py
            elif isinstance(py, (list, tuple)):
                lv = struct_pb2.ListValue()
                for item in py:
                    lv.values.append(_to_value(item))
                v.list_value.CopyFrom(lv)
            elif isinstance(py, dict):
                st = struct_pb2.Struct()
                for k, val in py.items():
                    st.fields[k].CopyFrom(_to_value(val))
                v.struct_value.CopyFrom(st)
            else:
                v.string_value = str(py)
            return v
        pb_items = []
        for it in set_params:
            id = int(it.get("id"))
            pb_items.append(self._pb2.SetParam(id=id, json_key=str(it["json_key"]), value=_to_value(it.get("value"))))
        req = self._pb2.ApplySceneParamsRequest(set_params=pb_items)
        resp = self._stub.ApplySceneParams(req)
        self._log_rpc("ApplySceneParams", req, resp)
        return bool(getattr(resp, "ok", False))

    def save_scene(self, path: Path) -> bool:
        req = self._pb2.SaveSceneRequest(path=str(path))
        resp = self._stub.SaveScene(req)
        self._log_rpc("SaveScene", req, resp)
        return bool(getattr(resp, "ok", False))

    # Cuts
    def cut_set_box(self, min_xyz: tuple[float, float, float], max_xyz: tuple[float, float, float], refit_camera: bool = False) -> bool:
        self.ensure_view()
        Vec3 = self._pb2.Vec3
        box = self._pb2.BBox(min=Vec3(x=min_xyz[0], y=min_xyz[1], z=min_xyz[2]),
                              max=Vec3(x=max_xyz[0], y=max_xyz[1], z=max_xyz[2]))
        req = self._pb2.CutSetRequest(box=box, refit_camera=refit_camera)
        return self._stub.CutSet(req).ok

    def cut_clear(self) -> bool:
        self.ensure_view()
        return self._stub.CutClear(self._pb2.Empty()).ok

    def cut_suggest_box(self, ids: Optional[list[int]] = None, margin: float = 0.0, after_clipping: bool = False):
        self.ensure_view()
        req = self._pb2.CutSuggestRequest(ids=ids or [], mode="box", margin=margin, after_clipping=after_clipping)
        resp = self._stub.CutSuggest(req)
        self._log_rpc("CutSuggest", req, resp)
        return resp
