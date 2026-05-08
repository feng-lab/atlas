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
import time
import urllib.error
import urllib.parse
import urllib.request

import download_utils
from logger import setup_logger


DEFAULT_ROOT_URL = "https://downloads.openmicroscopy.org/images/"
DEFAULT_OUTPUT_DIR = str(pathlib.Path.home() / "Documents" / "omeimages")
USER_AGENT = "AtlasBioFormatsBreadthDownloader/1.0"
BYTES_PER_MIB = 1024 * 1024
BYTES_PER_GIB = 1024 * 1024 * 1024

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
        default=None,
        help=(
            "Maximum files per top-level format. In all mode, 0 means "
            "unlimited; sample mode requires a positive value. Default: 3 in "
            "sample mode, unlimited in all mode."
        ),
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

    if args.per_format is None:
        args.per_format = 3 if args.mode == "sample" else 0
    if args.mode == "sample" and args.per_format <= 0:
        raise SystemExit("--per-format must be positive in sample mode")
    if args.mode == "all" and args.per_format < 0:
        raise SystemExit("--per-format must be >= 0")
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


def normalize_relative_path(value: object) -> pathlib.PurePosixPath:
    if not isinstance(value, str) or not value:
        raise ValueError("relative_path must be a non-empty string")
    path = pathlib.PurePosixPath(value)
    if path.is_absolute() or any(part in {"", ".", ".."} for part in path.parts):
        raise ValueError(
            f"relative_path must stay inside the output directory: {value!r}"
        )
    return path


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


def per_format_limit(args: argparse.Namespace) -> int | None:
    return None if args.per_format <= 0 else args.per_format


def format_byte_count(size_bytes: int) -> str:
    if size_bytes >= BYTES_PER_GIB:
        return f"{size_bytes / BYTES_PER_GIB:.2f} GiB"
    if size_bytes >= BYTES_PER_MIB:
        return f"{size_bytes / BYTES_PER_MIB:.1f} MiB"
    return f"{size_bytes} B"


def planning_stop_reason(
    *,
    queue: list[tuple[str, int]],
    candidates: list[dict[str, object]],
    format_limit: int | None,
    pages: int,
    max_pages: int,
) -> str:
    if format_limit is not None and len(candidates) >= format_limit:
        return "per-format file limit reached"
    if page_limit_reached(pages, max_pages):
        return "page limit reached"
    if not queue:
        return "directory queue exhausted"
    return "stopped"


