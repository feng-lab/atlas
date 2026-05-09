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

# Driver files for formats where selecting only the first N files can produce an
# unusable partial dataset. When one of these drivers is selected, the planner
# downloads the containing dataset directory atomically, even if that makes the
# per-format file count exceed --per-format.
DATASET_DRIVER_SUFFIXES = {
    "BDV": (".xml",),
    "CV7000": (".wpi",),
    "CellSens": (".vsi",),
    "HCS": (
        ".xdce",
        "/index.idx.xml",
        "/index.ref.xml",
        "/index.xml",
    ),
    "Hamamatsu-VMS": (".vms",),
    "ICS": (".ics",),
    "InCell2000": (".xdce",),
    "JDCE": (".jdce",),
    "Leica-XLEF": (".xlef",),
    "MetaXpress": (".htd",),
    "Metamorph": (".nd",),
    "Micro-Manager": ("/metadata.txt", "_metadata.txt", "/acqusition.xml"),
    "NRRD": (".nhdr",),
    "OME-XML": (".ome.xml",),
    "Olympus-FluoView": (".oif",),
    "PerkinElmer-Columbus": ("/measurementindex.columbusidx.xml",),
    "PerkinElmer-Operetta": (
        "/index.idx.xml",
        "/index.ref.xml",
        "/index.xml",
    ),
    "SPC-FIFO": (".set",),
    "ScanR": ("/experiment_descriptor.xml",),
}

