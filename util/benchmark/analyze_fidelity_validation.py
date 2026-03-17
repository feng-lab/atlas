#!/usr/bin/env python3

from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path
from typing import Any

import numpy as np
import zimg
from PIL import Image, ImageDraw, ImageFont
from matplotlib import colormaps
from scipy import ndimage
from skimage.metrics import structural_similarity


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Analyze Atlas fidelity-validation screenshots against the resident "
            "native-resolution reference. Computes SSIM, masked absolute-difference "
            "metrics, and per-condition difference maps."
        )
    )
    parser.add_argument("--render-manifest", type=Path, required=True)
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=None,
        help="Output directory for summary artifacts. Defaults to <render_root>/analysis.",
    )
    parser.add_argument(
        "--bbox-padding",
        type=int,
        default=4,
        help="Padding around the reference foreground bounding box before SSIM/diff analysis.",
    )
    parser.add_argument(
        "--rgb-heatmap-max-diff",
        type=float,
        default=64.0,
        help="Difference value mapped to the top of the screenshot-RGB heatmap range.",
    )
    parser.add_argument(
        "--raw-mip-heatmap-max-diff",
        type=float,
        default=0.25,
        help="Difference value mapped to the top of the raw-MIP scalar heatmap range.",
    )
    parser.add_argument(
        "--analysis-domain",
        choices=("auto", "screenshot_rgb", "raw_mip_scalar"),
        default="screenshot_rgb",
        help=(
            "Override the analysis domain for all records. Defaults to screenshot_rgb so the "
            "fidelity workflow uses final rendered pixels by default. Use raw_mip_scalar only "
            "for the optional MIP-specific scalar cross-check."
        ),
    )
    return parser.parse_args()


def _load_json(path: Path) -> dict[str, Any]:
    raw = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(raw, dict):
        raise ValueError(f"{path} must contain a JSON object")
    return raw


def _load_image(path: Path) -> np.ndarray:
    image = Image.open(path).convert("RGB")
    return np.asarray(image, dtype=np.uint8)


def _load_raw_mip_scalar(path: Path) -> np.ndarray:
    arrays = zimg.ZImg(str(path)).to_arrays("numpy")
    if not arrays:
        raise ValueError(f"Raw MIP image {path} did not contain any scenes")
    arr = np.asarray(arrays[0])
    if arr.ndim == 4:
        if int(arr.shape[1]) != 1:
            raise ValueError(
                f"Expected raw MIP image {path} to have a single Z slice, got shape {arr.shape}"
            )
        arr = arr[0, 0]
    elif arr.ndim == 3:
        arr = arr[0]
    elif arr.ndim != 2:
        raise ValueError(f"Unsupported raw MIP array shape for {path}: {arr.shape}")
    return np.asarray(arr, dtype=np.float32)


def _grayscale(rgb: np.ndarray) -> np.ndarray:
    weights = np.array([0.299, 0.587, 0.114], dtype=np.float32)
    return np.tensordot(rgb.astype(np.float32), weights, axes=([-1], [0]))


def _foreground_mask(reference: np.ndarray) -> np.ndarray:
    if reference.ndim == 3:
        mask = np.any(reference > 0, axis=2)
    else:
        mask = reference > 0
    labeled, count = ndimage.label(mask)
    if count <= 1:
        return mask
    component_sizes = ndimage.sum(mask, labeled, index=np.arange(1, count + 1))
    largest_label = int(np.argmax(component_sizes)) + 1
    return labeled == largest_label


def _mask_bbox(mask: np.ndarray, padding: int) -> tuple[int, int, int, int]:
    ys, xs = np.nonzero(mask)
    if len(xs) == 0 or len(ys) == 0:
        raise ValueError("Reference foreground mask is empty; cannot analyze fidelity.")
    x0 = max(0, int(xs.min()) - padding)
    x1 = min(mask.shape[1], int(xs.max()) + 1 + padding)
    y0 = max(0, int(ys.min()) - padding)
    y1 = min(mask.shape[0], int(ys.max()) + 1 + padding)
    return x0, x1, y0, y1


def _difference_map(reference_rgb: np.ndarray, candidate_rgb: np.ndarray) -> np.ndarray:
    diff = np.abs(reference_rgb.astype(np.int16) - candidate_rgb.astype(np.int16))
    return diff.max(axis=2).astype(np.float32)


def _difference_map_scalar(
    reference_scalar: np.ndarray, candidate_scalar: np.ndarray
) -> np.ndarray:
    return np.abs(
        reference_scalar.astype(np.float32) - candidate_scalar.astype(np.float32)
    )


