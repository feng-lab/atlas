from pathlib import Path

REPO_SENTINELS: tuple[Path, ...] = (
    Path("src/atlas/CMakeLists.txt"),
    Path("docs/AGENTS_GUIDE.md"),
)


def find_repo_root(start: Path | None = None, *, max_up: int = 5) -> Path | None:
    cur = (start or Path.cwd()).resolve()
    for _ in range(max_up + 1):
        if all((cur / s).exists() for s in REPO_SENTINELS):
            return cur
        if cur.parent == cur:
            break
        cur = cur.parent
    return None
