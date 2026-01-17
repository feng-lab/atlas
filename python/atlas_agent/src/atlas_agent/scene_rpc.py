import hashlib
import json
import logging
import os
import platform
import subprocess
import sys
import tempfile
import time
from contextlib import ExitStack
from dataclasses import dataclass
from importlib.resources import as_file, files
from pathlib import Path
from typing import Any, Iterable, Optional

import grpc  # type: ignore[import-untyped]
try:
    from jsonschema import Draft7Validator as JsonSchemaValidator  # type: ignore
    from jsonschema.exceptions import SchemaError as JsonSchemaError  # type: ignore
except Exception:  # pragma: no cover
    JsonSchemaValidator = None  # type: ignore[assignment]
    JsonSchemaError = Exception  # type: ignore[assignment]
from google.protobuf import empty_pb2, struct_pb2, wrappers_pb2  # type: ignore[import-untyped]
from google.protobuf.json_format import MessageToDict  # type: ignore[import-untyped]
from grpc_tools import protoc  # type: ignore[import-untyped]

from .discovery import compute_paths_from_atlas_dir, compute_protos_dir_from_atlas_dir, default_install_dirs

DEFAULT_RPC_LOG_LEVEL = logging.WARNING
DEFAULT_RPC_BOOTSTRAP_TIMEOUT_SEC = 1.0
DEFAULT_RPC_BOOTSTRAP_TOTAL_WAIT_SEC = 30.0
# How long we will wait for the 3D rendering engine to become ready after
# requesting a 3D window. This is distinct from "RPC bootstrap" (server up).
DEFAULT_ENGINE_READY_TOTAL_WAIT_SEC = 30.0
DEFAULT_ENGINE_READY_POLL_INTERVAL_SEC = 0.2
DEFAULT_ENGINE_READY_RPC_TIMEOUT_SEC = 10.0


def _expand_path(s: str) -> str:
    t = os.path.expanduser(os.path.expandvars(str(s)))
    # Normalize obvious Windows separators on POSIX (best-effort)
    if os.name != "nt" and "\\" in t and ":" in t[:3]:
        t = t.replace("\\", "/")
        # strip drive prefix like C:
        if ":" in t:
            t = t.split(":", 1)[1]
    return t


def _to_proto_value(py: Any) -> struct_pb2.Value:
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
            lv.values.append(_to_proto_value(item))
        v.list_value.CopyFrom(lv)
    elif isinstance(py, dict):
        st = struct_pb2.Struct()
        for k, val in py.items():
            st.fields[k].CopyFrom(_to_proto_value(val))
        v.struct_value.CopyFrom(st)
    else:
        v.string_value = str(py)
    return v


def _compile_proto(proto_path: Path, out_dir: Path) -> None:
    # Standard well-known types (e.g., google/protobuf/struct.proto) live here
    with ExitStack() as stack:
        std_include: Path | None = None
        try:
            std_include = stack.enter_context(
                as_file(files("grpc_tools").joinpath("_proto"))
            )
        except Exception:
            std_include = None
        args = [
            "protoc",
            f"-I{proto_path.parent}",
            *([f"-I{std_include}"] if std_include else []),
            f"--python_out={out_dir}",
            f"--grpc_python_out={out_dir}",
            str(proto_path),
        ]
        if protoc.main(args) != 0:
            raise RuntimeError(f"Failed to compile {proto_path.name} stubs")


def _load_stubs(atlas_dir: Path):
    """Compile Python gRPC stubs for the Scene service.

    Source of truth:
      - The running Atlas app bundle/resources (atlas_dir/Resources/protos/scene.proto)

    Rationale: the agent must match the exact running Atlas version; we do not
    fall back to monorepo protos to avoid drift.
    """
    with ExitStack() as stack:
        proto_path = compute_protos_dir_from_atlas_dir(atlas_dir) / "scene.proto"
        if not proto_path.exists():
            raise FileNotFoundError(
                "scene.proto not found in the running Atlas app bundle.\n"
                f"- expected: {proto_path}\n"
                "Rebuild/reinstall Atlas so it ships Resources/protos/scene.proto."
            )
        td = tempfile.TemporaryDirectory()
        out_dir = Path(td.name)
        _compile_proto(proto_path, out_dir)
        sys.path.insert(0, str(out_dir))
        scene_pb2 = __import__("scene_pb2")
        scene_pb2_grpc = __import__("scene_pb2_grpc")
        return td, scene_pb2, scene_pb2_grpc


def _try_get_atlas_dir_from_rpc(channel: Any, *, timeout_sec: float = 1.0) -> str | None:
    """Best-effort bootstrap: ask the running Atlas RPC server where it is installed.

    This uses a generic gRPC call with well-known protobuf types, so it does not
    require compiling scene.proto stubs first.
    """
    try:
        rpc = channel.unary_unary(
            "/atlas.rpc.Scene/GetAppLocation",
            request_serializer=empty_pb2.Empty.SerializeToString,
            response_deserializer=wrappers_pb2.StringValue.FromString,
        )
    except Exception:
        return None
    try:
        resp = rpc(empty_pb2.Empty(), timeout=float(timeout_sec))
    except Exception:
        return None
    try:
        val = str(getattr(resp, "value", "") or "").strip()
        return val if val else None
    except Exception:
        return None


def _wait_for_atlas_dir_from_rpc(
    channel: Any,
    *,
    total_wait_sec: float,
    poll_interval_sec: float = 0.5,
) -> str | None:
    """Poll GetAppLocation until the RPC server responds (or a timeout elapses)."""
    deadline = time.time() + max(0.0, float(total_wait_sec))
    while True:
        remaining = deadline - time.time()
        if remaining <= 0.0:
            return None
        timeout = min(DEFAULT_RPC_BOOTSTRAP_TIMEOUT_SEC, max(0.1, remaining))
        val = _try_get_atlas_dir_from_rpc(channel, timeout_sec=timeout)
        if val:
            return val
        time.sleep(max(0.05, float(poll_interval_sec)))


