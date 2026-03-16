#!/usr/bin/env python3

from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path
from typing import Any

import numpy as np
from PIL import Image
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
        "--heatmap-max-diff",
        type=float,
        default=64.0,
        help="Difference value mapped to the top of the saved heatmap range.",
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


def _grayscale(rgb: np.ndarray) -> np.ndarray:
    weights = np.array([0.299, 0.587, 0.114], dtype=np.float32)
    return np.tensordot(rgb.astype(np.float32), weights, axes=([-1], [0]))


def _foreground_mask(reference_rgb: np.ndarray) -> np.ndarray:
    mask = np.any(reference_rgb > 0, axis=2)
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


def _save_heatmap(path: Path, diff_map: np.ndarray, max_diff: float) -> None:
    normalized = np.clip(diff_map / max(1e-6, float(max_diff)), 0.0, 1.0)
    colored = colormaps["inferno"](normalized)[..., :3]
    rgb = np.round(colored * 255.0).astype(np.uint8)
    Image.fromarray(rgb, mode="RGB").save(path)


def _case_key(record: dict[str, Any]) -> tuple[str, str]:
    return str(record["roi_label"]), str(record["mode"])


def _condition_key(record: dict[str, Any]) -> str:
    return str(record["condition"])


def _analyze_pair(
    *,
    reference_path: Path,
    candidate_path: Path,
    padding: int,
    heatmap_max_diff: float,
    output_dir: Path,
) -> dict[str, Any]:
    reference_rgb = _load_image(reference_path)
    candidate_rgb = _load_image(candidate_path)
    if reference_rgb.shape != candidate_rgb.shape:
        raise ValueError(
            f"Image shape mismatch: {reference_path} {reference_rgb.shape} vs {candidate_path} {candidate_rgb.shape}"
        )
    mask = _foreground_mask(reference_rgb)
    x0, x1, y0, y1 = _mask_bbox(mask, padding)
    reference_crop = reference_rgb[y0:y1, x0:x1]
    candidate_crop = candidate_rgb[y0:y1, x0:x1]
    mask_crop = mask[y0:y1, x0:x1]
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
    _save_heatmap(heatmap_path, diff_map, heatmap_max_diff)
    return {
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
                "| Mode | Condition | Count | Mean SSIM | Mean abs diff | Mean P95 abs diff | Mean max abs diff |",
                "| --- | --- | --- | --- | --- | --- | --- |",
            ]
        )
        for row in aggregate_rows:
            lines.append(
                "| "
                f"{row['mode']} | {row['condition']} | {row['count']} | "
                f"{row['mean_ssim']:.6f} | {row['mean_mean_abs_diff']:.3f} | "
                f"{row['mean_p95_abs_diff']:.3f} | {row['mean_max_abs_diff']:.3f} |"
            )
        lines.extend(["", "## Per ROI", ""])
    lines.extend(
        [
            "| ROI | Mode | Condition | SSIM | Mean abs diff | P95 abs diff | Max abs diff | Foreground pixels |",
            "| --- | --- | --- | --- | --- | --- | --- | --- |",
        ]
    )
    for row in summary["rows"]:
        lines.append(
            "| "
            f"{row['roi_label']} | {row['mode']} | {row['condition']} | "
            f"{row['ssim']:.6f} | {row['mean_abs_diff']:.3f} | {row['p95_abs_diff']:.3f} | "
            f"{row['max_abs_diff']:.3f} | {row['foreground_pixel_count']} |"
        )
    return "\n".join(lines) + "\n"


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
                "ssim",
                "mean_abs_diff",
                "median_abs_diff",
                "p95_abs_diff",
                "max_abs_diff",
                "foreground_pixel_count",
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
            reference_path = Path(str(reference["screenshot_path"]))
            group_detail = {"roi_label": roi_label, "mode": mode, "comparisons": []}
            for condition in ("adaptive", "coarse_l1", "coarse_l2"):
                candidate = condition_map.get(condition)
                if candidate is None:
                    raise ValueError(
                        f"Missing condition {condition!r} for ROI={roi_label} mode={mode}"
                    )
                comparison_dir = output_dir / roi_label / _slug(mode) / condition
                comparison_dir.mkdir(parents=True, exist_ok=True)
                metrics = _analyze_pair(
                    reference_path=reference_path,
                    candidate_path=Path(str(candidate["screenshot_path"])),
                    padding=int(args.bbox_padding),
                    heatmap_max_diff=float(args.heatmap_max_diff),
                    output_dir=comparison_dir,
                )
                row = {
                    "roi_label": roi_label,
                    "mode": mode,
                    "condition": condition,
                    **metrics,
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