DRIVER_ONLY_FORMATS = {
    "BDV",
    "CV7000",
    "CellSens",
    "Hamamatsu-VMS",
    "ICS",
    "InCell2000",
    "JDCE",
    "Leica-XLEF",
    "MetaXpress",
    "Metamorph",
    "Micro-Manager",
    "NRRD",
    "OME-XML",
    "Olympus-FluoView",
    "PerkinElmer-Columbus",
    "PerkinElmer-Operetta",
    "SPC-FIFO",
    "ScanR",
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
            "Maximum files per top-level format in the derived download plan. "
            "In all mode, 0 means unlimited; sample mode requires a positive "
            "value. Default: 3 in sample mode, unlimited in all mode."
        ),
    )
    parser.add_argument(
        "--max-file-mib",
        type=int,
        default=None,
        help="Exclude files larger than this many MiB from the derived download plan. 0 means unlimited. Default: 512 in sample mode, unlimited in all mode.",
    )
    parser.add_argument(
        "--max-total-gib",
        type=float,
        default=None,
        help="Stop deriving the download plan after this many GiB. 0 means unlimited. Default: 20 in sample mode, unlimited in all mode.",
    )
    parser.add_argument(
        "--max-depth",
        type=int,
        default=None,
        help="Maximum directory depth below each format root in the derived download plan. 0 means unlimited. Default: 4 in sample mode, unlimited in all mode.",
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
    parser.add_argument("--full-manifest-name", default="full_manifest.json")
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
    if args.max_file_mib < 0:
        raise SystemExit("--max-file-mib must be >= 0")
    if args.max_total_gib < 0:
        raise SystemExit("--max-total-gib must be >= 0")
    if args.max_depth < 0:
        raise SystemExit("--max-depth must be >= 0")
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


def per_format_limit(args: argparse.Namespace) -> int | None:
    return None if args.per_format <= 0 else args.per_format


def format_byte_count(size_bytes: int) -> str:
    if size_bytes >= BYTES_PER_GIB:
        return f"{size_bytes / BYTES_PER_GIB:.2f} GiB"
    if size_bytes >= BYTES_PER_MIB:
        return f"{size_bytes / BYTES_PER_MIB:.1f} MiB"
    return f"{size_bytes} B"


def collection_stop_reason(
    *,
    queue: list[tuple[str, int]],
    records: list[dict[str, object]],
    format_limit: int | None,
) -> str:
    if format_limit is not None and len(records) >= format_limit:
        return "per-format record limit reached"
    if not queue:
        return "directory queue exhausted"
    return "stopped"


def collect_inventory_records(
    format_name: str,
    format_url: str,
    args: argparse.Namespace,
    max_file_bytes: int | None,
) -> list[dict[str, object]]:
    queue: list[tuple[str, int]] = [(format_url, 0)]
    pages = 0
    records: list[dict[str, object]] = []
    format_limit = per_format_limit(args)
    include_sidecars = args.mode == "all"
    start_time = time.monotonic()
    collected_bytes = 0
    directories_queued = 0
    directories_skipped_by_depth = 0
    skipped_filtered_files = 0
    skipped_unknown_size = 0
    skipped_too_large = 0

    logger.info(
        "collecting %s: crawl start url=%s mode=%s max_depth=%s "
        "max_file=%s per_format=%s",
        format_name,
        format_url,
        args.mode,
        "unlimited" if args.max_depth == 0 else args.max_depth,
        "unlimited" if max_file_bytes is None else format_byte_count(max_file_bytes),
        "unlimited" if format_limit is None else format_limit,
    )

    while queue and (format_limit is None or len(records) < format_limit):
        directory_url, depth = queue.pop(0)
        pages += 1
        page_start_time = time.monotonic()
        logger.info(
            "collecting %s: listing page=%d depth=%d queued=%d collected=%d "
            "collected_bytes=%s url=%s",
            format_name,
            pages,
            depth,
            len(queue),
            len(records),
            format_byte_count(collected_bytes),
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
            "collecting %s: listed page=%d in %.1fs entries=%d dirs=%d files=%d",
            format_name,
            pages,
            time.monotonic() - page_start_time,
            len(entries),
            directory_entries,
            file_entries,
        )

        page_directories_queued = 0
        page_directories_skipped_by_depth = 0
        page_skipped_filtered_files = 0
        page_skipped_unknown_size = 0
        page_skipped_too_large = 0
        page_collected = 0
        page_collected_bytes = 0

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
                skipped_filtered_files += 1
                page_skipped_filtered_files += 1
                continue
            if listed_size is not None:
                size = listed_size
            else:
                head_start_time = time.monotonic()
                logger.info("collecting %s: probing size url=%s", format_name, link)
                size = head_size(link, args.timeout)
                logger.info(
                    "collecting %s: size probe finished in %.1fs size=%s url=%s",
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
            records.append(
                {
                    "format": format_name,
                    "url": link,
                    "relative_path": str(
                        safe_relative_path(format_name, link, format_url)
                    ),
                    "size": size,
                }
            )
            collected_bytes += size
            page_collected += 1
            page_collected_bytes += size
            if format_limit is not None and len(records) >= format_limit:
                break

        logger.info(
            "collecting %s: page=%d complete collected=%d collected_bytes=%s "
            "queued_dirs=%d skipped_dirs_by_depth=%d skipped_filtered_files=%d "
            "skipped_unknown_size=%d skipped_too_large=%d total_collected=%d "
            "total_collected_bytes=%s remaining_queue=%d elapsed=%.1fs",
            format_name,
            pages,
            page_collected,
            format_byte_count(page_collected_bytes),
            page_directories_queued,
            page_directories_skipped_by_depth,
            page_skipped_filtered_files,
            page_skipped_unknown_size,
            page_skipped_too_large,
            len(records),
            format_byte_count(collected_bytes),
            len(queue),
            time.monotonic() - start_time,
        )

    logger.info(
        "collecting %s: finished reason=%s pages=%d queued_dirs=%d "
        "skipped_dirs_by_depth=%d collected=%d collected_bytes=%s "
        "skipped_filtered_files=%d skipped_unknown_size=%d skipped_too_large=%d "
        "elapsed=%.1fs",
        format_name,
        collection_stop_reason(
            queue=queue,
            records=records,
            format_limit=format_limit,
        ),
        pages,
        directories_queued,
        directories_skipped_by_depth,
        len(records),
        format_byte_count(collected_bytes),
        skipped_filtered_files,
        skipped_unknown_size,
        skipped_too_large,
        time.monotonic() - start_time,
    )

    return records


def full_manifest_inventory_args(args: argparse.Namespace) -> argparse.Namespace:
    return argparse.Namespace(
        mode="all",
        per_format=0,
        max_depth=0,
        timeout=args.timeout,
    )


def relative_path_format_name(relative_path: pathlib.PurePosixPath) -> str:
    if not relative_path.parts:
        raise ValueError("relative_path must include a top-level format directory")
    return relative_path.parts[0]


def order_records_for_download(
    records: list[dict[str, object]],
) -> list[dict[str, object]]:
    format_counts: dict[str, int] = {}
    format_first_index: dict[str, int] = {}
    indexed_records: list[tuple[int, dict[str, object], str]] = []
    for index, record in enumerate(records):
        relative_path = normalize_relative_path(record["relative_path"])
        format_name = relative_path_format_name(relative_path)
        indexed_records.append((index, record, format_name))
        format_counts[format_name] = format_counts.get(format_name, 0) + 1
        format_first_index.setdefault(format_name, index)

    # Finish smaller formats first so partial runs maximize format coverage.
    def sort_key(
        indexed_record: tuple[int, dict[str, object], str],
    ) -> tuple[int, int, int]:
        index, _, format_name = indexed_record
        return (format_counts[format_name], format_first_index[format_name], index)

    return [record for _, record, _ in sorted(indexed_records, key=sort_key)]


def relative_path_parent_depth(relative_path: pathlib.PurePosixPath) -> int:
    return max(len(relative_path.parts) - 2, 0)


def relative_path_is_under(
    relative_path: pathlib.PurePosixPath, prefix: pathlib.PurePosixPath
) -> bool:
    return relative_path.parts[: len(prefix.parts)] == prefix.parts


def has_path_suffix(
    relative_path: pathlib.PurePosixPath, suffixes: tuple[str, ...]
) -> bool:
    lower_path = relative_path.as_posix().lower()
    return any(lower_path.endswith(suffix) for suffix in suffixes)


def dataset_driver_group_prefix(
    relative_path: pathlib.PurePosixPath,
) -> pathlib.PurePosixPath | None:
    format_name = relative_path_format_name(relative_path)
    suffixes = DATASET_DRIVER_SUFFIXES.get(format_name)
    if suffixes is None or not has_path_suffix(relative_path, suffixes):
        return None
    if len(relative_path.parts) < 2:
        return None

    if (
        format_name in {"HCS", "PerkinElmer-Operetta"}
        and len(relative_path.parts) >= 3
        and relative_path.parts[-2].lower() == "images"
    ):
        return pathlib.PurePosixPath(*relative_path.parts[:-2])
    return pathlib.PurePosixPath(*relative_path.parts[:-1])


def derive_plan_from_full_manifest(
    *,
    args: argparse.Namespace,
    full_records: list[dict[str, object]],
    max_file_bytes: int | None,
    max_total_bytes: int | None,
) -> tuple[list[dict[str, object]], int]:
    format_limit = per_format_limit(args)
    include_sidecars = args.mode == "all"
    normalized_records: list[
        tuple[dict[str, object], pathlib.PurePosixPath, str, int]
    ] = []
    records_by_format: dict[
        str,
        list[tuple[dict[str, object], pathlib.PurePosixPath, str, int]],
    ] = {}
    for sample in full_records:
        record = manifest_sample_record(sample)
        relative_path = normalize_relative_path(record["relative_path"])
        record_info = (
            record,
            relative_path,
            relative_path_format_name(relative_path),
            int(record["size"]),
        )
        normalized_records.append(record_info)
        records_by_format.setdefault(record_info[2], []).append(record_info)

    selected: list[dict[str, object]] = []
    selected_paths: set[str] = set()
    selected_bytes = 0
    per_format_counts: dict[str, int] = {}
    skipped_by_mode = 0
    skipped_by_depth = 0
    skipped_by_size = 0
    skipped_by_per_format = 0
    skipped_by_total_size = 0
    total_limit_reached = False
    dataset_groups_selected = 0
    dataset_group_files_selected = 0
    per_format_overrun_files = 0
    group_records_by_prefix: dict[
        pathlib.PurePosixPath,
        list[tuple[dict[str, object], pathlib.PurePosixPath, str, int]],
    ] = {}

    logger.info(
        "deriving download plan from full manifest: records=%d mode=%s "
        "max_file=%s max_total=%s max_depth=%s per_format=%s",
        len(normalized_records),
        args.mode,
        "unlimited" if max_file_bytes is None else format_byte_count(max_file_bytes),
        "unlimited" if max_total_bytes is None else format_byte_count(max_total_bytes),
        "unlimited" if args.max_depth == 0 else args.max_depth,
        "unlimited" if format_limit is None else format_limit,
    )

    def records_for_prefix(
        prefix: pathlib.PurePosixPath,
    ) -> list[tuple[dict[str, object], pathlib.PurePosixPath, str, int]]:
        cached = group_records_by_prefix.get(prefix)
        if cached is not None:
            return cached
        format_records = records_by_format.get(relative_path_format_name(prefix), [])
        records = [
            record_info
            for record_info in format_records
            if relative_path_is_under(record_info[1], prefix)
        ]
        group_records_by_prefix[prefix] = records
        return records

    def add_record_group(
        records: list[tuple[dict[str, object], pathlib.PurePosixPath, str, int]],
        *,
        label: str,
        complete_dataset_group: bool,
    ) -> int:
        nonlocal selected_bytes
        nonlocal skipped_by_size
        nonlocal skipped_by_per_format
        nonlocal skipped_by_total_size
        nonlocal total_limit_reached
        nonlocal per_format_overrun_files

        new_records = [
            record_info
            for record_info in records
            if str(record_info[1]) not in selected_paths
        ]
        if not new_records:
            return 0

        format_name = new_records[0][2]
        if any(record_info[2] != format_name for record_info in new_records):
            raise RuntimeError(f"dataset group spans multiple formats: {label}")

        if max_file_bytes is not None and any(
            not within_optional_limit(record_info[3], max_file_bytes)
            for record_info in new_records
        ):
            skipped_by_size += len(new_records)
            logger.info(
                "skipping complete dataset group with oversized file(s): %s files=%d",
                label,
                len(new_records),
            )
            return 0

        before_count = per_format_counts.get(format_name, 0)
        if format_limit is not None and before_count >= format_limit:
            skipped_by_per_format += len(new_records)
            return 0

        group_size = sum(record_info[3] for record_info in new_records)
        if (
            max_total_bytes is not None
            and selected_bytes + group_size > max_total_bytes
        ):
            if complete_dataset_group:
                skipped_by_total_size += len(new_records)
                logger.info(
                    "skipping complete dataset group beyond total-byte limit: "
                    "%s planned=%s group=%s limit=%s",
                    label,
                    format_byte_count(selected_bytes),
                    format_byte_count(group_size),
                    format_byte_count(max_total_bytes),
                )
                return 0
            logger.info(
                "global plan limit reached before %s: planned=%s next_group=%s limit=%s",
                label,
                format_byte_count(selected_bytes),
                format_byte_count(group_size),
                format_byte_count(max_total_bytes),
            )
            total_limit_reached = True
            return 0

        selected.extend(record_info[0] for record_info in new_records)
        selected_paths.update(str(record_info[1]) for record_info in new_records)
        selected_bytes += group_size
        after_count = before_count + len(new_records)
        per_format_counts[format_name] = after_count
        if (
            complete_dataset_group
            and format_limit is not None
            and after_count > format_limit
        ):
            per_format_overrun_files += after_count - max(format_limit, before_count)
        return len(new_records)

    processed_group_prefixes: set[pathlib.PurePosixPath] = set()
    for record_info in normalized_records:
        _, relative_path, _, _ = record_info
        group_prefix = dataset_driver_group_prefix(relative_path)
        if group_prefix is None or group_prefix in processed_group_prefixes:
            continue
        processed_group_prefixes.add(group_prefix)
        if (
            args.max_depth > 0
            and relative_path_parent_depth(relative_path) > args.max_depth
        ):
            skipped_by_depth += 1
            continue

        group_records = records_for_prefix(group_prefix)
        added = add_record_group(
            group_records,
            label=group_prefix.as_posix(),
            complete_dataset_group=True,
        )
        if added:
            dataset_groups_selected += 1
            dataset_group_files_selected += added
        if total_limit_reached:
            break

    if not total_limit_reached:
        for record, relative_path, format_name, size in normalized_records:
            if str(relative_path) in selected_paths:
                continue
            if dataset_driver_group_prefix(relative_path) is not None:
                continue
            if format_name in DRIVER_ONLY_FORMATS and (
                format_limit is not None or not include_sidecars
            ):
                skipped_by_mode += 1
                continue
            if not include_sidecars and not is_candidate_file(
                str(record["url"]), include_sidecars=False
            ):
                skipped_by_mode += 1
                continue
            if (
                args.max_depth > 0
                and relative_path_parent_depth(relative_path) > args.max_depth
            ):
                skipped_by_depth += 1
                continue
            if not within_optional_limit(size, max_file_bytes):
                skipped_by_size += 1
                continue

            add_record_group(
                [(record, relative_path, format_name, size)],
                label=str(record["relative_path"]),
                complete_dataset_group=False,
            )
            if total_limit_reached:
                break

    logger.info(
        "derived download plan: selected=%d selected_bytes=%s "
        "formats=%d skipped_by_mode=%d skipped_by_depth=%d skipped_by_size=%d "
        "skipped_by_per_format=%d skipped_by_total_size=%d dataset_groups=%d "
        "dataset_group_files=%d per_format_overrun_files=%d total_limit_reached=%s",
        len(selected),
        format_byte_count(selected_bytes),
        len(per_format_counts),
        skipped_by_mode,
        skipped_by_depth,
        skipped_by_size,
        skipped_by_per_format,
        skipped_by_total_size,
        dataset_groups_selected,
        dataset_group_files_selected,
        per_format_overrun_files,
        total_limit_reached,
    )
    return selected, selected_bytes


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
            "hash": args.hash,
        },
        "planned_count": planned_count,
        "planned_total_bytes": planned_total_bytes,
        "file_count": len(records),
        "sample_count": len(records),
        "total_bytes": sum(int(sample["size"]) for sample in records),
        "samples": [manifest_sample_record(sample) for sample in records],
    }