def collect_candidates(
    format_name: str,
    format_url: str,
    args: argparse.Namespace,
    max_file_bytes: int | None,
) -> list[dict[str, object]]:
    queue: list[tuple[str, int]] = [(format_url, 0)]
    pages = 0
    candidates: list[dict[str, object]] = []
    format_limit = per_format_limit(args)
    include_sidecars = args.mode == "all"
    start_time = time.monotonic()
    candidate_bytes = 0
    directories_queued = 0
    directories_skipped_by_depth = 0
    skipped_non_candidates = 0
    skipped_unknown_size = 0
    skipped_too_large = 0

    logger.info(
        "planning %s: crawl start url=%s mode=%s max_depth=%s "
        "max_pages_per_format=%s max_file=%s per_format=%s",
        format_name,
        format_url,
        args.mode,
        "unlimited" if args.max_depth == 0 else args.max_depth,
        "unlimited" if args.max_pages_per_format == 0 else args.max_pages_per_format,
        "unlimited" if max_file_bytes is None else format_byte_count(max_file_bytes),
        "unlimited" if format_limit is None else format_limit,
    )

    while (
        queue
        and (format_limit is None or len(candidates) < format_limit)
        and not page_limit_reached(pages, args.max_pages_per_format)
    ):
        directory_url, depth = queue.pop(0)
        pages += 1
        page_start_time = time.monotonic()
        logger.info(
            "planning %s: listing page=%d depth=%d queued=%d selected=%d "
            "selected_bytes=%s url=%s",
            format_name,
            pages,
            depth,
            len(queue),
            len(candidates),
            format_byte_count(candidate_bytes),
            directory_url,
        )
        try:
            entries = list_entries(directory_url, args.timeout)
        except Exception as exc:
            logger.warning("failed to list %s: %s", directory_url, exc)
            continue

        directory_entries = sum(1 for link, _ in entries if link.endswith("/"))
        file_entries = len(entries) - directory_entries
        logger.info(
            "planning %s: listed page=%d in %.1fs entries=%d dirs=%d files=%d",
            format_name,
            pages,
            time.monotonic() - page_start_time,
            len(entries),
            directory_entries,
            file_entries,
        )

        page_directories_queued = 0
        page_directories_skipped_by_depth = 0
        page_skipped_non_candidates = 0
        page_skipped_unknown_size = 0
        page_skipped_too_large = 0
        page_selected = 0
        page_selected_bytes = 0

        for link, listed_size in entries:
            if link.endswith("/"):
                if can_descend(depth, args.max_depth):
                    queue.append((link, depth + 1))
                    directories_queued += 1
                    page_directories_queued += 1
                else:
                    directories_skipped_by_depth += 1
                    page_directories_skipped_by_depth += 1
                continue
            if not is_candidate_file(link, include_sidecars=include_sidecars):
                skipped_non_candidates += 1
                page_skipped_non_candidates += 1
                continue
            if listed_size is not None:
                size = listed_size
            else:
                head_start_time = time.monotonic()
                logger.info("planning %s: probing size url=%s", format_name, link)
                size = head_size(link, args.timeout)
                logger.info(
                    "planning %s: size probe finished in %.1fs size=%s url=%s",
                    format_name,
                    time.monotonic() - head_start_time,
                    "unknown" if size is None else format_byte_count(size),
                    link,
                )
            if size is None:
                skipped_unknown_size += 1
                page_skipped_unknown_size += 1
                logger.warning("skipping file with unknown size: %s", link)
                continue
            if not within_optional_limit(size, max_file_bytes):
                skipped_too_large += 1
                page_skipped_too_large += 1
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
            candidate_bytes += size
            page_selected += 1
            page_selected_bytes += size
            if format_limit is not None and len(candidates) >= format_limit:
                break

        logger.info(
            "planning %s: page=%d complete selected=%d selected_bytes=%s "
            "queued_dirs=%d skipped_dirs_by_depth=%d skipped_non_candidates=%d "
            "skipped_unknown_size=%d skipped_too_large=%d total_selected=%d "
            "total_selected_bytes=%s remaining_queue=%d elapsed=%.1fs",
            format_name,
            pages,
            page_selected,
            format_byte_count(page_selected_bytes),
            page_directories_queued,
            page_directories_skipped_by_depth,
            page_skipped_non_candidates,
            page_skipped_unknown_size,
            page_skipped_too_large,
            len(candidates),
            format_byte_count(candidate_bytes),
            len(queue),
            time.monotonic() - start_time,
        )

    logger.info(
        "planning %s: finished reason=%s pages=%d queued_dirs=%d "
        "skipped_dirs_by_depth=%d selected=%d selected_bytes=%s "
        "skipped_non_candidates=%d skipped_unknown_size=%d skipped_too_large=%d "
        "elapsed=%.1fs",
        format_name,
        planning_stop_reason(
            queue=queue,
            candidates=candidates,
            format_limit=format_limit,
            pages=pages,
            max_pages=args.max_pages_per_format,
        ),
        pages,
        directories_queued,
        directories_skipped_by_depth,
        len(candidates),
        format_byte_count(candidate_bytes),
        skipped_non_candidates,
        skipped_unknown_size,
        skipped_too_large,
        time.monotonic() - start_time,
    )

    return candidates


def should_hash(args: argparse.Namespace) -> bool:
    return args.hash == "complete" or (args.hash == "auto" and args.mode == "sample")


def manifest_sample_record(sample: dict[str, object]) -> dict[str, object]:
    relative_path = normalize_relative_path(sample.get("relative_path"))
    url = sample.get("url")
    if not isinstance(url, str) or not url:
        raise ValueError(f"manifest record is missing url: {sample!r}")
    try:
        size = int(sample["size"])
    except (KeyError, TypeError, ValueError) as exc:
        raise ValueError(f"manifest record has invalid size: {sample!r}") from exc
    if size < 0:
        raise ValueError(f"manifest record has negative size: {sample!r}")

    return {
        "relative_path": str(relative_path),
        "url": url,
        "size": size,
    }