def _resolve_install_dir_for_launch(*, atlas_dir_hint: str | None) -> Path | None:
    """Return an Atlas install dir to launch, if available on disk."""
    if isinstance(atlas_dir_hint, str) and atlas_dir_hint.strip():
        try:
            p = Path(atlas_dir_hint.strip())
            if p.exists():
                return p
        except Exception:
            pass
    for d in default_install_dirs():
        try:
            if d.exists():
                return d
        except Exception:
            continue
    return None


def _launch_atlas(*, atlas_dir: Path) -> None:
    """Launch (or activate) Atlas at the given install dir."""
    system = platform.system()
    if system == "Darwin":
        # LaunchServices is the most reliable way to start a .app bundle.
        if atlas_dir.suffix.lower() != ".app":
            # Some dev builds may return the binary dir; fall back to the binary
            # when the bundle root isn't provided.
            atlas_bin, _ = compute_paths_from_atlas_dir(atlas_dir)
            candidate = atlas_bin if atlas_bin.exists() else atlas_dir
            subprocess.Popen(
                [str(candidate)],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            return
        subprocess.Popen(
            ["open", "-a", str(atlas_dir)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        return

    atlas_bin, _ = compute_paths_from_atlas_dir(atlas_dir)
    if not atlas_bin.exists():
        raise FileNotFoundError(f"Atlas binary not found at {atlas_bin}")
    subprocess.Popen(
        [str(atlas_bin)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


@dataclass
class SceneClient:
    address: str = "localhost:50051"
    atlas_dir: str | None = None
    # When true, validate user-supplied parameter values against the server's
    # JSON Schemas before sending them over RPC.
    strict_schema_validation: bool = True
    _tmpdir: Any = None
    _pb2: Any = None
    _pb2_grpc: Any = None
    _channel: Any = None
    _stub: Any = None
    _param_list_cache: dict[int, Any] | None = None
    _jsonschema_validator_cache: dict[str, Any] | None = None
    _camera_param_json_key: str | None = None

    def __post_init__(self):
        # Configure logger (default INFO to stdout) once
        self._logger = logging.getLogger("atlas_agent.rpc")
        if not self._logger.handlers:
            h = logging.StreamHandler()
            fmt = logging.Formatter(
                fmt="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
                datefmt="%H:%M:%S",
            )
            h.setFormatter(fmt)
            self._logger.addHandler(h)
            # Default to WARNING to reduce noise (internal policy; not user-configurable).
            self._logger.setLevel(DEFAULT_RPC_LOG_LEVEL)
            self._logger.propagate = False

        self._channel = grpc.insecure_channel(self.address)

        # Bootstrap atlas_dir from the running app (authoritative).
        detected = _try_get_atlas_dir_from_rpc(
            self._channel, timeout_sec=DEFAULT_RPC_BOOTSTRAP_TIMEOUT_SEC
        )
        if not detected:
            install = _resolve_install_dir_for_launch(atlas_dir_hint=self.atlas_dir)
            if install is None:
                raise RuntimeError(
                    "Atlas RPC server is not reachable and no installed Atlas app was found.\n"
                    f"- rpc_address: {self.address}\n"
                    "- expected install locations: "
                    + ", ".join(str(p) for p in default_install_dirs())
                    + "\n"
                    "Please install Atlas to the default location, or launch Atlas manually, then re-run atlas-agent."
                )
            self._logger.warning(
                "Atlas RPC not reachable at %s; launching Atlas from %s",
                self.address,
                str(install),
            )
            try:
                _launch_atlas(atlas_dir=install)
            except Exception as e:
                raise RuntimeError(
                    "Failed to launch Atlas from the detected install location.\n"
                    f"- install: {install}\n"
                    f"- error: {e}"
                ) from e

            detected = _wait_for_atlas_dir_from_rpc(
                self._channel,
                total_wait_sec=DEFAULT_RPC_BOOTSTRAP_TOTAL_WAIT_SEC,
            )

        if not detected:
            raise RuntimeError(
                "Atlas was launched (or is expected to be running), but the RPC server did not become ready.\n"
                f"- rpc_address: {self.address}\n"
                f"- waited_sec: {DEFAULT_RPC_BOOTSTRAP_TOTAL_WAIT_SEC}\n"
                "Make sure Atlas is running and that the local gRPC scene server is enabled."
            )

        self.atlas_dir = str(detected).strip()

        atlas_dir_path: Path | None = None
        if isinstance(self.atlas_dir, str) and self.atlas_dir.strip():
            try:
                atlas_dir_path = Path(self.atlas_dir.strip())
            except Exception:
                atlas_dir_path = None

        if atlas_dir_path is None:
            raise RuntimeError("Atlas app location could not be resolved from RPC response.")

        self._tmpdir, self._pb2, self._pb2_grpc = _load_stubs(atlas_dir_path)
        self._stub = self._pb2_grpc.SceneStub(self._channel)
        self._param_list_cache = {}
        self._jsonschema_validator_cache = {}
        self._camera_param_json_key = None

    def engine_ready(self, *, timeout_sec: float | None = None) -> bool:
        req = self._pb2.Empty()
        kw = {}
        if timeout_sec is not None:
            kw["timeout"] = float(timeout_sec)
        resp = self._stub.EngineReady(req, **kw)
        self._log_rpc("EngineReady", req, resp)
        return bool(resp.ok)

    def get_status(
        self,
        *,
        ids: Iterable[int] | None = None,
        include_all_objects: bool = False,
        timeout_sec: float = 5.0,
    ) -> dict[str, Any]:
        """Return a readiness snapshot from the GUI RPC server.

        Requires Atlas >= the build that implements Scene.GetStatus.
        """

        if self._stub is None or not hasattr(self._stub, "GetStatus"):
            raise RuntimeError("RPC GetStatus is not available in this Atlas build.")
        if self._pb2 is None or not hasattr(self._pb2, "GetStatusRequest"):
            raise RuntimeError("RPC GetStatusRequest is not available in this Atlas build.")

        ids_list: list[int] = []
        if ids is not None:
            for v in ids:
                try:
                    ids_list.append(int(v))
                except Exception:
                    continue
        req = self._pb2.GetStatusRequest(
            ids=ids_list,
            include_all_objects=bool(include_all_objects),
        )
        resp = self._stub.GetStatus(req, timeout=float(timeout_sec))
        self._log_rpc("GetStatus", req, resp)
        try:
            return MessageToDict(resp)
        except Exception:
            return {"ok": bool(getattr(resp, "ok", False))}

    def wait_for_objects_ready(
        self,
        ids: Iterable[int],
        *,
        timeout_sec: float = 30.0,
        poll_interval_sec: float = 0.2,
    ) -> dict[str, Any]:
        """Block until object ids are ready for engine-backed RPCs (or timeout).

        This waits for the 3D view/filter binding (not full progressive data load).
        Requires Atlas >= the build that implements Scene.WaitForObjectsReady.
        """

        if self._stub is None or not hasattr(self._stub, "WaitForObjectsReady"):
            raise RuntimeError("RPC WaitForObjectsReady is not available in this Atlas build.")
        if self._pb2 is None or not hasattr(self._pb2, "WaitForObjectsReadyRequest"):
            raise RuntimeError(
                "RPC WaitForObjectsReadyRequest is not available in this Atlas build."
            )

        ids_list: list[int] = []
        for v in ids:
            try:
                ids_list.append(int(v))
            except Exception:
                continue
        if not ids_list:
            raise ValueError("ids must contain at least one integer id")

        timeout_ms = max(0, int(float(timeout_sec) * 1000.0))
        poll_ms = max(0, int(float(poll_interval_sec) * 1000.0))
        req = self._pb2.WaitForObjectsReadyRequest(
            ids=ids_list,
            timeout_ms=timeout_ms,
            poll_interval_ms=poll_ms,
        )
        # Client-side RPC timeout must exceed the server-side wait window.
        # Add a small margin so transport jitter doesn't cause false timeouts.
        rpc_timeout = None
        if timeout_sec is not None:
            rpc_timeout = float(timeout_sec) + 10.0
        resp = self._stub.WaitForObjectsReady(req, timeout=rpc_timeout)
        self._log_rpc("WaitForObjectsReady", req, resp)
        try:
            return MessageToDict(resp)
        except Exception:
            return {"ok": bool(getattr(resp, "ok", False))}

    def ensure_view(self, *, require: bool = True) -> bool:
        """Ensure a 3D view (and rendering engine) exists and is ready.

        Notes:
          - Ensure3DWindow is asynchronous on the UI side; EngineReady is the
            authoritative signal that engine-backed RPCs are safe to call.
          - When require=True (default), this raises on timeout so callers do not
            proceed into FAILED_PRECONDITION errors.
        """

        def _is_rpc_unavailable(err: Exception) -> bool:
            # gRPC server down/crashed typically surfaces as UNAVAILABLE + connection refused.
            try:
                if isinstance(err, grpc.RpcError):
                    return err.code() == grpc.StatusCode.UNAVAILABLE
            except Exception:
                pass
            msg = str(err or "").lower()
            return "connection refused" in msg or "failed to connect to all addresses" in msg

        def _raise_unavailable(err: Exception) -> None:
            msg = str(err or "").strip()
            raise RuntimeError(
                "Atlas RPC server is unavailable (cannot connect to localhost:50051).\n"
                "This usually means Atlas was closed or crashed.\n"
                f"- error: {msg}"
            )

        # Fast-path: already ready (avoid waking UI)
        try:
            if self.engine_ready(timeout_sec=DEFAULT_ENGINE_READY_RPC_TIMEOUT_SEC):
                return True
        except grpc.RpcError as e:
            if _is_rpc_unavailable(e):
                if require:
                    _raise_unavailable(e)
                return False
            # Fall through to requesting a window + polling readiness.
        except Exception:
            # Fall through to requesting a window + polling readiness.
            pass

        # Ask GUI to ensure a 3D window/canvas exists, then wait for readiness.
        req = self._pb2.Empty()
        try:
            resp = self._stub.Ensure3DWindow(req, timeout=1.0)
            self._log_rpc("Ensure3DWindow", req, resp)
        except grpc.RpcError as e:
            # If the server is down, fail fast rather than waiting for engine readiness.
            self._log_rpc("Ensure3DWindow", req, None, error=e)
            if _is_rpc_unavailable(e):
                if require:
                    _raise_unavailable(e)
                return False
        except Exception as e:
            self._log_rpc("Ensure3DWindow", req, None, error=e)

        deadline = time.time() + float(DEFAULT_ENGINE_READY_TOTAL_WAIT_SEC)
        while time.time() < deadline:
            try:
                if self.engine_ready(timeout_sec=DEFAULT_ENGINE_READY_RPC_TIMEOUT_SEC):
                    return True
            except grpc.RpcError as e:
                if _is_rpc_unavailable(e):
                    if require:
                        _raise_unavailable(e)
                    return False
                # Server may still be initializing, or may be restarting.
            except Exception:
                # Server may still be initializing, or may be restarting.
                pass
            time.sleep(DEFAULT_ENGINE_READY_POLL_INTERVAL_SEC)

        if require:
            raise RuntimeError(
                "3D engine not ready (timed out waiting after Ensure3DWindow).\n"
                f"- waited_sec: {DEFAULT_ENGINE_READY_TOTAL_WAIT_SEC}\n"
                "If Atlas is launching or loading a heavy scene, retry after it becomes responsive."
            )
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

    def get_app_version(self, *, timeout_sec: float = 1.0) -> str | None:
        """Best-effort: return the running Atlas build/version string.

        Returns None when the server does not implement GetAppVersion (older installs)
        or when RPC is unavailable.
        """
        try:
            if self._stub is not None and hasattr(self._stub, "GetAppVersion"):
                resp = self._stub.GetAppVersion(
                    empty_pb2.Empty(), timeout=float(timeout_sec)
                )
                val = str(getattr(resp, "value", "") or "").strip()
                return val if val else None
        except Exception:
            pass

        # Fallback: generic call (does not require compiled stubs).
        try:
            rpc = self._channel.unary_unary(
                "/atlas.rpc.Scene/GetAppVersion",
                request_serializer=empty_pb2.Empty.SerializeToString,
                response_deserializer=wrappers_pb2.StringValue.FromString,
            )
            resp = rpc(empty_pb2.Empty(), timeout=float(timeout_sec))
            val = str(getattr(resp, "value", "") or "").strip()
            return val if val else None
        except grpc.RpcError as e:
            try:
                if e.code() == grpc.StatusCode.UNIMPLEMENTED:
                    return None
            except Exception:
                pass
            return None
        except Exception:
            return None

    def load_files(self, files: Iterable[str]):
        exp: list[str] = []
        missing: list[str] = []
        for s in files:
            t = _expand_path(s)
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
                    existing_paths.add(os.path.normpath(_expand_path(p)))
            except Exception:
                pass
        to_load: list[str] = []
        skipped: list[str] = []
        for s in files:
            t = _expand_path(s)
            t_norm = os.path.normpath(t)
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

    def scene_facts(
        self,
        *,
        animation_id: int | None = None,
        include_values: bool = False,
        include_scene_values: bool = True,
    ) -> dict:
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
            # Camera keys (optional: only when an animation id is provided)
            if animation_id is not None:
                lr = self.list_keys(animation_id=int(animation_id), target_id=0, include_values=False)
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
                    if animation_id is None:
                        continue
                    try:
                        lr = self.list_keys(
                            animation_id=int(animation_id),
                            target_id=int(oid),
                            json_key=str(jk),
                            include_values=bool(include_values),
                        )
                        if include_values:
                            entries = []
                            for k in getattr(lr, "keys", []) or []:
                                try:
                                    vj = getattr(k, "value_json", "") or ""
                                    val = json.loads(vj) if vj else None
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

    def scene_facts_compact(
        self,
        *,
        animation_id: int | None = None,
        key_targets: dict[int, list[str]] | None = None,
        include_key_values: bool = False,
        scene_value_targets: dict[int, list[str]] | None = None,
        include_objects: bool = True,
        include_camera_key_times: bool = True,
    ) -> dict:
        """Return a compact facts snapshot suitable for LLM grounding.

        Unlike scene_facts(), this does NOT enumerate every parameter of every
        object. Callers must provide explicit targets.

        Returns:
          {
            "objects_list": [{id,type,name,path,visible}, ...]  (optional)
            "keys": {
              "camera": [times...],
              "objects": { "<id>": { "<json_key>": ([times...] | [{time,value}...]) } }
            },
            "scene_values": { "<id>": { "<json_key>": value, ... }, ... } (optional)
          }
        """
        facts: dict[str, Any] = {"objects_list": [], "keys": {"camera": [], "objects": {}}}

        if include_objects:
            try:
                objs = self.list_objects()
                for o in getattr(objs, "objects", []):
                    facts["objects_list"].append(
                        {
                            "id": int(getattr(o, "id", 0)),
                            "type": getattr(o, "type", ""),
                            "name": getattr(o, "name", ""),
                            "path": getattr(o, "path", ""),
                            "visible": bool(getattr(o, "visible", False)),
                        }
                    )
            except Exception:
                pass

        if include_camera_key_times and animation_id is not None:
            try:
                lr = self.list_keys(animation_id=int(animation_id), target_id=0, include_values=False)
                cam_times = [float(getattr(k, "time", 0.0)) for k in getattr(lr, "keys", [])]
                if cam_times:
                    facts["keys"]["camera"] = sorted(cam_times)
            except Exception:
                pass

        if key_targets:
            for oid, json_keys in key_targets.items():
                try:
                    oid_int = int(oid)
                except Exception:
                    continue
                if oid_int <= 0:
                    continue
                for jk in json_keys or []:
                    if not isinstance(jk, str) or not jk:
                        continue
                    if animation_id is None:
                        continue
                    try:
                        lr = self.list_keys(
                            animation_id=int(animation_id),
                            target_id=int(oid_int),
                            json_key=str(jk),
                            include_values=bool(include_key_values),
                        )
                        if include_key_values:
                            entries = []
                            for k in getattr(lr, "keys", []) or []:
                                try:
                                    vj = getattr(k, "value_json", "") or ""
                                    val = json.loads(vj) if vj else None
                                except Exception:
                                    val = None
                                ent: dict[str, Any] = {"time": float(getattr(k, "time", 0.0))}
                                if val is not None:
                                    ent["value"] = val
                                entries.append(ent)
                            if entries:
                                facts["keys"]["objects"].setdefault(str(oid_int), {})[jk] = sorted(
                                    entries, key=lambda e: float(e.get("time", 0.0))
                                )
                        else:
                            times = [float(getattr(k, "time", 0.0)) for k in getattr(lr, "keys", [])]
                            if times:
                                facts["keys"]["objects"].setdefault(str(oid_int), {})[jk] = sorted(times)
                    except Exception:
                        continue

        if scene_value_targets:
            sv: dict[str, dict[str, Any]] = {}
            for oid, json_keys in scene_value_targets.items():
                try:
                    oid_int = int(oid)
                except Exception:
                    continue
                keys = [k for k in (json_keys or []) if isinstance(k, str) and k]
                if not keys:
                    continue
                try:
                    sv[str(oid_int)] = self.get_param_values(id=oid_int, json_keys=keys)
                except Exception:
                    continue
            if sv:
                facts["scene_values"] = sv

        return facts

    def list_objects(self):
        req = self._pb2.Empty()
        resp = self._stub.ListObjects(req)
        self._log_rpc("ListObjects", req, resp)
        return resp

    def bbox(self, *, ids: list[int] | None = None, after_clipping: bool = False) -> dict[str, Any]:
        """Compute a world-space bounding box for the given ids.

        This requires the 3D rendering engine. The client will ensure the 3D view
        exists before calling the RPC.

        Returns:
          {
            "ok": bool,
            "min": [x,y,z],
            "max": [x,y,z],
            "center": [x,y,z],
            "size": [x,y,z],
            "error"?: str
          }
        """

        # BBox is implemented in the engine; ensure the 3D window/engine exists.
        if not self.ensure_view(require=False):
            return {"ok": False, "error": "engine not ready"}

        req = self._pb2.BBoxRequest(ids=[int(i) for i in (ids or [])], after_clipping=bool(after_clipping))
        try:
            resp = self._stub.BBox(req)
        except Exception as e:
            self._log_rpc("BBox", req, None, error=e)
            msg = str(e)
            try:
                msg = e.details()  # type: ignore[attr-defined]
            except Exception:
                pass
            return {"ok": False, "error": msg}
        self._log_rpc("BBox", req, resp)
        b = resp.bbox
        return {
            "ok": True,
            "min": [b.min.x, b.min.y, b.min.z],
            "max": [b.max.x, b.max.y, b.max.z],
            "center": [b.center.x, b.center.y, b.center.z],
            "size": [b.size.x, b.size.y, b.size.z],
        }

    # Animation/timeline
    def ensure_animation(self, *, create_new: bool = False, name: str | None = None):
        """Ensure an Animation3D exists and return its id.

        Returns the raw EnsureAnimationResponse proto:
          - ok (bool)
          - animation_id (uint64)
          - created (bool)
          - error (string)
        """
        # Ensure the rendering engine exists (open 3D view if necessary).
        self.ensure_view()
        req = self._pb2.EnsureAnimationRequest(create_new=bool(create_new), name=str(name or ""))
        resp = self._stub.EnsureAnimation(req)
        self._log_rpc("EnsureAnimation", req, resp)
        return resp

    def set_duration(self, *, animation_id: int, seconds: float) -> bool:
        req = self._pb2.SetDurationRequest(animation_id=int(animation_id), duration=float(seconds))
        resp = self._stub.SetDuration(req)
        self._log_rpc("SetDuration", req, resp)
        return resp.ok

    def set_time(self, *, animation_id: int, seconds: float, cancel_rendering: bool = False) -> bool:
        req = self._pb2.SetTimeRequest(animation_id=int(animation_id), seconds=float(seconds), cancel_rendering=cancel_rendering)
        resp = self._stub.SetTime(req)
        self._log_rpc("SetTime", req, resp)
        return resp.ok

    def save_animation(self, *, animation_id: int, path: Path) -> bool:
        req = self._pb2.SaveAnimationRequest(animation_id=int(animation_id), path=str(path))
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

    def camera_get(self) -> dict:
        """Return the current engine camera as a typed value (no key writes)."""
        self.ensure_view()
        if not hasattr(self._stub, "CameraGet"):
            raise RuntimeError("CameraGet is not supported by this Atlas version")
        req = empty_pb2.Empty()
        resp = self._stub.CameraGet(req)
        self._log_rpc("CameraGet", req, resp)
        vals = [MessageToDict(v) for v in getattr(resp, "values", [])]
        return vals[0] if vals else {}

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
        bv = _to_proto_value(base_value) if base_value is not None else None
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

    def camera_move_local(
        self,
        *,
        op: str,
        distance: float,
        distance_is_fraction_of_bbox_radius: bool = False,
        ids: Optional[list[int]] = None,
        after_clipping: bool = True,
        move_center: bool = True,
        base_value: Optional[dict] = None,
    ) -> dict:
        """Move camera in its local basis (first-person 'fly' building block)."""
        self.ensure_view()
        if not hasattr(self._stub, "CameraMoveLocal"):
            raise RuntimeError("CameraMoveLocal is not supported by this Atlas version")
        kwargs: dict[str, Any] = {
            "op": str(op),
            "distance": float(distance),
            "distance_is_fraction_of_bbox_radius": bool(distance_is_fraction_of_bbox_radius),
            "ids": [int(i) for i in (ids or [])],
            "after_clipping": bool(after_clipping),
            "move_center": bool(move_center),
        }
        if base_value is not None:
            kwargs["base_value"] = _to_proto_value(base_value)
        req = self._pb2.CameraMoveLocalRequest(**kwargs)
        resp = self._stub.CameraMoveLocal(req)
        self._log_rpc("CameraMoveLocal", req, resp)
        vals = [MessageToDict(v) for v in getattr(resp, "values", [])]
        return vals[0] if vals else {}

    def camera_look_at(
        self,
        *,
        world_point: Optional[tuple[float, float, float]] = None,
        target_bbox_center: bool = False,
        bbox_fraction_point: Optional[tuple[float, float, float]] = None,
        ids: Optional[list[int]] = None,
        after_clipping: bool = True,
        base_value: Optional[dict] = None,
    ) -> dict:
        """Aim camera at a world point or bbox-derived point (no key writes)."""
        self.ensure_view()
        if not hasattr(self._stub, "CameraLookAt"):
            raise RuntimeError("CameraLookAt is not supported by this Atlas version")

        modes = 0
        if world_point is not None:
            modes += 1
        if bool(target_bbox_center):
            modes += 1
        if bbox_fraction_point is not None:
            modes += 1
        if modes != 1:
            raise ValueError("Exactly one of: world_point, target_bbox_center, bbox_fraction_point must be set")

        kwargs: dict[str, Any] = {
            "ids": [int(i) for i in (ids or [])],
            "after_clipping": bool(after_clipping),
        }
        if base_value is not None:
            kwargs["base_value"] = _to_proto_value(base_value)

        if world_point is not None:
            x, y, z = world_point
            kwargs["world_point"] = self._pb2.Vec3(x=float(x), y=float(y), z=float(z))
        elif bbox_fraction_point is not None:
            fx, fy, fz = bbox_fraction_point
            kwargs["bbox_fraction_point"] = self._pb2.Vec3(x=float(fx), y=float(fy), z=float(fz))
        else:
            # Oneof presence matters; set to True explicitly.
            kwargs["target_bbox_center"] = True

        req = self._pb2.CameraLookAtRequest(**kwargs)
        resp = self._stub.CameraLookAt(req)
        self._log_rpc("CameraLookAt", req, resp)
        vals = [MessageToDict(v) for v in getattr(resp, "values", [])]
        return vals[0] if vals else {}

    def camera_path_solve(
        self,
        *,
        ids: Optional[list[int]] = None,
        after_clipping: bool = True,
        base_value: Optional[dict] = None,
        waypoints: list[dict],
    ) -> list[dict]:
        """Solve typed camera keys from waypoints (does not write keys)."""
        self.ensure_view()
        if not hasattr(self._stub, "CameraPathSolve"):
            raise RuntimeError("CameraPathSolve is not supported by this Atlas version")

        pb_wps = []
        for w in waypoints or []:
            if not isinstance(w, dict):
                raise ValueError("each waypoint must be an object")
            tm = float(w.get("time", 0.0))
            kw: dict[str, Any] = {"time": tm}

            eye = w.get("eye")
            if isinstance(eye, dict):
                if "world" in eye:
                    x, y, z = eye["world"]
                    kw["world_eye"] = self._pb2.Vec3(x=float(x), y=float(y), z=float(z))
                elif "bbox_fraction" in eye:
                    x, y, z = eye["bbox_fraction"]
                    kw["bbox_fraction_eye"] = self._pb2.Vec3(x=float(x), y=float(y), z=float(z))

            look = w.get("look_at")
            if isinstance(look, dict):
                if "world" in look:
                    x, y, z = look["world"]
                    kw["world_look_at"] = self._pb2.Vec3(x=float(x), y=float(y), z=float(z))
                elif look.get("bbox_center") is True:
                    kw["look_at_bbox_center"] = True
                elif "bbox_fraction" in look:
                    x, y, z = look["bbox_fraction"]
                    kw["bbox_fraction_look_at"] = self._pb2.Vec3(x=float(x), y=float(y), z=float(z))

            pb_wps.append(self._pb2.CameraWaypoint(**kw))

        req_kwargs: dict[str, Any] = {
            "ids": [int(i) for i in (ids or [])],
            "after_clipping": bool(after_clipping),
            "waypoints": pb_wps,
        }
        if base_value is not None:
            req_kwargs["base_value"] = _to_proto_value(base_value)
        req = self._pb2.CameraPathSolveRequest(**req_kwargs)
        resp = self._stub.CameraPathSolve(req)
        self._log_rpc("CameraPathSolve", req, resp)
        out: list[dict] = []
        for k in getattr(resp, "keys", []):
            out.append({"time": float(getattr(k, "time", 0.0)), "value": MessageToDict(getattr(k, "value"))})
        return out

    def set_camera_interpolation_method(self, *, animation_id: int, method: str) -> bool:
        """Set camera animation interpolation method for an Animation3D."""
        self.ensure_view()
        if not hasattr(self._stub, "SetCameraInterpolationMethod"):
            raise RuntimeError("SetCameraInterpolationMethod is not supported by this Atlas version")
        req = self._pb2.SetCameraInterpolationMethodRequest(animation_id=int(animation_id), method=str(method))
        resp = self._stub.SetCameraInterpolationMethod(req)
        self._log_rpc("SetCameraInterpolationMethod", req, resp)
        return bool(getattr(resp, "ok", False))

    def get_camera_interpolation_method(self, *, animation_id: int) -> str | None:
        """Get camera animation interpolation method for an Animation3D."""
        self.ensure_view()
        if not hasattr(self._stub, "GetCameraInterpolationMethod"):
            return None
        req = self._pb2.GetCameraInterpolationMethodRequest(animation_id=int(animation_id))
        resp = self._stub.GetCameraInterpolationMethod(req)
        self._log_rpc("GetCameraInterpolationMethod", req, resp)
        try:
            v = str(getattr(resp, "value", "") or "").strip()
            return v if v else None
        except Exception:
            return None

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
            st = struct_pb2.Struct()
            for k, param_value in params.items():
                st.fields[k].CopyFrom(_to_proto_value(param_value))
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
        animation_id: int | None = None,
        ids: Optional[list[int]] = None,
        times: list[float],
        values: list[dict] | None = None,
        constraints: Optional[dict] = None,
        policies: Optional[dict] = None,
    ) -> dict:
        self.ensure_view()
        values = values or []
        if values:
            cam_key = self._camera_json_key()
            values = [
                self._validate_param_value(id=0, json_key=cam_key, value=v)  # type: ignore[arg-type]
                for v in values
            ]
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
        req_kwargs: dict[str, Any] = {
            "ids": ids or [],
            "times": [float(t) for t in times],
            "values": [_to_proto_value(camera_value) for camera_value in values],
            "constraints": cons if cons is not None else None,
            "policies": pol if pol is not None else None,
        }
        if animation_id is not None:
            req_kwargs["animation_id"] = int(animation_id)
        req = self._pb2.CameraValidateRequest(
            **req_kwargs,
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
    def set_key_camera(self, *, animation_id: int, time: float, easing: str, value: Any) -> bool:
        # Ensure engine/view exists before setting camera keys
        self.ensure_view()
        cam_key = self._camera_json_key()
        value = self._validate_param_value(id=0, json_key=cam_key, value=value)
        v = _to_proto_value(value)
        req = self._pb2.SetKeyRequest(animation_id=int(animation_id), target_id=0, time=time, easing=easing, value=v)
        resp = self._stub.SetKey(req)
        self._log_rpc("SetKey(camera)", req, resp)
        return resp.ok

    def list_params(self, *, id: int):
        # Ensure engine is ready (and open a 3D window if necessary)
        self.ensure_view()
        bound_id = int(id)
        if self._param_list_cache is not None:
            cached = self._param_list_cache.get(bound_id)
            if cached is not None:
                return cached
        req = self._pb2.ListParamsRequest(id=bound_id)
        resp = self._stub.ListParams(req)
        self._log_rpc("ListParams", req, resp)
        if self._param_list_cache is not None:
            self._param_list_cache[bound_id] = resp
        return resp

    def _camera_json_key(self) -> str:
        if isinstance(self._camera_param_json_key, str) and self._camera_param_json_key.strip():
            return self._camera_param_json_key
        pl = self.list_params(id=0)
        for p in getattr(pl, "params", []) or []:
            jk = str(getattr(p, "json_key", "") or "").strip()
            if jk:
                self._camera_param_json_key = jk
                return jk
        raise RuntimeError("Camera parameter schema is unavailable (ListParams(id=0) returned empty).")

    def _param_meta(self, *, id: int, json_key: str) -> Any | None:
        pl = self.list_params(id=int(id))
        for p in getattr(pl, "params", []) or []:
            if str(getattr(p, "json_key", "") or "") == json_key:
                return p
        return None

    def _param_value_schema(self, meta: Any) -> dict[str, Any] | bool:
        try:
            if hasattr(meta, "HasField") and meta.HasField("value_schema"):
                schema = MessageToDict(getattr(meta, "value_schema")) or {}
                return schema if isinstance(schema, dict) else {}
        except Exception:
            pass
        return {}

    def _normalize_value_for_param(self, *, param_type: str, value: Any) -> Any:
        """Normalize common user-friendly aliases to canonical schema keys."""
        if param_type != "3DTransform" or not isinstance(value, dict):
            return value

        # Patch-style: callers may provide any subset; omit unchanged fields.
        # Accept common aliases and map them to canonical jsonKey names.
        mapping = {
            "Scale": "Scale Vec3",
            "Scale Vec3": "Scale Vec3",
            "Translation": "Translation Vec3",
            "Translation Vec3": "Translation Vec3",
            "Rotation": "Rotation Vec4",
            "Rotation Vec4": "Rotation Vec4",
            "Rotation Center": "Rotation Center Vec3",
            "Rotation Center Vec3": "Rotation Center Vec3",
            "Center": "Rotation Center Vec3",
            "Center Vec3": "Rotation Center Vec3",
        }
        out: dict[str, Any] = {}
        for k, v in value.items():
            kk = mapping.get(str(k), str(k))
            out[kk] = v
        return out

    def _validate_param_value(self, *, id: int, json_key: str, value: Any) -> Any:
        if not self.strict_schema_validation:
            return value
        if JsonSchemaValidator is None:  # pragma: no cover
            raise RuntimeError("jsonschema is not available (strict_schema_validation requires it).")

        meta = self._param_meta(id=id, json_key=json_key)
        if meta is None:
            raise KeyError(f"json_key not found for id={id}: {json_key}")

        param_type = str(getattr(meta, "type", "") or "")
        normalized = self._normalize_value_for_param(param_type=param_type, value=value)
        schema = self._param_value_schema(meta)
        if not schema:
            return normalized
        if not isinstance(schema, dict):
            raise RuntimeError(f"invalid value_schema for id={id} json_key={json_key}: {type(schema).__name__}")

        try:
            digest = hashlib.sha256(json.dumps(schema, sort_keys=True).encode("utf-8")).hexdigest()
        except Exception:
            digest = ""
        cache_key = f"{id}:{json_key}:{digest}"
        validator = None
        if self._jsonschema_validator_cache is not None:
            validator = self._jsonschema_validator_cache.get(cache_key)
        if validator is None:
            try:
                validator = JsonSchemaValidator(schema)
            except JsonSchemaError as e:
                raise RuntimeError(
                    "invalid value_schema received from server; cannot validate client payload.\n"
                    f"- id: {id}\n"
                    f"- json_key: {json_key}\n"
                    f"- error: {e}"
                ) from e
            if self._jsonschema_validator_cache is not None:
                self._jsonschema_validator_cache[cache_key] = validator

        errors = list(validator.iter_errors(normalized))
        if errors:
            e = errors[0]
            loc = "".join(
                [f"[{repr(p)}]" if isinstance(p, int) else f".{p}" for p in e.path]
            )
            msg = f"{loc}: {e.message}" if loc else e.message
            raise ValueError(
                f"value does not match schema for id={id} json_key={json_key}: {msg}"
            )
        return normalized

    def clear_keys(self, *, animation_id: int, target_id: int, json_key: Optional[str] = None) -> bool:
        self.ensure_view()
        req = self._pb2.ClearKeysRequest(animation_id=int(animation_id), target_id=int(target_id), json_key=json_key or "")
        resp = self._stub.ClearKeys(req)
        self._log_rpc("ClearKeys", req, resp)
        return resp.ok

    # Non-camera parameter key operations (id-based)
    def set_key_param(self, *, animation_id: int, target_id: int, json_key: str, time: float, easing: str = "Linear", value: Any) -> bool:
        # Key writes require engine+doc on the server side; ensure the engine exists.
        self.ensure_view()
        value = self._validate_param_value(id=int(target_id), json_key=str(json_key), value=value)
        v = _to_proto_value(value)
        req = self._pb2.SetKeyRequest(animation_id=int(animation_id), target_id=int(target_id), json_key=json_key, time=float(time), easing=easing, value=v)
        resp = self._stub.SetKey(req)
        self._log_rpc("SetKey(param)", req, resp)
        return resp.ok

    def remove_key(self, *, animation_id: int, target_id: int, json_key: str, time: float) -> bool:
        self.ensure_view()
        req = self._pb2.RemoveKeyRequest(animation_id=int(animation_id), target_id=int(target_id), json_key=json_key, time=float(time))
        resp = self._stub.RemoveKey(req)
        self._log_rpc("RemoveKey", req, resp)
        return resp.ok

    def batch(self, *, animation_id: int, set_keys: list[dict] | None = None, remove_keys: list[dict] | None = None, commit: bool = True) -> bool:
        # Ensure engine/view exists before batch operations
        self.ensure_view()
        set_keys = set_keys or []
        remove_keys = remove_keys or []
        if not set_keys and not remove_keys:
            self._logger.error("Batch: refusing to execute with empty set/remove")
            return False
        # Construct protobuf requests (Animation3D-scoped ops)
        pb_set = []
        for s in set_keys:
            target_id = int(s.get("target_id", s.get("id", -1)))
            val = s.get("value")
            if target_id == 0:
                cam_key = self._camera_json_key()
                val = self._validate_param_value(id=0, json_key=cam_key, value=val)
                pb_set.append(
                    self._pb2.BatchSetKey(
                        target_id=target_id,
                        time=float(s["time"]),
                        easing=str(s.get("easing", "Linear")),
                        value=_to_proto_value(val),
                    )
                )
            else:
                jk = str(s.get("json_key") or "")
                if not jk:
                    raise ValueError("Batch set_keys requires json_key when target_id != 0")
                val = self._validate_param_value(id=target_id, json_key=jk, value=val)
                pb_set.append(
                    self._pb2.BatchSetKey(
                        target_id=target_id,
                        json_key=str(jk),
                        time=float(s["time"]),
                        easing=str(s.get("easing", "Linear")),
                        value=_to_proto_value(val),
                    )
                )
        pb_rem = []
        for r in remove_keys:
            target_id = int(r.get("target_id", r.get("id", -1)))
            pb_rem.append(
                self._pb2.BatchRemoveKey(
                    target_id=target_id,
                    json_key=str(r.get("json_key") or ""),
                    time=float(r["time"]),
                )
            )
        # Human-friendly payload log (sanitized)
        def _summarize_keys(keys: list[dict]):
            out: list[dict] = []
            for k in keys:
                target_id = int(k.get("target_id", k.get("id", -1)))
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
                out.append({"target_id": target_id, "json_key": jk, "time": t, "easing": ez, "value": val})
            return out
        self._logger.info("Batch(payload) %s", json.dumps({
            "animation_id": int(animation_id),
            "commit": bool(commit),
            "set_keys": _summarize_keys(set_keys),
            "remove_keys": _summarize_keys(remove_keys),
        }))
        req = self._pb2.BatchRequest(animation_id=int(animation_id), set_keys=pb_set, remove_keys=pb_rem, commit=bool(commit))
        resp = self._stub.Batch(req)
        self._log_rpc("Batch", req, resp)

        # Verify that keys now exist at requested times; log discrepancies.
        try:
            missing: list[dict] = []
            for s in set_keys:
                target_id = int(s.get("target_id", s.get("id", -1)))
                lr = self.list_keys(animation_id=int(animation_id), target_id=int(target_id), json_key=str(s.get("json_key", "")))
                target_times = [k.time for k in getattr(lr, "keys", [])]
                want_t = float(s.get("time", 0.0))
                if not any(abs(want_t - t) < 1e-6 for t in target_times):
                    missing.append({"target_id": target_id, "json_key": s.get("json_key"), "time": want_t})
            if missing:
                self._logger.warning("Batch verify: missing keys at times: %s", json.dumps(missing))
            else:
                self._logger.info("Batch verify: all keys present (%d)", len(set_keys))
        except Exception as e:
            self._log_rpc("BatchVerify", req, None, error=e)
        return bool(resp.ok)

    def list_keys(self, *, animation_id: int, target_id: int, json_key: Optional[str] = None, include_values: bool = False):
        # Ensure engine/view exists. Do not force-create animations here; callers
        # must supply an explicit Animation3D id for determinism.
        self.ensure_view()
        req = self._pb2.ListKeysRequest(
            animation_id=int(animation_id),
            target_id=int(target_id),
            json_key=json_key or "",
            include_values=bool(include_values),
        )
        resp = self._stub.ListKeys(req)
        self._log_rpc("ListKeys", req, resp)
        return resp

    def get_time(self, *, animation_id: int):
        req = self._pb2.GetTimeRequest(animation_id=int(animation_id))
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
        # Parameter readback is implemented via the rendering engine parameter list.
        # Ensure the 3D view/engine exists so GetParamValues does not fail with
        # FAILED_PRECONDITION ("engine not ready").
        self.ensure_view()
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
        # Validation runs against the engine parameter schemas; ensure the engine exists.
        self.ensure_view()
        pb_items = []
        for it in set_params:
            id = int(it.get("id"))
            json_key = str(it["json_key"])
            value = it.get("value")
            value = self._validate_param_value(id=id, json_key=json_key, value=value)
            pb_items.append(
                self._pb2.SetParam(
                    id=id,
                    json_key=json_key,
                    value=_to_proto_value(value),
                )
            )
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
        # ApplySceneParams runs via the engine parameter system; ensure the engine exists.
        self.ensure_view()
        pb_items = []
        for it in set_params:
            id = int(it.get("id"))
            json_key = str(it["json_key"])
            value = it.get("value")
            value = self._validate_param_value(id=id, json_key=json_key, value=value)
            pb_items.append(
                self._pb2.SetParam(
                    id=id,
                    json_key=json_key,
                    value=_to_proto_value(value),
                )
            )
        req = self._pb2.ApplySceneParamsRequest(set_params=pb_items)
        resp = self._stub.ApplySceneParams(req)
        self._log_rpc("ApplySceneParams", req, resp)
        return bool(getattr(resp, "ok", False))

    def save_scene(self, path: Path) -> bool:
        req = self._pb2.SaveSceneRequest(path=str(path))
        resp = self._stub.SaveScene(req)
        self._log_rpc("SaveScene", req, resp)
        return bool(getattr(resp, "ok", False))

    def screenshot_3d(
        self,
        *,
        width: int,
        height: int,
        path: Path | None = None,
        overwrite: bool = True,
    ) -> dict[str, Any]:
        """Render a single screenshot of the current 3D scene (no animation export).

        Returns: {"ok": bool, "path": str, "error"?: str}
        """

        self.ensure_view()

        if self._stub is None or not hasattr(self._stub, "TakeScreenshot3D"):
            raise RuntimeError("TakeScreenshot3D is not supported by this Atlas version")

        out_path = str(path) if path is not None else ""
        req = self._pb2.ScreenshotRequest(
            width=int(width),
            height=int(height),
            path=out_path,
            overwrite=bool(overwrite),
        )
        resp = self._stub.TakeScreenshot3D(req)
        self._log_rpc("TakeScreenshot3D", req, resp)

        ok = bool(getattr(resp, "ok", False))
        out: dict[str, Any] = {"ok": ok, "path": str(getattr(resp, "path", "") or "")}
        err = str(getattr(resp, "error", "") or "")
        if (not ok) and err:
            out["error"] = err
        return out

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
