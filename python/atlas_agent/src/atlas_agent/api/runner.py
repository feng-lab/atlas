"""Plan runner for Python scripts (validate → apply → verify).

This module provides a simple helper to execute a mixed Plan of stateless
scene edits and timeline keys with verification. It raises exceptions on
failure and does not attempt LLM-like auto-repair (scripts should handle
their own logic deterministically).
"""

from typing import Any

from ..scene_rpc import SceneClient
from .plan_types import Plan


def run_plan(client: SceneClient, plan: Plan) -> dict:
    """Execute a plan against the Scene RPC and return a verification report.

    Steps:
      - validate → apply for set_params
      - batch(set_keys/remove_keys, commit)
      - verify keys at requested times and return a compact report
    """
    report: dict[str, Any] = {"applied": {"params": 0, "keys": 0, "removed": 0}, "verify": {"missing_keys": []}}

    # Stateless scene params
    if plan.set_params:
        req = []
        for sp in plan.set_params:
            req.append({"id": int(sp.id), "json_key": sp.json_key, "value": sp.value})
        val = client.validate_apply(req)
        if not val.get("ok", False):
            raise RuntimeError(f"ValidateSceneParams failed: {val}")
        if not client.apply_params(req):
            raise RuntimeError("ApplySceneParams failed")
        report["applied"]["params"] = len(plan.set_params)

    # Timeline keys
    if plan.set_keys or plan.remove_keys:
        set_keys_req = []
        for sk in plan.set_keys:
            ent = {"id": int(sk.id), "time": float(sk.time), "easing": sk.easing, "value": sk.value}
            if int(sk.id) != 0:
                ent["json_key"] = str(sk.json_key or "")
            set_keys_req.append(ent)
        remove_req = []
        for rk in plan.remove_keys:
            ent = {"id": int(rk.id), "time": float(rk.time)}
            if int(rk.id) != 0:
                ent["json_key"] = str(rk.json_key or "")
            remove_req.append(ent)
        if not client.batch(set_keys=set_keys_req, remove_keys=remove_req, commit=bool(plan.commit)):
            raise RuntimeError("Batch failed")
        report["applied"]["keys"] = len(set_keys_req)
        report["applied"]["removed"] = len(remove_req)

        # Verify keys
        missing = []
        for sk in plan.set_keys:
            want_t = float(sk.time)
            if int(sk.id) == 0:
                lr = client.list_keys(id=0)
            else:
                lr = client.list_keys(id=int(sk.id), json_key=str(sk.json_key or ""))
            times = [k.time for k in getattr(lr, "keys", [])]
            if not any(abs(want_t - t) < 1e-6 for t in times):
                missing.append({"id": int(sk.id), "json_key": sk.json_key, "time": want_t})
        report["verify"]["missing_keys"] = missing
    return report