def load_planned_samples_from_manifest(
    manifest_path: pathlib.Path,
) -> list[dict[str, object]]:
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except OSError as exc:
        raise RuntimeError(
            f"failed to read existing manifest: {manifest_path}"
        ) from exc
    except json.JSONDecodeError as exc:
        raise RuntimeError(
            f"failed to parse existing manifest: {manifest_path}"
        ) from exc

    if not isinstance(manifest, dict):
        raise RuntimeError(f"manifest root must be an object: {manifest_path}")
    samples = manifest.get("samples")
    if not isinstance(samples, list):
        raise RuntimeError(f"manifest samples must be a list: {manifest_path}")

    planned_count = manifest.get("planned_count")
    if isinstance(planned_count, int) and planned_count != len(samples):
        logger.warning(
            "Existing manifest has planned_count=%d but samples=%d. "
            "Using the samples list as the saved plan; delete %s to replan.",
            planned_count,
            len(samples),
            manifest_path,
        )

    records: list[dict[str, object]] = []
    for index, sample in enumerate(samples, start=1):
        if not isinstance(sample, dict):
            raise RuntimeError(
                f"manifest sample {index} must be an object: {manifest_path}"
            )
        try:
            records.append(manifest_sample_record(sample))
        except ValueError as exc:
            raise RuntimeError(
                f"invalid manifest sample {index} in {manifest_path}: {exc}"
            ) from exc
    return records


def download_sample(
    sample: dict[str, object],
    output_dir: pathlib.Path,
    dry_run: bool,
    hash_file: bool,
) -> dict[str, object]:
    relative_path = normalize_relative_path(sample["relative_path"])
    dest = output_dir.joinpath(*relative_path.parts)
    size = int(sample["size"])
    sample = dict(sample)
    sample["path"] = str(dest)

    if dry_run:
        sample["status"] = "planned"
        return sample

    dest.parent.mkdir(parents=True, exist_ok=True)
    existed_before = dest.exists()
    previous_size = dest.stat().st_size if existed_before else 0
    ok = download_utils.download_file_with_resume(
        str(sample["url"]), "", str(dest), expected_size=size
    )
    if not ok:
        raise RuntimeError(f"failed to download {sample['url']}")
    if hash_file:
        hash_start_time = time.monotonic()
        logger.info("hashing %s", dest)
        sample["sha256"] = download_utils.calculate_checksum(dest)
        logger.info(
            "hash complete in %.1fs for %s", time.monotonic() - hash_start_time, dest
        )
    if existed_before and previous_size == size:
        sample["status"] = "exists"
    elif size == 0:
        sample["status"] = "downloaded"
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
            "per_format": per_format_limit(args),
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
        "samples": [manifest_sample_record(sample) for sample in records],
    }


def write_manifest(manifest_path: pathlib.Path, manifest: dict[str, object]) -> None:
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = manifest_path.with_suffix(manifest_path.suffix + ".tmp")
    tmp_path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    tmp_path.replace(manifest_path)


