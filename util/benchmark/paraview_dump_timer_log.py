from __future__ import annotations

import json
import time
from pathlib import Path

from vtkmodules.vtkCommonSystem import vtkTimerLog


INTERACTIVE_RENDER_EVENT = "Interactive Render"
STILL_RENDER_EVENT = "Still Render"


def _collect_timer_events() -> list[dict[str, object]]:
    events: list[dict[str, object]] = []
    for index in range(int(vtkTimerLog.GetNumberOfEvents())):
        events.append(
            {
                "index": index,
                "name": str(vtkTimerLog.GetEventString(index)),
                "event_type": int(vtkTimerLog.GetEventType(index)),
                "indent": int(vtkTimerLog.GetEventIndent(index)),
                "wall_time_seconds": float(vtkTimerLog.GetEventWallTime(index)),
            }
        )
    return events


def _collect_timer_scopes(events: list[dict[str, object]]) -> list[dict[str, object]]:
    scopes: list[dict[str, object]] = []
    stack: list[dict[str, object]] = []
    for event in events:
        event_type = int(event["event_type"])
        if event_type == 1:
            stack.append(event)
            continue
        if event_type != 2 or not stack:
            continue
        start_event = stack.pop()
        start_seconds = float(start_event["wall_time_seconds"])
        end_seconds = float(event["wall_time_seconds"])
        scopes.append(
            {
                "name": str(start_event["name"]),
                "start_seconds": start_seconds,
                "end_seconds": end_seconds,
                "duration_ms": max(0.0, (end_seconds - start_seconds) * 1000.0),
                "indent": int(start_event["indent"]),
            }
        )
    return scopes


def main() -> int:
    out_dir = Path("/tmp")
    stamp = time.strftime("%Y%m%d_%H%M%S")
    txt_path = out_dir / f"paraview_live_manual_timer_log_{stamp}.txt"
    json_path = out_dir / f"paraview_live_manual_timer_summary_{stamp}.json"
    latest_txt = out_dir / "paraview_live_manual_timer_log_latest.txt"
    latest_json = out_dir / "paraview_live_manual_timer_summary_latest.json"

    vtkTimerLog.DumpLog(str(txt_path))
    events = _collect_timer_events()
    scopes = _collect_timer_scopes(events)

    render_scopes = [
        scope
        for scope in scopes
        if scope["name"] in {INTERACTIVE_RENDER_EVENT, STILL_RENDER_EVENT}
    ]
    interactive_scopes = [
        scope for scope in render_scopes if scope["name"] == INTERACTIVE_RENDER_EVENT
    ]
    still_scopes = [
        scope for scope in render_scopes if scope["name"] == STILL_RENDER_EVENT
    ]

    summary = {
        "event_count": len(events),
        "interactive_render_count": len(interactive_scopes),
        "still_render_count": len(still_scopes),
        "last_interactive_renders": interactive_scopes[-10:],
        "last_still_renders": still_scopes[-10:],
        "raw_log_path": str(txt_path),
    }

    json_text = json.dumps(summary, indent=2) + "\n"
    json_path.write_text(json_text, encoding="utf-8")
    latest_json.write_text(json_text, encoding="utf-8")
    latest_txt.write_text(txt_path.read_text(encoding="utf-8"), encoding="utf-8")
    print(f"Wrote timer log dump to {txt_path}")
    print(f"Wrote timer summary to {json_path}")
    return 0


main()
