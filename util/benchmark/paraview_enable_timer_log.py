"""Enable ParaView's global timer log on startup.

Use this with the ParaView GUI:

    /Applications/ParaView-6.1.0-RC1.app/Contents/MacOS/paraview \
      --script /Users/feng/code/atlas/util/benchmark/paraview_enable_timer_log.py

This uses the same "misc/TimerLog" proxy as Tools > Timer Log, so it affects
the client and any connected server processes for the active session.
"""

from paraview import servermanager
from paraview.servermanager import vtkSMPropertyHelper


MAX_ENTRIES = 1_000_000


def main() -> None:
    pxm = servermanager.ProxyManager()
    timer_log = pxm.NewProxy("misc", "TimerLog")
    vtkSMPropertyHelper(timer_log, "Enable").Set(1)
    vtkSMPropertyHelper(timer_log, "MaxEntries").Set(MAX_ENTRIES)
    timer_log.UpdateVTKObjects()
    print(f"ParaView timer log enabled; MaxEntries={MAX_ENTRIES}")


main()