def main() -> int:
    setup_logger()
    args = parse_args()
    max_file_bytes = optional_mib_to_bytes(args.max_file_mib)
    max_total_bytes = optional_gib_to_bytes(args.max_total_gib)
    output_dir = pathlib.Path(args.output_dir)
    manifest_path = output_dir / args.manifest_name

    root_url = args.root_url if args.root_url.endswith("/") else args.root_url + "/"
    logger.info("Bio-Formats sample downloader started")
    logger.info(
        "configuration: root_url=%s output_dir=%s mode=%s dry_run=%s hash=%s "
        "timeout=%ds",
        root_url,
        output_dir,
        args.mode,
        args.dry_run,
        args.hash,
        args.timeout,
    )
    logger.info(
        "planning limits: max_file=%s max_total=%s max_depth=%s "
        "max_pages_per_format=%s per_format=%s",
        "unlimited" if max_file_bytes is None else format_byte_count(max_file_bytes),
        "unlimited" if max_total_bytes is None else format_byte_count(max_total_bytes),
        "unlimited" if args.max_depth == 0 else args.max_depth,
        "unlimited" if args.max_pages_per_format == 0 else args.max_pages_per_format,
        "unlimited" if per_format_limit(args) is None else per_format_limit(args),
    )

    if manifest_path.exists():
        logger.info(
            "using existing manifest plan: %s. Delete this file to replan.",
            manifest_path,
        )
        planned = load_planned_samples_from_manifest(manifest_path)
        total_bytes = sum(int(sample["size"]) for sample in planned)
        logger.info(
            "loaded manifest plan: %d file(s), %s",
            len(planned),
            format_byte_count(total_bytes),
        )
    else:
        logger.info("no manifest found at %s; planning from OME index", manifest_path)
        logger.info("reading top-level OME image index: %s", root_url)
        root_index_start_time = time.monotonic()
        top_links = [
            link for link in list_links(root_url, args.timeout) if link.endswith("/")
        ]
        logger.info(
            "found %d top-level format directories in %.1fs",
            len(top_links),
            time.monotonic() - root_index_start_time,
        )
        planned: list[dict[str, object]] = []
        total_bytes = 0
        total_limit_reached = False

        for format_index, format_url in enumerate(top_links, start=1):
            format_name = urllib.parse.unquote(
                posixpath.basename(urllib.parse.urlparse(format_url.rstrip("/")).path)
            )
            if not format_name:
                continue
            logger.info(
                "planning format %d/%d: %s", format_index, len(top_links), format_name
            )
            candidates = collect_candidates(
                format_name, format_url, args, max_file_bytes
            )
            format_added = 0
            format_added_bytes = 0
            for candidate in candidates:
                size = int(candidate["size"])
                if max_total_bytes is not None and total_bytes + size > max_total_bytes:
                    logger.info(
                        "global planning limit reached before %s: planned=%s "
                        "next_file=%s limit=%s",
                        candidate["relative_path"],
                        format_byte_count(total_bytes),
                        format_byte_count(size),
                        format_byte_count(max_total_bytes),
                    )
                    total_limit_reached = True
                    break
                planned.append(manifest_sample_record(candidate))
                total_bytes += size
                format_added += 1
                format_added_bytes += size
            logger.info(
                "planning format %d/%d complete: %s selected=%d added_to_plan=%d "
                "added_bytes=%s total_planned=%d total_bytes=%s",
                format_index,
                len(top_links),
                format_name,
                len(candidates),
                format_added,
                format_byte_count(format_added_bytes),
                len(planned),
                format_byte_count(total_bytes),
            )
            if total_limit_reached or (
                max_total_bytes is not None and total_bytes >= max_total_bytes
            ):
                break

    output_dir.mkdir(parents=True, exist_ok=True)
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
        logger.info(
            "dry run complete: planned %d file(s), %s",
            len(planned),
            format_byte_count(total_bytes),
        )
        return 0

    completed: list[dict[str, object]] = []
    logger.info(
        "download plan complete: %d file(s), %s manifest=%s",
        len(planned),
        format_byte_count(total_bytes),
        manifest_path,
    )
    if not manifest_path.exists():
        write_manifest(
            manifest_path,
            build_manifest(
                args=args,
                root_url=root_url,
                records=planned,
                planned_count=len(planned),
                planned_total_bytes=total_bytes,
            ),
        )
        logger.info("wrote manifest plan to %s", manifest_path)

    completed_bytes = 0
    for index, sample in enumerate(planned, start=1):
        size = int(sample["size"])
        file_start_time = time.monotonic()
        logger.info(
            "downloading file %d/%d: %s size=%s completed=%s/%s",
            index,
            len(planned),
            sample["relative_path"],
            format_byte_count(size),
            format_byte_count(completed_bytes),
            format_byte_count(total_bytes),
        )
        completed.append(download_sample(sample, output_dir, False, hash_file))
        completed_bytes += size
        overall_progress = (
            completed_bytes / total_bytes * 100.0 if total_bytes else 100.0
        )
        logger.info(
            "finished file %d/%d: %s status=%s elapsed=%.1fs completed=%s/%s (%.1f%%)",
            index,
            len(planned),
            sample["relative_path"],
            completed[-1]["status"],
            time.monotonic() - file_start_time,
            format_byte_count(completed_bytes),
            format_byte_count(total_bytes),
            overall_progress,
        )

    logger.info("download run complete: %d file(s) processed", len(completed))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
