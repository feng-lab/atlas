from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from util.benchmark import export_scene_animation_stability as benchmark


def _perf_record(*, versioned: bool) -> dict[str, object]:
    stats_fields = (
        benchmark._CURRENT_PERF_STATS
        if versioned
        else benchmark._LEGACY_UNVERSIONED_PERF_STATS
    )
    record: dict[str, object] = {
        "frame": 1,
        "cpu_ms": 2.0,
        "gpu_ms": 3.0,
        "gpu_scoped_ms": 2.5,
        "top": [{"label": "frame", "ms": 2.5, "pct": 100.0}],
        "stats": {field: 0 for field in stats_fields},
    }
    if versioned:
        record["schema"] = benchmark._PERF_SCHEMA
        record["schema_version"] = benchmark._CURRENT_PERF_SCHEMA_VERSION
    return record


def _animation_worker_record(
    *,
    requested_device_index: int | None,
    actual_device_index: int,
    frame_start: int,
    frame_end: int,
) -> dict[str, object]:
    return {
        "schema": benchmark._ANIMATION_WORKER_REPORT_SCHEMA,
        "schema_version": benchmark._ANIMATION_WORKER_REPORT_SCHEMA_VERSION,
        "backend": "vulkan",
        "requested_device_index": requested_device_index,
        "actual_device": {
            "preference_index": actual_device_index,
            "uuid": f"00000000-0000-0000-0000-{actual_device_index:012d}",
        },
        "frames": {
            "start": frame_start,
            "end": frame_end,
            "count": frame_end - frame_start,
        },
        "timing_ms": {
            "engine_init": 1.0,
            "animation_load": 2.0,
            "view_bind": 3.0,
            "export": 4.0,
            "worker_total": 10.0,
        },
    }


def _parse_animation_worker_records(
    records: list[dict[str, object]],
    expected_frame_range: range,
    expected_requested_device_indices: tuple[int | None, ...],
) -> benchmark.AnimationWorkerReportParseResult:
    with tempfile.TemporaryDirectory() as temp_dir:
        directory = Path(temp_dir)
        for index, record in enumerate(records):
            (directory / f"worker_{index}.json").write_text(
                json.dumps(record), encoding="utf-8"
            )
        return benchmark._parse_animation_worker_reports(
            directory,
            expected_frame_range=expected_frame_range,
            expected_requested_device_indices=expected_requested_device_indices,
            dry_run=False,
        )


