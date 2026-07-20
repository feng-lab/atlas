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

    def test_missing_animation_duration_uses_atlas_ten_second_default(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            animation_path = Path(temp_dir) / "sample.animation3d"
            animation_path.write_text(json.dumps({"Animation3D": {}}), encoding="utf-8")

            self.assertEqual(
                benchmark._animation_duration_seconds(animation_path), 10.0
            )
            self.assertEqual(
                benchmark._expected_animation_frame_count(
                    animation_path, fps=30, start_frame=0, end_frame=-1
                ),
                300,
            )

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
            self.assertEqual(animation_run["expected_file_count"], 5)

            aggregate = json.loads(
                (output_root / "aggregate_summary.json").read_text(encoding="utf-8")
            )
            self.assertEqual(aggregate["status"], "dry_run")
            self.assertTrue(aggregate["all_runs_complete"])

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
