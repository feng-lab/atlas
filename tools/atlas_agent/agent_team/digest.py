from __future__ import annotations

from typing import Any, Iterable


def _trim_text(s: str, limit: int) -> str:
    return s if len(s) <= limit else s[:limit] + "…"


def build_verification_digest(
    *,
    scene: Any,
    ledger: Iterable[dict] | None,
    include_values: bool = False,
    validate_camera: bool = True,
    max_param_per_object: int = 3,
    max_times_per_entry: int = 6,
    max_chars: int = 4000,
) -> str:
    """Build a compact, text-based verification digest for Inspector/Describer.

    - Includes camera key times and optional validation summary
    - Includes per-object parameter key times (targeted by this turn's ledger)
    - Optionally includes a trimmed sample of value_json for those keys
    - Trims the final digest to max_chars
    """
    lines: list[str] = []

    # Camera: times and optional validation
    cam_times: list[float] = []
    lr_cam = None
    try:
        lr_cam = scene.list_keys(id=0, include_values=include_values)
        cam_times = [float(k.time) for k in getattr(lr_cam, "keys", [])]
    except Exception:
        cam_times = []
    if cam_times:
        st = ", ".join(f"{t:.3g}" for t in cam_times[:max_times_per_entry])
        more = " …" if len(cam_times) > max_times_per_entry else ""
        lines.append(f"camera_times=[{st}]{more}")
    cam_ok = None
    if validate_camera and cam_times:
        try:
            # Build values (typed) when available
            vals: list[dict] = []
            for k in getattr(lr_cam, "keys", []) if lr_cam else []:
                try:
                    vj = getattr(k, "value_json", "")
                    val = __import__("json").loads(vj) if vj else {}
                    vals.append(val)
                except Exception:
                    vals.append({})
            ids_for_cam: list[int] = []
            try:
                ids_for_cam = scene.fit_candidates()
            except Exception:
                ids_for_cam = []
            cv = scene.camera_validate(
                ids=ids_for_cam,
                times=cam_times,
                values=vals,
                constraints={"keep_visible": True, "min_coverage": 0.95},
                policies={"adjust_fov": False, "adjust_distance": False, "adjust_clipping": False},
            )
            cam_ok = bool(cv.get("ok", False))
        except Exception:
            cam_ok = None
    if cam_ok is not None:
        lines.append(f"camera_validated={str(bool(cam_ok)).lower()}")

    # Determine targeted (id, json_key) pairs from ledger for this turn
    target_ids: set[int] = set()
    id_to_params: dict[int, set[str]] = {}
    scene_targets: dict[int, set[str]] = {}
    for e in (ledger or []):
        tool = e.get("tool")
        args = e.get("args") or {}
        if tool in ("animation_set_key_param", "animation_replace_key_param"):
            try:
                tid = int(args.get("id", -1))
                if tid > 0:
                    target_ids.add(tid)
                    jk = args.get("json_key")
                    if jk:
                        id_to_params.setdefault(tid, set()).add(str(jk))
            except Exception:
                continue
        elif tool == "animation_batch":
            try:
                for sk in (args.get("set_keys") or []):
                    tid = int(sk.get("id", -1))
                    if tid > 0:
                        target_ids.add(tid)
                        jk = sk.get("json_key")
                        if jk:
                            id_to_params.setdefault(tid, set()).add(str(jk))
            except Exception:
                continue
        elif tool in ("scene_apply", "scene_validate_apply"):
            try:
                for sp in (args.get("set_params") or []):
                    sid = int(sp.get("id", -1))
                    if sid >= 0:
                        jk = sp.get("json_key")
                        if jk:
                            scene_targets.setdefault(sid, set()).add(str(jk))
            except Exception:
                continue

    # Resolve object names (targets only)
    id_name: dict[int, str] = {}
    try:
        objs = scene.list_objects()
        for o in getattr(objs, "objects", []):
            oid = int(getattr(o, "id", -1))
            if oid in target_ids:
                nm = getattr(o, "name", "") or getattr(o, "path", "") or getattr(o, "type", "")
                id_name[oid] = str(nm)
    except Exception:
        pass

    # Query actual times (and optional sample values) per param
    for oid in sorted(target_ids):
        nm = id_name.get(oid, "")
        lines.append(f"object {oid} {nm}:")
        params = sorted(id_to_params.get(oid, set()))
        shown = 0
        for jk in params:
            if shown >= max_param_per_object:
                break
            try:
                lr = scene.list_keys(id=oid, json_key=jk, include_values=include_values)
                times = [float(k.time) for k in getattr(lr, "keys", [])]
                if not times:
                    continue
                st = ", ".join(f"{t:.3g}" for t in times[:max_times_per_entry])
                more = " …" if len(times) > max_times_per_entry else ""
                if include_values:
                    # Sample up to one value_json at the first time
                    vtxt = None
                    for k in getattr(lr, "keys", []):
                        try:
                            vj = getattr(k, "value_json", "") or ""
                            if vj:
                                vtxt = _trim_text(vj, 96)
                                break
                        except Exception:
                            continue
                    if vtxt:
                        lines.append(f"  - {jk}: [{st}]{more} sample={vtxt}")
                    else:
                        lines.append(f"  - {jk}: [{st}]{more}")
                else:
                    lines.append(f"  - {jk}: [{st}]{more}")
                shown += 1
            except Exception:
                continue

    # Scene-only parameters: echo current values (no timeline)
    for sid in sorted(scene_targets.keys()):
        try:
            nm = id_name.get(sid, "")
            lines.append(f"scene {sid} {nm}:")
            jks = sorted(scene_targets.get(sid) or [])
            vals = scene.get_param_values(id=int(sid), json_keys=jks)
            shown = 0
            for jk in jks:
                if shown >= max_param_per_object:
                    break
                v = vals.get(jk)
                try:
                    vtxt = _trim_text(__import__("json").dumps(v), 96)
                except Exception:
                    vtxt = _trim_text(str(v), 96)
                lines.append(f"  - {jk}: value={vtxt}")
                shown += 1
        except Exception:
            continue

    digest_text = "\n".join(lines)
    if len(digest_text) > max_chars:
        digest_text = digest_text[:max_chars] + "…"
    return digest_text