def _format_heatmap_label(value: float) -> str:
    if math.isclose(value, round(value), rel_tol=0.0, abs_tol=1e-9):
        return str(int(round(value)))
    return f"{value:.3f}".rstrip("0").rstrip(".")


def _save_heatmap(
    path: Path, diff_map: np.ndarray, max_diff: float, *, show_colorbar: bool
) -> None:
    normalized = np.clip(diff_map / max(1e-6, float(max_diff)), 0.0, 1.0)
    colored = colormaps["inferno"](normalized)[..., :3]
    rgb = np.round(colored * 255.0).astype(np.uint8)
    image = Image.fromarray(rgb, mode="RGB")
    if not show_colorbar:
        image.save(path)
        return

    font = ImageFont.load_default()
    heatmap_width, heatmap_height = image.size
    gap = 24
    bar_width = 24
    tick_width = 8
    label_padding = 8
    label_values = [float(max_diff), float(max_diff) * 0.5, 0.0]
    label_texts = [_format_heatmap_label(value) for value in label_values]
    label_bbox_widths = [
        ImageDraw.Draw(Image.new("RGB", (1, 1))).textbbox((0, 0), text, font=font)[2]
        for text in label_texts
    ]
    label_width = max(label_bbox_widths, default=0)

    canvas_width = (
        heatmap_width + gap + bar_width + tick_width + label_padding + label_width + 8
    )
    canvas = Image.new("RGB", (canvas_width, heatmap_height), color=(0, 0, 0))
    canvas.paste(image, (0, 0))

    gradient = np.linspace(1.0, 0.0, heatmap_height, dtype=np.float32)[:, None]
    gradient = np.repeat(gradient, bar_width, axis=1)
    bar_rgb = np.round(colormaps["inferno"](gradient)[..., :3] * 255.0).astype(np.uint8)
    bar_image = Image.fromarray(bar_rgb, mode="RGB")
    bar_x = heatmap_width + gap
    canvas.paste(bar_image, (bar_x, 0))

    draw = ImageDraw.Draw(canvas)
    tick_x0 = bar_x + bar_width
    tick_x1 = tick_x0 + tick_width
    label_x = tick_x1 + label_padding
    label_positions = [0, heatmap_height // 2, heatmap_height - 1]
    for y, text in zip(label_positions, label_texts):
        draw.line((tick_x0, y, tick_x1, y), fill=(255, 255, 255), width=1)
        text_bbox = draw.textbbox((0, 0), text, font=font)
        text_height = text_bbox[3] - text_bbox[1]
        text_y = max(0, min(heatmap_height - text_height, y - text_height // 2))
        draw.text((label_x, text_y), text, fill=(255, 255, 255), font=font)

    canvas.save(path)


def _analysis_domain(record: dict[str, Any], override: str = "auto") -> str:
    if override != "auto":
        return override
    domain = str(record.get("analysis_domain", "") or "").strip()
    if domain:
        return domain
    raw_mip_path = record.get("raw_mip_path")
    if raw_mip_path:
        return "raw_mip_scalar"
    return "screenshot_rgb"


def _analysis_source_path(record: dict[str, Any], override: str = "auto") -> Path:
    domain = _analysis_domain(record, override)
    if domain == "raw_mip_scalar":
        raw_mip_path = record.get("raw_mip_path")
        if not raw_mip_path:
            raise ValueError(
                f"Record is missing raw_mip_path for raw-MIP analysis: {record}"
            )
        return Path(str(raw_mip_path))
    return Path(str(record["screenshot_path"]))


def _case_key(record: dict[str, Any]) -> tuple[str, str]:
    return str(record["roi_label"]), str(record["mode"])


def _condition_key(record: dict[str, Any]) -> str:
    return str(record["condition"])


def _screen_space_audit(record: dict[str, Any]) -> dict[str, Any]:
    audit = record.get("screen_space_sufficiency_audit")
    if not isinstance(audit, dict):
        return {
            "contributing_samples": 0,
            "sufficient_samples": 0,
            "level0_samples": 0,
            "level0_limited_samples": 0,
            "contributing_pixels": 0,
            "sufficient_pixels": 0,
            "level0_pixels": 0,
            "level0_limited_pixels": 0,
            "sufficient_sample_fraction": None,
            "sufficient_pixel_fraction": None,
            "level0_sample_fraction": None,
            "level0_limited_sample_fraction": None,
            "level0_pixel_fraction": None,
            "level0_limited_pixel_fraction": None,
        }
    return {
        "contributing_samples": int(audit.get("contributing_samples", 0) or 0),
        "sufficient_samples": int(audit.get("sufficient_samples", 0) or 0),
        "level0_samples": int(audit.get("level0_samples", 0) or 0),
        "level0_limited_samples": int(audit.get("level0_limited_samples", 0) or 0),
        "contributing_pixels": int(audit.get("contributing_pixels", 0) or 0),
        "sufficient_pixels": int(audit.get("sufficient_pixels", 0) or 0),
        "level0_pixels": int(audit.get("level0_pixels", 0) or 0),
        "level0_limited_pixels": int(audit.get("level0_limited_pixels", 0) or 0),
        "sufficient_sample_fraction": (
            float(audit["sufficient_sample_fraction"])
            if audit.get("sufficient_sample_fraction") is not None
            else None
        ),
        "sufficient_pixel_fraction": (
            float(audit["sufficient_pixel_fraction"])
            if audit.get("sufficient_pixel_fraction") is not None
            else None
        ),
        "level0_sample_fraction": (
            float(audit["level0_sample_fraction"])
            if audit.get("level0_sample_fraction") is not None
            else None
        ),
        "level0_limited_sample_fraction": (
            float(audit["level0_limited_sample_fraction"])
            if audit.get("level0_limited_sample_fraction") is not None
            else None
        ),
        "level0_pixel_fraction": (
            float(audit["level0_pixel_fraction"])
            if audit.get("level0_pixel_fraction") is not None
            else None
        ),
        "level0_limited_pixel_fraction": (
            float(audit["level0_limited_pixel_fraction"])
            if audit.get("level0_limited_pixel_fraction") is not None
            else None
        ),
    }


def _analyze_pair(
    *,
    analysis_domain: str,
    reference_path: Path,
    candidate_path: Path,
    padding: int,
    heatmap_max_diff: float,
    output_dir: Path,
) -> dict[str, Any]:
    if analysis_domain == "raw_mip_scalar":
        reference_scalar = _load_raw_mip_scalar(reference_path)
        candidate_scalar = _load_raw_mip_scalar(candidate_path)
        if reference_scalar.shape != candidate_scalar.shape:
            raise ValueError(
                f"Image shape mismatch: {reference_path} {reference_scalar.shape} vs {candidate_path} {candidate_scalar.shape}"
            )
        mask = _foreground_mask(reference_scalar)
    else:
        reference_rgb = _load_image(reference_path)
        candidate_rgb = _load_image(candidate_path)
        if reference_rgb.shape != candidate_rgb.shape:
            raise ValueError(
                f"Image shape mismatch: {reference_path} {reference_rgb.shape} vs {candidate_path} {candidate_rgb.shape}"
            )
        mask = _foreground_mask(reference_rgb)
    x0, x1, y0, y1 = _mask_bbox(mask, padding)
    mask_crop = mask[y0:y1, x0:x1]
    if analysis_domain == "raw_mip_scalar":
        reference_crop = reference_scalar[y0:y1, x0:x1]
        candidate_crop = candidate_scalar[y0:y1, x0:x1]
        ssim_value = float(
            structural_similarity(reference_crop, candidate_crop, data_range=1.0)
        )
        diff_map = _difference_map_scalar(reference_crop, candidate_crop)
    else:
        reference_crop = reference_rgb[y0:y1, x0:x1]
        candidate_crop = candidate_rgb[y0:y1, x0:x1]
        reference_gray = _grayscale(reference_crop)
        candidate_gray = _grayscale(candidate_crop)
        ssim_value = float(
            structural_similarity(reference_gray, candidate_gray, data_range=255.0)
        )
        diff_map = _difference_map(reference_crop, candidate_crop)
    masked_values = diff_map[mask_crop]
    if masked_values.size == 0:
        raise ValueError(f"Foreground mask is empty for {reference_path}")
    heatmap_path = output_dir / "difference_heatmap.png"
    _save_heatmap(
        heatmap_path,
        diff_map,
        heatmap_max_diff,
        show_colorbar=analysis_domain == "screenshot_rgb",
    )
    return {
        "analysis_domain": analysis_domain,
        "reference_path": str(reference_path),
        "candidate_path": str(candidate_path),
        "bbox_xyxy": [int(x0), int(y0), int(x1), int(y1)],
        "foreground_pixel_count": int(mask_crop.sum()),
        "ssim": ssim_value,
        "mean_abs_diff": float(masked_values.mean()),
        "median_abs_diff": float(np.median(masked_values)),
        "p95_abs_diff": float(np.quantile(masked_values, 0.95)),
        "max_abs_diff": float(masked_values.max()),
        "heatmap_path": str(heatmap_path),
    }


def _summary_markdown(summary: dict[str, Any]) -> str:
    lines = [
        "# Fidelity Validation Summary",
        "",
        f"Render manifest: `{summary['render_manifest']}`",
        "",
    ]
    aggregate_rows = summary.get("aggregate_rows") or []
    if aggregate_rows:
        lines.extend(
            [
                "## Aggregate By Mode/Condition",
                "",
                "| Mode | Condition | Count | Mean SSIM | Mean abs diff | Mean P95 abs diff | Mean max abs diff | Mean sample sufficiency | Mean pixel sufficiency | Mean level-0 sample frac | Mean level-0-limited sample frac |",
                "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |",
            ]
        )
        for row in aggregate_rows:
            lines.append(
                "| "
                f"{row['mode']} | {row['condition']} | {row['count']} | "
                f"{row['mean_ssim']:.6f} | {row['mean_mean_abs_diff']:.3f} | "
                f"{row['mean_p95_abs_diff']:.3f} | {row['mean_max_abs_diff']:.3f} | "
                f"{_format_optional_metric(row['mean_sufficient_sample_fraction'])} | "
                f"{_format_optional_metric(row['mean_sufficient_pixel_fraction'])} | "
                f"{_format_optional_metric(row['mean_level0_sample_fraction'])} | "
                f"{_format_optional_metric(row['mean_level0_limited_sample_fraction'])} |"
            )
        lines.extend(["", "## Per ROI", ""])
    lines.extend(
        [
            "| ROI | Mode | Condition | Domain | SSIM | Mean abs diff | P95 abs diff | Max abs diff | Foreground pixels | Sample sufficiency | Pixel sufficiency | Level-0 sample frac | Level-0-limited sample frac |",
            "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |",
        ]
    )
    for row in summary["rows"]:
        lines.append(
            "| "
            f"{row['roi_label']} | {row['mode']} | {row['condition']} | {row['analysis_domain']} | "
            f"{row['ssim']:.6f} | {row['mean_abs_diff']:.3f} | {row['p95_abs_diff']:.3f} | "
            f"{row['max_abs_diff']:.3f} | {row['foreground_pixel_count']} | "
            f"{_format_optional_metric(row['sufficient_sample_fraction'])} | "
            f"{_format_optional_metric(row['sufficient_pixel_fraction'])} | "
            f"{_format_optional_metric(row['level0_sample_fraction'])} | "
            f"{_format_optional_metric(row['level0_limited_sample_fraction'])} |"
        )
    return "\n".join(lines) + "\n"


def _mean_optional_metric(items: list[dict[str, Any]], key: str) -> float | None:
    values = [float(item[key]) for item in items if item.get(key) is not None]
    if not values:
        return None
    return float(np.mean(values))


def _format_optional_metric(value: float | None) -> str:
    if value is None:
        return "n/a"
    return f"{value:.6f}"


def main() -> int:
    args = _parse_args()
    render_manifest = _load_json(args.render_manifest.resolve())
    render_records = render_manifest.get("render_records")
    if not isinstance(render_records, list) or not render_records:
        raise ValueError("Render manifest must define a non-empty render_records list")

    output_dir = (
        args.output_dir.resolve()
        if args.output_dir is not None
        else args.render_manifest.resolve().parent / "analysis"
    )
    output_dir.mkdir(parents=True, exist_ok=True)

    grouped: dict[tuple[str, str], dict[str, dict[str, Any]]] = {}
    for record in render_records:
        if not isinstance(record, dict):
            continue
        key = _case_key(record)
        grouped.setdefault(key, {})[_condition_key(record)] = record

    rows: list[dict[str, Any]] = []
    aggregate_groups: dict[tuple[str, str], list[dict[str, Any]]] = {}
    details: dict[str, Any] = {
        "render_manifest": str(args.render_manifest.resolve()),
        "groups": [],
    }
    csv_path = output_dir / "summary.csv"
    with csv_path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(
            stream,
            fieldnames=[
                "roi_label",
                "mode",
                "condition",
                "analysis_domain",
                "ssim",
                "mean_abs_diff",
                "median_abs_diff",
                "p95_abs_diff",
                "max_abs_diff",
                "foreground_pixel_count",
                "contributing_samples",
                "sufficient_samples",
                "level0_samples",
                "level0_limited_samples",
                "contributing_pixels",
                "sufficient_pixels",
                "level0_pixels",
                "level0_limited_pixels",
                "sufficient_sample_fraction",
                "sufficient_pixel_fraction",
                "level0_sample_fraction",
                "level0_limited_sample_fraction",
                "level0_pixel_fraction",
                "level0_limited_pixel_fraction",
                "bbox_xyxy",
                "reference_path",
                "candidate_path",
                "heatmap_path",
            ],
        )
        writer.writeheader()
        for (roi_label, mode), condition_map in sorted(grouped.items()):
            reference = condition_map.get("reference")
            if reference is None:
                raise ValueError(
                    f"Missing reference render for ROI={roi_label} mode={mode}"
                )
            group_detail = {"roi_label": roi_label, "mode": mode, "comparisons": []}
            for condition in ("adaptive", "coarse_l1", "coarse_l2"):
                candidate = condition_map.get(condition)
                if candidate is None:
                    raise ValueError(
                        f"Missing condition {condition!r} for ROI={roi_label} mode={mode}"
                    )
                comparison_dir = output_dir / roi_label / _slug(mode) / condition
                comparison_dir.mkdir(parents=True, exist_ok=True)
                analysis_domain = _analysis_domain(reference, str(args.analysis_domain))
                if (
                    _analysis_domain(candidate, str(args.analysis_domain))
                    != analysis_domain
                ):
                    raise ValueError(
                        f"Analysis domain mismatch for ROI={roi_label} mode={mode} "
                        f"reference={analysis_domain} candidate={_analysis_domain(candidate, str(args.analysis_domain))}"
                    )
                metrics = _analyze_pair(
                    analysis_domain=analysis_domain,
                    reference_path=_analysis_source_path(
                        reference, str(args.analysis_domain)
                    ),
                    candidate_path=_analysis_source_path(
                        candidate, str(args.analysis_domain)
                    ),
                    padding=int(args.bbox_padding),
                    heatmap_max_diff=float(
                        args.raw_mip_heatmap_max_diff
                        if analysis_domain == "raw_mip_scalar"
                        else args.rgb_heatmap_max_diff
                    ),
                    output_dir=comparison_dir,
                )
                audit = _screen_space_audit(candidate)
                row = {
                    "roi_label": roi_label,
                    "mode": mode,
                    "condition": condition,
                    **metrics,
                    **audit,
                }
                rows.append(row)
                aggregate_groups.setdefault((mode, condition), []).append(row)
                writer.writerow(row)
                group_detail["comparisons"].append(row)
            details["groups"].append(group_detail)

    aggregate_rows: list[dict[str, Any]] = []
    for (mode, condition), items in sorted(aggregate_groups.items()):
        aggregate_rows.append(
            {
                "mode": mode,
                "condition": condition,
                "count": len(items),
                "mean_ssim": float(np.mean([float(item["ssim"]) for item in items])),
                "mean_mean_abs_diff": float(
                    np.mean([float(item["mean_abs_diff"]) for item in items])
                ),
                "mean_p95_abs_diff": float(
                    np.mean([float(item["p95_abs_diff"]) for item in items])
                ),
                "mean_max_abs_diff": float(
                    np.mean([float(item["max_abs_diff"]) for item in items])
                ),
                "mean_sufficient_sample_fraction": _mean_optional_metric(
                    items, "sufficient_sample_fraction"
                ),
                "mean_sufficient_pixel_fraction": _mean_optional_metric(
                    items, "sufficient_pixel_fraction"
                ),
                "mean_level0_sample_fraction": _mean_optional_metric(
                    items, "level0_sample_fraction"
                ),
                "mean_level0_limited_sample_fraction": _mean_optional_metric(
                    items, "level0_limited_sample_fraction"
                ),
                "mean_level0_pixel_fraction": _mean_optional_metric(
                    items, "level0_pixel_fraction"
                ),
                "mean_level0_limited_pixel_fraction": _mean_optional_metric(
                    items, "level0_limited_pixel_fraction"
                ),
            }
        )

    summary = {
        "render_manifest": str(args.render_manifest.resolve()),
        "summary_csv": str(csv_path),
        "rows": rows,
        "aggregate_rows": aggregate_rows,
        "group_count": len(details["groups"]),
    }
    (output_dir / "summary.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8"
    )
    (output_dir / "details.json").write_text(
        json.dumps(details, indent=2) + "\n", encoding="utf-8"
    )
    (output_dir / "summary.md").write_text(_summary_markdown(summary), encoding="utf-8")
    return 0


def _slug(text: str) -> str:
    return text.strip().lower().replace(" ", "_").replace("-", "_").replace("/", "_")


if __name__ == "__main__":
    raise SystemExit(main())