def build_full_manifest(
    *,
    root_url: str,
    records: list[dict[str, object]],
) -> dict[str, object]:
    return {
        "generated_at": _dt.datetime.now(tz=_dt.timezone.utc).isoformat(),
        "manifest_type": "full",
        "root_url": root_url,
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


def full_manifest_path(
    args: argparse.Namespace, output_dir: pathlib.Path
) -> pathlib.Path:
    return output_dir / args.full_manifest_name


def build_full_manifest_inventory(
    *,
    args: argparse.Namespace,
    root_url: str,
    manifest_path: pathlib.Path,
) -> None:
    logger.info(
        "no full manifest found at %s; building complete inventory from OME index",
        manifest_path,
    )
    logger.info(
        "full manifest crawl is unbounded by --per-format, --max-file-mib, "
        "--max-total-gib, and --max-depth"
    )
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

    inventory_args = full_manifest_inventory_args(args)
    records: list[dict[str, object]] = []
    total_bytes = 0
    for format_index, format_url in enumerate(top_links, start=1):
        format_name = urllib.parse.unquote(
            posixpath.basename(urllib.parse.urlparse(format_url.rstrip("/")).path)
        )
        if not format_name:
            continue
        logger.info(
            "building full manifest format %d/%d: %s",
            format_index,
            len(top_links),
            format_name,
        )
        inventory_records = collect_inventory_records(
            format_name, format_url, inventory_args, None
        )
        format_records = [
            manifest_sample_record(record) for record in inventory_records
        ]
        records.extend(format_records)
        format_bytes = sum(int(sample["size"]) for sample in format_records)
        total_bytes += format_bytes
        logger.info(
            "full manifest format %d/%d complete: %s files=%d bytes=%s "
            "total_files=%d total_bytes=%s",
            format_index,
            len(top_links),
            format_name,
            len(format_records),
            format_byte_count(format_bytes),
            len(records),
            format_byte_count(total_bytes),
        )

    write_manifest(
        manifest_path,
        build_full_manifest(root_url=root_url, records=records),
    )
    logger.info(
        "wrote full manifest inventory to %s: %d file(s), %s",
        manifest_path,
        len(records),
        format_byte_count(total_bytes),
    )


def ensure_full_manifest(
    *,
    args: argparse.Namespace,
    root_url: str,
    output_dir: pathlib.Path,
) -> pathlib.Path:
    manifest_path = full_manifest_path(args, output_dir)
    if manifest_path.exists():
        logger.info(
            "full manifest inventory exists: %s. Delete this file to refresh "
            "from the OME index.",
            manifest_path,
        )
        return manifest_path

    build_full_manifest_inventory(
        args=args,
        root_url=root_url,
        manifest_path=manifest_path,
    )
    if not manifest_path.exists():
        raise RuntimeError(f"full manifest was not created: {manifest_path}")
    return manifest_path


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
        "plan derivation limits: max_file=%s max_total=%s max_depth=%s per_format=%s",
        "unlimited" if max_file_bytes is None else format_byte_count(max_file_bytes),
        "unlimited" if max_total_bytes is None else format_byte_count(max_total_bytes),
        "unlimited" if args.max_depth == 0 else args.max_depth,
        "unlimited" if per_format_limit(args) is None else per_format_limit(args),
    )

    output_dir.mkdir(parents=True, exist_ok=True)
    full_manifest_path = ensure_full_manifest(
        args=args,
        root_url=root_url,
        output_dir=output_dir,
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
        logger.info(
            "no manifest found at %s; deriving plan from full manifest",
            manifest_path,
        )
        if not full_manifest_path.exists():
            raise RuntimeError(
                f"full manifest is required to derive plan: {full_manifest_path}"
            )
        full_records = load_planned_samples_from_manifest(full_manifest_path)
        logger.info(
            "loaded full manifest inventory for plan derivation: %d file(s), %s",
            len(full_records),
            format_byte_count(sum(int(sample["size"]) for sample in full_records)),
        )
        planned, total_bytes = derive_plan_from_full_manifest(
            args=args,
            full_records=full_records,
            max_file_bytes=max_file_bytes,
            max_total_bytes=max_total_bytes,
        )
        logger.info("derived plan from full manifest: %s", full_manifest_path)

    ordered_planned = order_records_for_download(planned)
    if ordered_planned != planned:
        logger.info(
            "download order updated: formats scheduled by ascending planned file count",
        )
    planned = ordered_planned

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
