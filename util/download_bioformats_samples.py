#!/usr/bin/env python3
"""Download public Bio-Formats validation data from OME sample images."""

from __future__ import annotations

import argparse
import datetime as _dt
import html
import html.parser
import json
import logging
import pathlib
import posixpath
import re
import urllib.error
import urllib.parse
import urllib.request

import download_utils


DEFAULT_ROOT_URL = "https://downloads.openmicroscopy.org/images/"
DEFAULT_OUTPUT_DIR = str(pathlib.Path.home() / "Documents" / "omeimages")
USER_AGENT = "AtlasBioFormatsBreadthDownloader/1.0"

logger = logging.getLogger(__name__)

EXCLUDED_SUFFIXES = {
    ".7z",
    ".bz2",
    ".cfg",
    ".csv",
    ".doc",
    ".docx",
    ".gz",
    ".html",
    ".ini",
    ".jar",
    ".json",
    ".log",
    ".m",
    ".md",
    ".pdf",
    ".py",
    ".rar",
    ".sha",
    ".tar",
    ".txt",
    ".xls",
    ".xlsx",
    ".xml",
    ".zip",
}

EXCLUDED_FILENAMES = {
    "copying",
    "license",
    "license.txt",
    "readme",
    "readme.txt",
}


class LinkParser(html.parser.HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.links: list[str] = []

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        if tag.lower() != "a":
            return
        for name, value in attrs:
            if name.lower() == "href" and value:
                self.links.append(value)


INDEX_LINE_RE = re.compile(
    r'<a\s+href="(?P<href>[^"]+)">.*?</a>\s+\d{2}-[A-Za-z]{3}-\d{4}\s+\d{2}:\d{2}\s+(?P<size>[0-9-]+)'
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root-url",
        default=DEFAULT_ROOT_URL,
        help=f"OME image root URL. Default: {DEFAULT_ROOT_URL}",
    )
    parser.add_argument(
        "--output-dir",
        default=DEFAULT_OUTPUT_DIR,
        help="Destination directory. Default: $HOME/Documents/omeimages",
    )
    parser.add_argument(
        "--mode",
        choices=("sample", "all"),
        default="sample",
        help="sample selects a small breadth corpus; all mirrors every non-hidden file under the OME image tree.",
    )
    parser.add_argument(
        "--per-format",
        type=int,
        default=3,
        help="Sample mode only: maximum files per top-level format.",
    )
    parser.add_argument(
        "--max-file-mib",
        type=int,
        default=None,
        help="Skip files larger than this many MiB. 0 means unlimited. Default: 512 in sample mode, unlimited in all mode.",
    )
    parser.add_argument(
        "--max-total-gib",
        type=float,
        default=None,
        help="Stop planning after this many GiB. 0 means unlimited. Default: 20 in sample mode, unlimited in all mode.",
    )
    parser.add_argument(
        "--max-depth",
        type=int,
        default=None,
        help="Maximum directory depth below each format root. 0 means unlimited. Default: 4 in sample mode, unlimited in all mode.",
    )
    parser.add_argument(
        "--max-pages-per-format",
        type=int,
        default=None,
        help="Maximum directories to crawl under each top-level format. 0 means unlimited. Default: 50 in sample mode, unlimited in all mode.",
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=60,
        help="Timeout in seconds for directory listings and HEAD requests.",
    )
    parser.add_argument(
        "--hash",
        choices=("auto", "none", "complete"),
        default="auto",
        help="Record SHA-256 hashes. auto hashes sample mode only; complete hashes every completed file.",
    )
    parser.add_argument("--manifest-name", default="manifest.json")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    if args.mode == "sample" and args.per_format <= 0:
        raise SystemExit("--per-format must be positive in sample mode")
    if args.max_file_mib is None:
        args.max_file_mib = 512 if args.mode == "sample" else 0
    if args.max_total_gib is None:
        args.max_total_gib = 20.0 if args.mode == "sample" else 0.0
    if args.max_depth is None:
        args.max_depth = 4 if args.mode == "sample" else 0
    if args.max_pages_per_format is None:
        args.max_pages_per_format = 50 if args.mode == "sample" else 0
    if args.max_file_mib < 0:
        raise SystemExit("--max-file-mib must be >= 0")
    if args.max_total_gib < 0:
        raise SystemExit("--max-total-gib must be >= 0")
    if args.max_depth < 0:
        raise SystemExit("--max-depth must be >= 0")
    if args.max_pages_per_format < 0:
        raise SystemExit("--max-pages-per-format must be >= 0")
    return args


def read_url(url: str, timeout: int) -> bytes:
    def read_once() -> bytes:
        request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return response.read()

    return download_utils.call_with_backoff(
        read_once,
        retries=3,
        backoff_in_seconds=1,
        max_backoff_seconds=8,
        operation_name=f"read directory listing {url}",
    )


def head_size(url: str, timeout: int) -> int | None:
    request = urllib.request.Request(
        url, method="HEAD", headers={"User-Agent": USER_AGENT}
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            value = response.headers.get("Content-Length")
            return int(value) if value else None
    except (OSError, urllib.error.HTTPError):
        return None


def list_links(url: str, timeout: int) -> list[str]:
    parser = LinkParser()
    parser.feed(read_url(url, timeout).decode("utf-8", errors="replace"))
    result: list[str] = []
    for href in parser.links:
        if href in {"../", "./"}:
            continue
        joined = urllib.parse.urljoin(url, href)
        if not joined.startswith(url):
            continue
        result.append(joined)
    return sorted(set(result))


def list_entries(url: str, timeout: int) -> list[tuple[str, int | None]]:
    text = read_url(url, timeout).decode("utf-8", errors="replace")
    entries: list[tuple[str, int | None]] = []
    for line in text.splitlines():
        match = INDEX_LINE_RE.search(line)
        if not match:
            continue
        href = html.unescape(match.group("href"))
        if href in {"../", "./"}:
            continue
        joined = urllib.parse.urljoin(url, href)
        if not joined.startswith(url):
            continue
        size_text = match.group("size")
        entries.append((joined, None if size_text == "-" else int(size_text)))
    if entries:
        return sorted(set(entries), key=lambda entry: entry[0])
    return [(link, None) for link in list_links(url, timeout)]


def filename_suffix(path: str) -> str:
    lower = path.lower()
    for suffix in (".ome.tiff", ".ome.tif", ".tar.gz"):
        if lower.endswith(suffix):
            return suffix
    return pathlib.PurePosixPath(lower).suffix


def is_candidate_file(url: str, *, include_sidecars: bool = False) -> bool:
    parsed = urllib.parse.urlparse(url)
    name = urllib.parse.unquote(posixpath.basename(parsed.path))
    if not name or name.startswith("."):
        return False
    if include_sidecars:
        return True
    if name.lower() in EXCLUDED_FILENAMES:
        return False
    return filename_suffix(name) not in EXCLUDED_SUFFIXES


def safe_relative_path(
    format_name: str, url: str, format_url: str
) -> pathlib.PurePosixPath:
    rel = urllib.parse.unquote(
        urllib.parse.urlparse(url).path.removeprefix(
            urllib.parse.urlparse(format_url).path
        )
    )
    parts = [
        part for part in pathlib.PurePosixPath(rel).parts if part not in {"", ".", ".."}
    ]
    return pathlib.PurePosixPath(format_name, *parts)


def optional_mib_to_bytes(value: int) -> int | None:
    return None if value <= 0 else value * 1024 * 1024


def optional_gib_to_bytes(value: float) -> int | None:
    return None if value <= 0 else int(value * 1024 * 1024 * 1024)


def within_optional_limit(value: int, limit: int | None) -> bool:
    return limit is None or value <= limit


def can_descend(depth: int, max_depth: int) -> bool:
    return max_depth == 0 or depth < max_depth


def page_limit_reached(pages: int, max_pages: int) -> bool:
    return max_pages > 0 and pages >= max_pages


def collect_candidates(
    format_name: str,
    format_url: str,
    args: argparse.Namespace,
    max_file_bytes: int | None,
) -> list[dict[str, object]]:
    queue: list[tuple[str, int]] = [(format_url, 0)]
    pages = 0
    candidates: list[dict[str, object]] = []
    sample_limit = args.per_format if args.mode == "sample" else None
    include_sidecars = args.mode == "all"

    while (
        queue
        and (sample_limit is None or len(candidates) < sample_limit)
        and not page_limit_reached(pages, args.max_pages_per_format)
    ):
        directory_url, depth = queue.pop(0)
        pages += 1
        try:
            entries = list_entries(directory_url, args.timeout)
        except Exception as exc:
            logger.warning("failed to list %s: %s", directory_url, exc)
            continue

        for link, listed_size in entries:
            if link.endswith("/"):
                if can_descend(depth, args.max_depth):
                    queue.append((link, depth + 1))
                continue
            if not is_candidate_file(link, include_sidecars=include_sidecars):
                continue
            size = (
                listed_size
                if listed_size is not None
                else head_size(link, args.timeout)
            )
            if size is None:
                logger.warning("skipping file with unknown size: %s", link)
                continue
            if not within_optional_limit(size, max_file_bytes):
                continue
            candidates.append(
                {
                    "format": format_name,
                    "url": link,
                    "relative_path": str(
                        safe_relative_path(format_name, link, format_url)
                    ),
                    "size": size,
                }
            )
            if sample_limit is not None and len(candidates) >= sample_limit:
                break

    return candidates


def should_hash(args: argparse.Namespace) -> bool:
    return args.hash == "complete" or (args.hash == "auto" and args.mode == "sample")


def download_sample(
    sample: dict[str, object],
    output_dir: pathlib.Path,
    dry_run: bool,
    hash_file: bool,
) -> dict[str, object]:
    relative_path = pathlib.PurePosixPath(str(sample["relative_path"]))
    dest = output_dir.joinpath(*relative_path.parts)
    size = int(sample["size"])
    sample = dict(sample)
    sample["path"] = str(dest)

    if dry_run:
        sample["status"] = "planned"
        return sample

    dest.parent.mkdir(parents=True, exist_ok=True)
    previous_size = dest.stat().st_size if dest.exists() else 0
    ok = download_utils.download_file_with_resume(
        str(sample["url"]), "", str(dest), expected_size=size
    )
    if not ok:
        raise RuntimeError(f"failed to download {sample['url']}")
    if hash_file:
        sample["sha256"] = download_utils.calculate_checksum(dest)
    if previous_size == size:
        sample["status"] = "exists"
    elif previous_size > 0:
        sample["status"] = "resumed"
    else:
        sample["status"] = "downloaded"
    return sample


def build_manifest(
    *,
    args: argparse.Namespace,
    root_url: str,
    records: list[dict[str, object]],
    planned_count: int,
    planned_total_bytes: int,
) -> dict[str, object]:
    return {
        "generated_at": _dt.datetime.now(tz=_dt.timezone.utc).isoformat(),
        "root_url": root_url,
        "parameters": {
            "mode": args.mode,
            "per_format": args.per_format if args.mode == "sample" else None,
            "max_file_mib": args.max_file_mib,
            "max_total_gib": args.max_total_gib,
            "max_depth": args.max_depth,
            "max_pages_per_format": args.max_pages_per_format,
            "hash": args.hash,
        },
        "planned_count": planned_count,
        "planned_total_bytes": planned_total_bytes,
        "file_count": len(records),
        "sample_count": len(records),
        "total_bytes": sum(int(sample["size"]) for sample in records),
        "samples": records,
    }


def write_manifest(manifest_path: pathlib.Path, manifest: dict[str, object]) -> None:
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = manifest_path.with_suffix(manifest_path.suffix + ".tmp")
    tmp_path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    tmp_path.replace(manifest_path)


def main() -> int:
    logging.basicConfig(level=logging.INFO, format="%(message)s")
    args = parse_args()
    max_file_bytes = optional_mib_to_bytes(args.max_file_mib)
    max_total_bytes = optional_gib_to_bytes(args.max_total_gib)
    output_dir = pathlib.Path(args.output_dir)

    root_url = args.root_url if args.root_url.endswith("/") else args.root_url + "/"
    top_links = [
        link for link in list_links(root_url, args.timeout) if link.endswith("/")
    ]
    planned: list[dict[str, object]] = []
    total_bytes = 0

    for format_url in top_links:
        format_name = urllib.parse.unquote(
            posixpath.basename(urllib.parse.urlparse(format_url.rstrip("/")).path)
        )
        if not format_name:
            continue
        print(f"planning {format_name}...", flush=True)
        candidates = collect_candidates(format_name, format_url, args, max_file_bytes)
        print(f"  selected {len(candidates)} file(s)", flush=True)
        for candidate in candidates:
            size = int(candidate["size"])
            if max_total_bytes is not None and total_bytes + size > max_total_bytes:
                break
            planned.append(candidate)
            total_bytes += size
        if max_total_bytes is not None and total_bytes >= max_total_bytes:
            break

    output_dir.mkdir(parents=True, exist_ok=True)
    manifest_path = output_dir / args.manifest_name
    hash_file = should_hash(args)

    if args.dry_run:
        completed = [
            download_sample(sample, output_dir, True, hash_file) for sample in planned
        ]
        manifest = build_manifest(
            args=args,
            root_url=root_url,
            records=completed,
            planned_count=len(planned),
            planned_total_bytes=total_bytes,
        )
        print(json.dumps(manifest, indent=2, sort_keys=True))
        print(
            f"planned {len(planned)} file(s), {total_bytes / (1024 * 1024 * 1024):.2f} GiB"
        )
        return 0

    completed: list[dict[str, object]] = []
    write_manifest(
        manifest_path,
        build_manifest(
            args=args,
            root_url=root_url,
            records=completed,
            planned_count=len(planned),
            planned_total_bytes=total_bytes,
        ),
    )

    for index, sample in enumerate(planned, start=1):
        print(
            f"[{index}/{len(planned)}] {sample['relative_path']} ({int(sample['size']) / (1024 * 1024):.1f} MiB)",
            flush=True,
        )
        completed.append(download_sample(sample, output_dir, False, hash_file))
        write_manifest(
            manifest_path,
            build_manifest(
                args=args,
                root_url=root_url,
                records=completed,
                planned_count=len(planned),
                planned_total_bytes=total_bytes,
            ),
        )

    print(f"wrote {len(completed)} file entries to {manifest_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