class ExportSceneAnimationStabilityTest(unittest.TestCase):
    def test_baseline_comparison_reports_duration_delta_and_exact_hash_match(
        self,
    ) -> None:
        group = {
            "kind": "scene",
            "backend": "vulkan",
            "label": "sample.scene",
            "source_sha256": "source-hash",
            "source_path": "/data/sample.scene",
            "success": True,
            "duration_seconds": {"median": 10.0},
        }
        stability = {
            "entries": [
                {
                    "kind": "scene",
                    "backend": "vulkan",
                    "label": "sample.scene",
                    "source_sha256": "source-hash",
                    "source_path": "/data/sample.scene",
                    "relative_path": "export.png",
                    "stable_and_complete": True,
                    "hashes": ["output-hash"],
                }
            ]
        }
        with tempfile.TemporaryDirectory() as temp_dir:
            baseline_root = Path(temp_dir)
            (baseline_root / "aggregate_summary.json").write_text(
                json.dumps({"run_label": "baseline", "groups": [group]}),
                encoding="utf-8",
            )
            (baseline_root / "stability_summary.json").write_text(
                json.dumps(stability), encoding="utf-8"
            )
            (baseline_root / "manifest.json").write_text("{}\n", encoding="utf-8")
            candidate_group = {
                **group,
                "duration_seconds": {"median": 8.0},
            }

            comparison = benchmark._build_baseline_comparison(
                baseline_root=baseline_root,
                candidate_manifest={},
                candidate_summary={
                    "run_label": "candidate",
                    "groups": [candidate_group],
                },
                candidate_stability=stability,
            )
            incompatible = benchmark._build_baseline_comparison(
                baseline_root=baseline_root,
                candidate_manifest={"host": {"cpu": "different"}},
                candidate_summary={
                    "run_label": "candidate",
                    "groups": [candidate_group],
                },
                candidate_stability=stability,
            )

        case = comparison["cases"][0]
        self.assertEqual(case["candidate_minus_baseline_seconds"], -2.0)
        self.assertEqual(case["candidate_minus_baseline_percent"], -20.0)
        self.assertTrue(case["output_hash_sets_match"])
        self.assertEqual(incompatible["status"], "incompatible")
        self.assertIsNone(incompatible["cases"][0]["candidate_minus_baseline_seconds"])

    def test_manifest_compatibility_compares_effective_args_and_hash_policies(
        self,
    ) -> None:
        old_manifest = {"extra_args": ["--common=true"]}
        equivalent_manifest = {
            "extra_args": [],
            "scene_extra_args": ["--common=true"],
            "animation_extra_args": ["--common=true"],
            "scene_hash_policy": "exact",
            "animation_hash_policy": "exact",
        }

        equivalent = benchmark._manifest_compatibility(
            old_manifest, equivalent_manifest
        )
        scene_args_changed = benchmark._manifest_compatibility(
            old_manifest,
            {
                **equivalent_manifest,
                "scene_extra_args": ["--common=true", "--scene-only=true"],
            },
        )
        hash_policy_changed = benchmark._manifest_compatibility(
            old_manifest,
            {**equivalent_manifest, "scene_hash_policy": "record-only"},
        )

        self.assertTrue(equivalent["compatible"])
        self.assertEqual(
            [entry["field"] for entry in scene_args_changed["mismatches"]],
            ["effective_extra_args"],
        )
        self.assertEqual(
            [entry["field"] for entry in hash_policy_changed["mismatches"]],
            ["hash_policies"],
        )

    def test_aggregate_fails_when_any_expected_run_is_missing(self) -> None:
        key: benchmark.RunGroupKey = (
            "scene",
            "vulkan",
            "sample.scene",
            "abc",
            "/data/sample.scene",
        )
        summary = benchmark._build_aggregate_summary(
            runs=[],
            expected_groups={key: 2},
            expected_source_paths={key: "/data/sample.scene"},
            harness_errors=[],
            stability_summary=None,
            dry_run=False,
            run_label="candidate",
        )

        self.assertEqual(summary["status"], "failed")
        self.assertFalse(summary["all_runs_complete"])
        self.assertEqual(summary["expected_run_count"], 2)
        self.assertEqual(summary["recorded_run_count"], 0)
        self.assertEqual(len(summary["failures"]), 1)

    def test_expected_animation_frame_count_matches_atlas_range_rules(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            animation_path = Path(temp_dir) / "sample.animation3d"
            animation_path.write_text(
                json.dumps({"Animation3D": {"Duration": 1.25}}), encoding="utf-8"
            )

            self.assertEqual(
                benchmark._expected_animation_frame_count(
                    animation_path, fps=4, start_frame=0, end_frame=-1
                ),
                5,
            )
            self.assertEqual(
                benchmark._expected_animation_frame_count(
                    animation_path, fps=4, start_frame=1, end_frame=4
                ),
                3,
            )
            self.assertEqual(
                benchmark._expected_animation_frame_count(
                    animation_path, fps=4, start_frame=1, end_frame=100
                ),
                4,
            )

    def test_animation_worker_reports_accept_exact_adjacent_ranges(self) -> None:
        result = _parse_animation_worker_records(
            [
                _animation_worker_record(
                    requested_device_index=0,
                    actual_device_index=0,
                    frame_start=5,
                    frame_end=8,
                ),
                _animation_worker_record(
                    requested_device_index=None,
                    actual_device_index=1,
                    frame_start=8,
                    frame_end=10,
                ),
            ],
            range(5, 10),
            (0, None),
        )
        self.assertEqual(result.errors, ())
        self.assertEqual(
            [entry["report"]["frames"]["start"] for entry in result.records],
            [5, 8],
        )

    def test_animation_worker_reports_reject_invalid_coverage_routing_adapter_and_timing(
        self,
    ) -> None:
        first = _animation_worker_record(
            requested_device_index=0,
            actual_device_index=0,
            frame_start=0,
            frame_end=2,
        )
        gap = _animation_worker_record(
            requested_device_index=1,
            actual_device_index=1,
            frame_start=3,
            frame_end=4,
        )
        fallback = _animation_worker_record(
            requested_device_index=1,
            actual_device_index=0,
            frame_start=0,
            frame_end=2,
        )
        invalid_timing = _animation_worker_record(
            requested_device_index=0,
            actual_device_index=0,
            frame_start=0,
            frame_end=2,
        )
        invalid_timing["timing_ms"]["export"] = float("nan")

        wrong_route_first = _animation_worker_record(
            requested_device_index=1,
            actual_device_index=1,
            frame_start=0,
            frame_end=2,
        )
        wrong_route_second = _animation_worker_record(
            requested_device_index=1,
            actual_device_index=1,
            frame_start=2,
            frame_end=4,
        )

        gap_result = _parse_animation_worker_records([first, gap], range(0, 4), (0, 1))
        fallback_result = _parse_animation_worker_records([fallback], range(0, 2), (1,))
        timing_result = _parse_animation_worker_records(
            [invalid_timing], range(0, 2), (0,)
        )
        route_result = _parse_animation_worker_records(
            [wrong_route_first, wrong_route_second], range(0, 4), (0, 1)
        )

        self.assertTrue(any("frame gap" in error for error in gap_result.errors))
        self.assertTrue(
            any("not requested" in error for error in fallback_result.errors)
        )
        self.assertTrue(
            any("timing_ms.export" in error for error in timing_result.errors)
        )
        self.assertTrue(
            any("not benchmark device 0" in error for error in route_result.errors)
        )

    def test_animation_duration_uses_atlas_default_and_minimum(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            animation_path = root / "sample.animation3d"
            short_animation_path = root / "short.animation3d"
            negative_animation_path = root / "negative.animation3d"
            invalid_animation_path = root / "invalid.animation3d"
            animation_path.write_text(json.dumps({"Animation3D": {}}), encoding="utf-8")
            short_animation_path.write_text(
                json.dumps({"Animation3D": {"Duration": 0.25}}), encoding="utf-8"
            )
            negative_animation_path.write_text(
                json.dumps({"Animation3D": {"Duration": -2.0}}), encoding="utf-8"
            )
            invalid_animation_path.write_text(
                json.dumps({"Animation3D": {"Duration": "2.5"}}), encoding="utf-8"
            )

            self.assertEqual(
                benchmark._animation_duration_seconds(animation_path), 10.0
            )
            self.assertEqual(
                benchmark._animation_duration_seconds(short_animation_path), 1.0
            )
            self.assertEqual(
                benchmark._animation_duration_seconds(negative_animation_path), 1.0
            )
            self.assertEqual(
                benchmark._expected_animation_frame_count(
                    animation_path, fps=30, start_frame=0, end_frame=-1
                ),
                300,
            )
            self.assertEqual(
                benchmark._expected_animation_frame_count(
                    short_animation_path, fps=4, start_frame=0, end_frame=-1
                ),
                4,
            )
            with self.assertRaisesRegex(ValueError, "Invalid Animation3D.Duration"):
                benchmark._animation_duration_seconds(invalid_animation_path)

    def test_explicit_workload_selection_disables_implicit_defaults(self) -> None:
        scene = "/data/sample.scene"
        animation = "/data/sample.animation3d"

        default_scenes, default_animations = benchmark._selected_workload_paths(
            None, None
        )
        scene_only, no_animations = benchmark._selected_workload_paths([scene], None)
        no_scenes, animation_only = benchmark._selected_workload_paths(
            None, [animation]
        )

        self.assertEqual(default_scenes, list(benchmark.DEFAULT_SCENES))
        self.assertEqual(default_animations, list(benchmark.DEFAULT_ANIMATIONS))
        self.assertEqual(scene_only, [Path(scene)])
        self.assertEqual(no_animations, [])
        self.assertEqual(no_scenes, [])
        self.assertEqual(animation_only, [Path(animation)])

    def test_same_basename_inputs_get_distinct_output_directories(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            first = root / "first" / "sample.scene"
            second = root / "second" / "sample.scene"
            first.parent.mkdir()
            second.parent.mkdir()
            first.write_text("{}\n", encoding="utf-8")
            second.write_text("{}\n", encoding="utf-8")

            cases = benchmark._build_export_cases("scene", [first, second])

            self.assertEqual([case.label for case in cases], ["sample.scene"] * 2)
            self.assertEqual(len({case.output_key for case in cases}), 2)
            self.assertTrue(all(case.output_key.startswith("path_") for case in cases))

            rows = [
                {
                    "kind": "scene",
                    "backend": "vulkan",
                    "label": case.label,
                    "source_path": case.source_path,
                    "source_sha256": "identical-source-hash",
                    "run_index": 1,
                    "relative_path": "export.png",
                    "size_bytes": 1,
                    "sha256": f"output-{index}",
                    "output_dir": case.output_key,
                }
                for index, case in enumerate(cases)
            ]
            expected_groups = {
                (
                    "scene",
                    "vulkan",
                    case.label,
                    "identical-source-hash",
                    case.source_path,
                ): 1
                for case in cases
            }
            stability = benchmark._build_stability_summary(
                rows, expected_groups=expected_groups
            )

            self.assertEqual(len(stability["entries"]), 2)
            self.assertTrue(
                all(entry["stable_and_complete"] for entry in stability["entries"])
            )

    def test_malformed_perf_ndjson_is_rejected_with_shape_errors(self) -> None:
        valid_record = _perf_record(versioned=True)
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "perf.ndjson"
            path.write_text(
                json.dumps(valid_record) + "\n" + '{"frame": "wrong"}\nnot-json\n',
                encoding="utf-8",
            )

            result = benchmark._parse_perf_summary(path)

        self.assertEqual(result.record_count, 2)
        self.assertTrue(any("integer frame" in error for error in result.errors))
        self.assertTrue(any("no stats object" in error for error in result.errors))
        self.assertTrue(any("not valid JSON" in error for error in result.errors))

    def test_current_versioned_perf_ndjson_is_accepted(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "perf.ndjson"
            path.write_text(
                json.dumps(_perf_record(versioned=True)) + "\n", encoding="utf-8"
            )

            result = benchmark._parse_perf_summary(path)

        self.assertEqual(result.record_count, 1)
        self.assertEqual(result.profile, benchmark._CURRENT_PERF_PROFILE)
        self.assertEqual(result.unavailable_metrics, ())
        self.assertEqual(result.errors, ())

    def test_current_versioned_perf_ndjson_requires_submission_metrics(self) -> None:
        for missing_field in benchmark._NEW_PERF_STATS:
            with self.subTest(missing_field=missing_field):
                record = _perf_record(versioned=True)
                del record["stats"][missing_field]  # type: ignore[index]
                with tempfile.TemporaryDirectory() as temp_dir:
                    path = Path(temp_dir) / "perf.ndjson"
                    path.write_text(json.dumps(record) + "\n", encoding="utf-8")

                    result = benchmark._parse_perf_summary(path)

                self.assertTrue(
                    any(missing_field in error for error in result.errors),
                    result.errors,
                )

    def test_legacy_unversioned_perf_ndjson_reports_submission_metrics_unavailable(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "perf.ndjson"
            path.write_text(
                json.dumps(_perf_record(versioned=False)) + "\n", encoding="utf-8"
            )

            result = benchmark._parse_perf_summary(path)

        self.assertEqual(result.record_count, 1)
        self.assertEqual(result.profile, benchmark._LEGACY_UNVERSIONED_PERF_PROFILE)
        self.assertEqual(result.unavailable_metrics, benchmark._NEW_PERF_STATS)
        self.assertEqual(result.errors, ())

        failures = benchmark._validate_run_outputs(
            returncode=0,
            file_entries=[{"relative_path": "export.png", "size_bytes": 1}],
            expected_file_count=1,
            perf_summary_path=Path("perf.ndjson"),
            perf_summary_frame_count=result.record_count,
            perf_summary_errors=result.errors,
            timed_out=False,
            timeout_seconds=0.0,
            dry_run=False,
        )
        self.assertEqual(failures, ())

    def test_unversioned_submission_metrics_are_rejected(self) -> None:
        for present_fields in (
            ("submissions",),
            ("fence_waits",),
            benchmark._NEW_PERF_STATS,
        ):
            with self.subTest(present_fields=present_fields):
                record = _perf_record(versioned=False)
                stats = record["stats"]
                self.assertIsInstance(stats, dict)
                for field in present_fields:
                    stats[field] = 0
                with tempfile.TemporaryDirectory() as temp_dir:
                    path = Path(temp_dir) / "perf.ndjson"
                    path.write_text(json.dumps(record) + "\n", encoding="utf-8")

                    result = benchmark._parse_perf_summary(path)

                self.assertNotEqual(result.errors, ())

    def test_unknown_perf_schema_version_is_rejected(self) -> None:
        record = _perf_record(versioned=True)
        record["schema_version"] = benchmark._CURRENT_PERF_SCHEMA_VERSION + 1
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "perf.ndjson"
            path.write_text(json.dumps(record) + "\n", encoding="utf-8")

            result = benchmark._parse_perf_summary(path)

        self.assertTrue(any("unsupported" in error for error in result.errors))

    def test_mixed_legacy_and_current_perf_records_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "perf.ndjson"
            path.write_text(
                json.dumps(_perf_record(versioned=False))
                + "\n"
                + json.dumps(_perf_record(versioned=True))
                + "\n",
                encoding="utf-8",
            )

            result = benchmark._parse_perf_summary(path)

        self.assertTrue(
            any("mixes record profiles" in error for error in result.errors)
        )

    def test_native_qt_mode_clears_inherited_platform_environment(self) -> None:
        completed = mock.Mock(returncode=0)
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            with (
                mock.patch.dict(os.environ, {"QT_QPA_PLATFORM": "offscreen"}),
                mock.patch.object(
                    benchmark.subprocess, "run", return_value=completed
                ) as run_mock,
            ):
                returncode, _elapsed, timed_out = benchmark._run_process(
                    command=[sys.executable],
                    stdout_log=root / "stdout.log",
                    stderr_log=root / "stderr.log",
                    qt_platform="",
                    timeout_seconds=0.0,
                    dry_run=False,
                )

        self.assertEqual(returncode, 0)
        self.assertFalse(timed_out)
        self.assertNotIn("QT_QPA_PLATFORM", run_mock.call_args.kwargs["env"])

    def test_child_timeout_is_recorded_without_raising(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            with mock.patch.object(
                benchmark.subprocess,
                "run",
                side_effect=benchmark.subprocess.TimeoutExpired(["Atlas"], 0.1),
            ):
                returncode, _elapsed, timed_out = benchmark._run_process(
                    command=["Atlas"],
                    stdout_log=root / "stdout.log",
                    stderr_log=root / "stderr.log",
                    qt_platform="offscreen",
                    timeout_seconds=0.1,
                    dry_run=False,
                )

        self.assertEqual(returncode, 124)
        self.assertTrue(timed_out)

    def test_provenance_change_fails_identity_check(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "input.scene"
            path.write_text("{}\n", encoding="utf-8")
            identity = benchmark._file_metadata(path)
            path.write_text('{"changed": true}\n', encoding="utf-8")

            with self.assertRaisesRegex(RuntimeError, "provenance changed"):
                benchmark._assert_file_identity(path, identity)

    def test_nondeterministic_output_hashes_fail_aggregate(self) -> None:
        stability = {
            "entries": [
                {
                    "kind": "scene",
                    "backend": "vulkan",
                    "label": "sample.scene",
                    "source_sha256": "source",
                    "source_path": "/data/sample.scene",
                    "relative_path": "export.png",
                    "run_count": 2,
                    "expected_run_count": 2,
                    "complete_run_coverage": True,
                    "all_hashes_identical": False,
                    "hashes": ["first", "second"],
                }
            ]
        }
        summary = benchmark._build_aggregate_summary(
            runs=[],
            expected_groups={},
            expected_source_paths={},
            harness_errors=[],
            stability_summary=stability,
            dry_run=False,
            run_label="candidate",
        )

        self.assertEqual(summary["status"], "failed")
        self.assertEqual(summary["stability_failure_count"], 1)
        self.assertTrue(
            any(
                "nondeterministic" in reason
                for reason in summary["failures"][0]["reasons"]
            )
        )

    def test_record_only_hash_policy_reports_variation_without_failing(self) -> None:
        key: benchmark.RunGroupKey = (
            "scene",
            "vulkan",
            "sample.scene",
            "source",
            "/data/sample.scene",
        )
        rows = [
            {
                "kind": "scene",
                "backend": "vulkan",
                "label": "sample.scene",
                "source_sha256": "source",
                "source_path": "/data/sample.scene",
                "relative_path": "export.png",
                "sha256": output_hash,
            }
            for output_hash in ("first", "second")
        ]
        stability = benchmark._build_stability_summary(
            rows,
            expected_groups={key: 2},
            hash_policies={"scene": "record-only"},
        )
        summary = benchmark._build_aggregate_summary(
            runs=[],
            expected_groups={},
            expected_source_paths={},
            harness_errors=[],
            stability_summary=stability,
            dry_run=False,
            run_label="candidate",
        )

        entry = stability["entries"][0]
        self.assertFalse(entry["all_hashes_identical"])
        self.assertFalse(entry["stable_and_complete"])
        self.assertEqual(entry["hash_policy"], "record-only")
        self.assertEqual(stability["hash_variation_count"], 1)
        self.assertEqual(summary["hash_variation_count"], 1)
        self.assertEqual(summary["stability_failure_count"], 0)
        self.assertEqual(summary["status"], "passed")

    def test_record_only_hash_policy_does_not_hide_missing_run_coverage(self) -> None:
        stability = {
            "entries": [
                {
                    "kind": "animation",
                    "backend": "vulkan",
                    "label": "sample.animation3d",
                    "source_sha256": "source",
                    "source_path": "/data/sample.animation3d",
                    "relative_path": "frames/frame_000000.png",
                    "run_count": 1,
                    "expected_run_count": 2,
                    "complete_run_coverage": False,
                    "all_hashes_identical": False,
                    "hash_policy": "record-only",
                    "hashes": ["first"],
                }
            ]
        }
        summary = benchmark._build_aggregate_summary(
            runs=[],
            expected_groups={},
            expected_source_paths={},
            harness_errors=[],
            stability_summary=stability,
            dry_run=False,
            run_label="candidate",
        )

        self.assertEqual(summary["status"], "failed")
        self.assertEqual(summary["hash_variation_count"], 1)
        self.assertEqual(summary["stability_failure_count"], 1)
        reasons = summary["failures"][0]["reasons"]
        self.assertTrue(any("appeared in 1 of 2" in reason for reason in reasons))
        self.assertFalse(any("nondeterministic" in reason for reason in reasons))

    def test_run_validation_reports_all_observed_failures(self) -> None:
        failures = benchmark._validate_run_outputs(
            returncode=7,
            file_entries=[{"relative_path": "export.png", "size_bytes": 0}],
            expected_file_count=2,
            perf_summary_path=Path("missing.ndjson"),
            perf_summary_frame_count=0,
            perf_summary_errors=(),
            timed_out=False,
            timeout_seconds=0.0,
            dry_run=False,
        )

        self.assertEqual(len(failures), 4)
        self.assertTrue(any("code 7" in failure for failure in failures))
        self.assertTrue(any("expected 2" in failure for failure in failures))
        self.assertTrue(any("zero-byte" in failure for failure in failures))
        self.assertTrue(any("perf summary" in failure for failure in failures))

    def test_run_validation_requires_exact_animation_frame_indices(self) -> None:
        entries = [
            {"relative_path": "frames/frame_000011.png", "size_bytes": 1},
            {"relative_path": "frames/frame_000012.png", "size_bytes": 1},
        ]

        failures = benchmark._validate_run_outputs(
            returncode=0,
            file_entries=entries,
            expected_file_count=2,
            perf_summary_path=None,
            perf_summary_frame_count=0,
            perf_summary_errors=(),
            timed_out=False,
            timeout_seconds=0.0,
            dry_run=False,
            expected_animation_frame_range=range(10, 12),
        )

        self.assertIn("missing animation frame index(es): 10", failures)
        self.assertIn(
            "unexpected animation output file(s): frames/frame_000012.png",
            failures,
        )

    def test_run_validation_accepts_total_derived_frame_padding(self) -> None:
        entries = [
            {"relative_path": "frames/frame_0000000.png", "size_bytes": 1},
            {"relative_path": "frames/frame_0000001.png", "size_bytes": 1},
        ]

        failures = benchmark._validate_run_outputs(
            returncode=0,
            file_entries=entries,
            expected_file_count=2,
            perf_summary_path=None,
            perf_summary_frame_count=0,
            perf_summary_errors=(),
            timed_out=False,
            timeout_seconds=0.0,
            dry_run=False,
            expected_animation_frame_range=range(0, 2),
        )

        self.assertEqual(failures, ())

    def test_frame_validation_rejects_malformed_and_duplicate_indices(self) -> None:
        failures = benchmark._animation_frame_output_failures(
            [
                {"relative_path": "frames/frame_000010.png", "size_bytes": 1},
                {"relative_path": "frames/frame_10.png", "size_bytes": 1},
                {"relative_path": "frames/frame_bad.png", "size_bytes": 1},
            ],
            range(10, 11),
        )

        self.assertIn(
            "unexpected animation output file(s): frames/frame_bad.png", failures
        )
        self.assertTrue(
            any(
                failure.startswith("duplicate animation frame index(es): 10:")
                for failure in failures
            )
        )

    def test_opengl_command_keeps_perf_collection_disabled_after_extra_args(
        self,
    ) -> None:
        perf_mode, perf_path = benchmark._vulkan_perf_summary_config(
            "opengl", Path("run"), "full"
        )
        command = benchmark._base_command(
            Path("Atlas"),
            "opengl",
            "",
            ["--atlas_perf_mode=full"],
            perf_mode=perf_mode,
            perf_summary_path=perf_path,
        )

        self.assertEqual(perf_mode, "off")
        self.assertIsNone(perf_path)
        self.assertGreater(
            command.index("--atlas_perf_mode=off"),
            command.index("--atlas_perf_mode=full"),
        )

    def test_multi_process_animation_detection_requires_devices_and_frames(
        self,
    ) -> None:
        self.assertTrue(
            benchmark._animation_uses_multiple_processes(
                ["--use_gpu_devices=0,1"], expected_frame_count=2
            )
        )
        self.assertFalse(
            benchmark._animation_uses_multiple_processes(
                ["--use_gpu_devices=0,1"], expected_frame_count=1
            )
        )
        self.assertFalse(
            benchmark._animation_uses_multiple_processes(
                ["--use_gpu_devices=0"], expected_frame_count=2
            )
        )
        self.assertFalse(
            benchmark._animation_uses_multiple_processes(
                ["--use_gpu_devices=0,1", "--use_gpu_devices=0"],
                expected_frame_count=2,
            )
        )
        self.assertEqual(
            benchmark._expected_animation_worker_device_indices(
                ["--use_gpu_devices=", "--atlas_vk_device_index=3"], 2
            ),
            (3,),
        )
        self.assertEqual(
            benchmark._expected_animation_worker_device_indices(
                ["--use_gpu_devices=", "--atlas_vk_device_index", "-1"], 2
            ),
            (None,),
        )

    def test_absent_routing_flags_receive_explicit_defaults(self) -> None:
        scene_args = benchmark._with_absent_flag_defaults(
            ["--scene-only=true"], benchmark._SCENE_ROUTING_FLAG_DEFAULTS
        )
        animation_args = benchmark._with_absent_flag_defaults(
            ["--use_gpu_devices=0,1"],
            benchmark._ANIMATION_ROUTING_FLAG_DEFAULTS,
        )

        self.assertEqual(
            scene_args,
            [
                "--scene-only=true",
                "--use_gpu_devices=",
                "--atlas_vk_device_index=-1",
                "--atlas_vk_multi_device_tile_worker_indices=",
            ],
        )
        self.assertEqual(
            animation_args,
            [
                "--use_gpu_devices=0,1",
                "--atlas_vk_device_index=-1",
                "--animation_gpu_device_frame_weights=",
            ],
        )

    def test_validate_args_rejects_non_finite_child_timeout(self) -> None:
        for timeout in ("nan", "inf", "-inf"):
            with self.subTest(timeout=timeout):
                with mock.patch.object(
                    sys,
                    "argv",
                    [
                        "export_scene_animation_stability.py",
                        f"--child-timeout-seconds={timeout}",
                    ],
                ):
                    args = benchmark._parse_args()
                with self.assertRaisesRegex(ValueError, "finite and non-negative"):
                    benchmark._validate_args(args)

    def test_multi_process_animation_rejects_supervisor_only_timeout(self) -> None:
        benchmark._validate_multi_process_animation_timeout(
            ["--use_gpu_devices=0,1"], [1], timeout_seconds=5.0
        )
        benchmark._validate_multi_process_animation_timeout(
            ["--use_gpu_devices=0,1"], [4], timeout_seconds=0.0
        )
        with self.assertRaisesRegex(ValueError, "must be zero"):
            benchmark._validate_multi_process_animation_timeout(
                ["--use_gpu_devices=0,1"], [4], timeout_seconds=5.0
            )

    def test_dry_run_writes_complete_manifest_and_unique_vulkan_perf_paths(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            scene_path = root / "sample.scene"
            scene_path.write_text("{}\n", encoding="utf-8")
            animation_path = root / "sample.animation3d"
            animation_path.write_text(
                json.dumps({"Animation3D": {"Duration": 1.25}}), encoding="utf-8"
            )
            output_root = root / "results"
            argv = [
                "export_scene_animation_stability.py",
                "--atlas-path",
                sys.executable,
                "--output-root",
                str(output_root),
                "--scene",
                str(scene_path),
                "--animation",
                str(animation_path),
                "--backend",
                "vulkan",
                "--extra-arg=--atlas_default_render_backend=opengl",
                "--extra-arg=--atlas_perf_mode=off",
                "--extra-arg=--atlas_perf_summary=json:/tmp/not-authoritative.ndjson",
                "--extra-arg=--common=true",
                "--scene-extra-arg=--scene-only=true",
                "--animation-extra-arg=--animation-only=true",
                "--animation-hash-policy",
                "record-only",
                "--scene-runs",
                "1",
                "--animation-runs",
                "1",
                "--animation-fps",
                "4",
                "--dry-run",
            ]

            with (
                mock.patch.object(sys, "argv", argv),
                mock.patch.object(benchmark, "_git_metadata", return_value={}),
                mock.patch.object(benchmark, "_host_metadata", return_value={}),
            ):
                self.assertEqual(benchmark.main(), 0)

            manifest = json.loads(
                (output_root / "manifest.json").read_text(encoding="utf-8")
            )
            self.assertEqual(manifest["vulkan_perf_mode"], "light")
            self.assertEqual(len(manifest["dataset_hashes"]), 2)
            self.assertEqual(manifest["scene_extra_args"], ["--scene-only=true"])
            self.assertEqual(
                manifest["animation_extra_args"], ["--animation-only=true"]
            )
            self.assertEqual(manifest["scene_hash_policy"], "exact")
            self.assertEqual(manifest["animation_hash_policy"], "record-only")
            animation_metadata = next(
                entry
                for entry in manifest["input_files"]
                if entry["kind"] == "animation"
            )
            self.assertEqual(animation_metadata["expected_frame_count"], 5)

            scene_run_path = (
                output_root
                / "vulkan"
                / "scene"
                / scene_path.name
                / "run_01"
                / "run.json"
            )
            animation_run_path = (
                output_root
                / "vulkan"
                / "animation"
                / animation_path.name
                / "run_01"
                / "run.json"
            )
            scene_run = json.loads(scene_run_path.read_text(encoding="utf-8"))["run"]
            animation_run = json.loads(animation_run_path.read_text(encoding="utf-8"))[
                "run"
            ]
            self.assertNotEqual(
                scene_run["perf_summary_path"], animation_run["perf_summary_path"]
            )
            self.assertIn(
                f"--atlas_perf_summary=json:{scene_run['perf_summary_path']}",
                scene_run["command"],
            )
            self.assertGreater(
                scene_run["command"].index("--atlas_default_render_backend=vulkan"),
                scene_run["command"].index("--atlas_default_render_backend=opengl"),
            )
            self.assertGreater(
                scene_run["command"].index("--atlas_perf_mode=light"),
                scene_run["command"].index("--atlas_perf_mode=off"),
            )
            self.assertIn("--common=true", scene_run["command"])
            self.assertIn("--common=true", animation_run["command"])
            self.assertIn("--scene-only=true", scene_run["command"])
            self.assertLess(
                scene_run["command"].index("--common=true"),
                scene_run["command"].index("--scene-only=true"),
            )
            self.assertNotIn("--scene-only=true", animation_run["command"])
            self.assertIn("--animation-only=true", animation_run["command"])
            self.assertLess(
                animation_run["command"].index("--common=true"),
                animation_run["command"].index("--animation-only=true"),
            )
            self.assertNotIn("--animation-only=true", scene_run["command"])
            self.assertIn("--use_gpu_devices=", scene_run["command"])
            self.assertIn(
                "--atlas_vk_multi_device_tile_worker_indices=",
                scene_run["command"],
            )
            self.assertIn("--use_gpu_devices=", animation_run["command"])
            self.assertIn(
                "--animation_gpu_device_frame_weights=",
                animation_run["command"],
            )
            self.assertNotIn(
                "--atlas_vk_multi_device_tile_worker_indices=",
                animation_run["command"],
            )
            self.assertEqual(animation_run["expected_file_count"], 5)

            aggregate = json.loads(
                (output_root / "aggregate_summary.json").read_text(encoding="utf-8")
            )
            self.assertEqual(aggregate["status"], "dry_run")
            self.assertTrue(aggregate["all_runs_complete"])

    def test_multi_process_animation_records_wall_time_without_perf_summary(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            animation_path = root / "sample.animation3d"
            animation_path.write_text(
                json.dumps({"Animation3D": {"Duration": 1.0}}), encoding="utf-8"
            )
            output_root = root / "results"
            argv = [
                "export_scene_animation_stability.py",
                "--atlas-path",
                sys.executable,
                "--output-root",
                str(output_root),
                "--animation",
                str(animation_path),
                "--backend",
                "vulkan",
                "--animation-extra-arg=--use_gpu_devices=0,1",
                "--animation-extra-arg=--animation_gpu_device_frame_weights=2,1",
                "--animation-extra-arg=--animation_worker_report_directory=/not-authoritative",
                "--animation-runs",
                "1",
                "--animation-fps",
                "4",
                "--dry-run",
            ]

            with (
                mock.patch.object(sys, "argv", argv),
                mock.patch.object(benchmark, "_git_metadata", return_value={}),
                mock.patch.object(benchmark, "_host_metadata", return_value={}),
            ):
                self.assertEqual(benchmark.main(), 0)

            manifest = json.loads(
                (output_root / "manifest.json").read_text(encoding="utf-8")
            )
            self.assertEqual(manifest["scenes"], [])
            self.assertEqual(manifest["animations"], [str(animation_path.resolve())])
            self.assertEqual(
                manifest["animation_extra_args"],
                [
                    "--use_gpu_devices=0,1",
                    "--animation_gpu_device_frame_weights=2,1",
                    "--animation_worker_report_directory=/not-authoritative",
                ],
            )

            run_path = (
                output_root
                / "vulkan"
                / "animation"
                / animation_path.name
                / "run_01"
                / "run.json"
            )
            run_payload = json.loads(run_path.read_text(encoding="utf-8"))
            run = run_payload["run"]
            worker_report_directory = (run_path.parent / "animation_workers").resolve()
            self.assertIsNone(run["perf_summary_path"])
            self.assertEqual(run["perf_summary_frame_count"], 0)
            self.assertEqual(
                run["perf_summary_profile"],
                benchmark._MULTI_PROCESS_ANIMATION_PERF_PROFILE,
            )
            self.assertEqual(
                run["perf_summary_unavailable_metrics"],
                list(benchmark._MULTI_PROCESS_ANIMATION_UNAVAILABLE_METRICS),
            )
            self.assertIn("--atlas_perf_mode=off", run["command"])
            self.assertIn("--use_gpu_devices=0,1", run["command"])
            self.assertIn("--animation_gpu_device_frame_weights=2,1", run["command"])
            self.assertNotIn("--use_gpu_devices=", run["command"])
            self.assertNotIn("--animation_gpu_device_frame_weights=", run["command"])
            self.assertFalse(
                any(
                    argument.startswith("--atlas_perf_summary=json:")
                    for argument in run["command"]
                )
            )
            self.assertTrue(worker_report_directory.is_dir())
            self.assertIn(
                f"{benchmark._ANIMATION_WORKER_REPORT_FLAG}={worker_report_directory}",
                run["command"],
            )
            self.assertGreater(
                run["command"].index(
                    f"{benchmark._ANIMATION_WORKER_REPORT_FLAG}={worker_report_directory}"
                ),
                run["command"].index(
                    f"{benchmark._ANIMATION_WORKER_REPORT_FLAG}=/not-authoritative"
                ),
            )
            self.assertEqual(
                run["animation_worker_report_directory"],
                str(worker_report_directory),
            )
            self.assertEqual(run["animation_worker_report_expected_count"], 2)
            self.assertEqual(run["animation_worker_report_count"], 0)
            self.assertEqual(run["animation_worker_report_errors"], [])
            self.assertEqual(run_payload["animation_worker_reports"], [])
            self.assertEqual(run["expected_file_count"], 4)

            aggregate = json.loads(
                (output_root / "aggregate_summary.json").read_text(encoding="utf-8")
            )
            self.assertEqual(aggregate["status"], "dry_run")
            self.assertEqual(aggregate["recorded_run_count"], 1)

    def test_child_process_failure_makes_complete_aggregate_fail(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            scene_path = root / "sample.scene"
            scene_path.write_text("{}\n", encoding="utf-8")
            animation_path = root / "sample.animation3d"
            animation_path.write_text(
                json.dumps({"Animation3D": {"Duration": 0.1}}), encoding="utf-8"
            )
            output_root = root / "results"
            argv = [
                "export_scene_animation_stability.py",
                "--atlas-path",
                sys.executable,
                "--output-root",
                str(output_root),
                "--scene",
                str(scene_path),
                "--animation",
                str(animation_path),
                "--backend",
                "vulkan",
                "--scene-runs",
                "1",
                "--animation-runs",
                "1",
                "--vulkan-perf-mode",
                "off",
            ]

            with (
                mock.patch.object(sys, "argv", argv),
                mock.patch.object(benchmark, "_git_metadata", return_value={}),
                mock.patch.object(benchmark, "_host_metadata", return_value={}),
            ):
                self.assertEqual(benchmark.main(), 1)

            aggregate = json.loads(
                (output_root / "aggregate_summary.json").read_text(encoding="utf-8")
            )
            self.assertEqual(aggregate["status"], "failed")
            self.assertTrue(aggregate["all_runs_complete"])
            self.assertEqual(aggregate["recorded_run_count"], 2)
            self.assertEqual(aggregate["successful_run_count"], 0)
            self.assertEqual(len(aggregate["failures"]), 2)


if __name__ == "__main__":
    unittest.main()
