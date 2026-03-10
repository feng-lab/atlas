"""Embedded neuTube tracing / skeletonization presets for the zimg package."""

from __future__ import annotations

from copy import deepcopy
import json
from pathlib import Path
from types import MappingProxyType

COMMAND_CONFIG = {
    "skeletonize": {"include": "skeletonize.json"},
    "trace": {"include": "trace_config.json"},
}

COMMAND_CONFIG_MB = {
    "skeletonize": {"include": "skeletonize_mb.json"},
    "trace": {"include": "trace.json"},
}

FLYEM_SKELETONIZE = {
    "downsampleInterval": [7, 7, 7],
    "minimalLength": 40,
    "keepingSingleObject": True,
    "minimalObjectSize": 128,
    "fillingHole": True,
    "rebase": True,
    "maximalDistance": 50,
}

SKELETONIZE = {
    "downsampleInterval": [0, 0, 0],
    "minimalLength": 40,
    "finalMinimalLength": 0,
    "keepingSingleObject": True,
    "rebase": True,
    "fillingHole": True,
    "maximalDistance": 100,
}

SKELETONIZE_FIB25_LEN40 = {
    "downsampleInterval": [3, 3, 3],
    "minimalLength": 40,
    "keepingSingleObject": True,
    "minimalObjectSize": 128,
    "fillingHole": True,
    "rebase": True,
    "maximalDistance": 50,
}

SKELETONIZE_MB = {
    "downsampleInterval": [7, 7, 7],
    "minimalLength": 40,
    "keepingSingleObject": True,
    "minimalObjectSize": 128,
    "fillingHole": True,
    "rebase": True,
    "maximalDistance": 50,
}

SKELETONIZE_MB_LEN40 = {
    "downsampleInterval": [4, 4, 4],
    "minimalLength": 40,
    "keepingSingleObject": True,
    "minimalObjectSize": 128,
    "fillingHole": True,
    "rebase": True,
    "maximalDistance": 50,
}

TRACE = {
    "default": "trace_config.json",
}

TRACE_CONFIG = {
    "tag": "trace configuration",
    "default": {
        "minimalScoreAuto": 0.3,
        "minimalScoreManual": 0.3,
        "minimalScoreSeed": 0.35,
        "minimalScore2d": 0.5,
        "refit": False,
        "spTest": False,
        "crossoverTest": False,
        "tuneEnd": True,
        "edgePath": False,
        "enhanceMask": False,
        "seedMethod": 1,
        "recover": 1,
        "maxEucDist": 10,
    },
    "level": {
        "1": {
            "seedMethod": 2,
            "recover": 0,
        },
        "2": {
            "seedMethod": 2,
            "recover": 1,
        },
        "3": {
            "seedMethod": 2,
            "spTest": True,
            "recover": 1,
        },
        "4": {
            "seedMethod": 2,
            "spTest": True,
            "enhanceMask": True,
            "recover": 1,
        },
        "5": {
            "seedMethod": 2,
            "spTest": True,
            "enhanceMask": True,
            "recover": 1,
        },
        "6": {
            "seedMethod": 2,
            "spTest": True,
            "enhanceMask": True,
            "recover": 1,
            "refit": True,
        },
    },
}

TRACE_CONFIG_BIOCYTIN = {
    "tag": "trace configuration",
    "default": {
        "minimalScoreAuto": 0.2,
        "minimalScoreManual": 0.2,
        "minimalScoreSeed": 0.25,
        "minimalScore2d": 0.5,
        "refit": False,
        "spTest": False,
        "crossoverTest": False,
        "tuneEnd": True,
        "edgePath": False,
        "enhanceMask": False,
        "seedMethod": 1,
        "recover": 0,
        "maxEucDist": 10,
    },
    "level": {
        "1": {
            "seedMethod": 2,
            "recover": 0,
        },
        "2": {
            "seedMethod": 2,
            "recover": 1,
        },
        "3": {
            "seedMethod": 2,
            "spTest": True,
            "recover": 1,
        },
        "4": {
            "seedMethod": 2,
            "spTest": True,
            "enhanceMask": True,
            "recover": 1,
        },
        "5": {
            "seedMethod": 2,
            "spTest": True,
            "enhanceMask": True,
            "recover": 1,
        },
        "6": {
            "seedMethod": 2,
            "spTest": True,
            "enhanceMask": True,
            "recover": 1,
            "refit": True,
        },
    },
}

_PRESETS_BY_NAME = {
    "command_config.json": COMMAND_CONFIG,
    "command_config_mb.json": COMMAND_CONFIG_MB,
    "flyem_skeletonize.json": FLYEM_SKELETONIZE,
    "skeletonize.json": SKELETONIZE,
    "skeletonize_fib25_len40.json": SKELETONIZE_FIB25_LEN40,
    "skeletonize_mb.json": SKELETONIZE_MB,
    "skeletonize_mb_len40.json": SKELETONIZE_MB_LEN40,
    "trace.json": TRACE,
    "trace_config.json": TRACE_CONFIG,
    "trace_config_biocytin.json": TRACE_CONFIG_BIOCYTIN,
}


def _preset_to_text(preset: object) -> str:
    return json.dumps(preset, indent=2) + "\n"


ALL_PRESET_NAMES = tuple(sorted(_PRESETS_BY_NAME))
PARSED_PRESET_NAMES = ALL_PRESET_NAMES
TEXT_ONLY_PRESET_NAMES = tuple()
PARSED_BY_NAME = MappingProxyType(_PRESETS_BY_NAME)
RAW_TEXT_BY_NAME = MappingProxyType(
    {name: _preset_to_text(preset) for name, preset in _PRESETS_BY_NAME.items()}
)


def get_preset(name: str):
    """Return a deep copy of a packaged preset by filename."""
    try:
        return deepcopy(_PRESETS_BY_NAME[name])
    except KeyError as exc:
        raise KeyError(
            f"Unknown preset {name!r}. Known presets: {ALL_PRESET_NAMES}"
        ) from exc


def get_preset_text(name: str) -> str:
    """Return a JSON text representation of a packaged preset by filename."""
    try:
        return _preset_to_text(_PRESETS_BY_NAME[name])
    except KeyError as exc:
        raise KeyError(
            f"Unknown preset {name!r}. Known presets: {ALL_PRESET_NAMES}"
        ) from exc


def write_preset(name: str, path: str | Path) -> Path:
    """Write a packaged preset to ``path`` and return the resulting ``Path``."""
    out = Path(path)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(get_preset_text(name), encoding="utf-8")
    return out


COMMAND_CONFIG_TEXT = get_preset_text("command_config.json")
COMMAND_CONFIG_MB_TEXT = get_preset_text("command_config_mb.json")
FLYEM_SKELETONIZE_TEXT = get_preset_text("flyem_skeletonize.json")
SKELETONIZE_TEXT = get_preset_text("skeletonize.json")
SKELETONIZE_FIB25_LEN40_TEXT = get_preset_text("skeletonize_fib25_len40.json")
SKELETONIZE_MB_TEXT = get_preset_text("skeletonize_mb.json")
SKELETONIZE_MB_LEN40_TEXT = get_preset_text("skeletonize_mb_len40.json")
TRACE_TEXT = get_preset_text("trace.json")
TRACE_CONFIG_TEXT = get_preset_text("trace_config.json")
TRACE_CONFIG_BIOCYTIN_TEXT = get_preset_text("trace_config_biocytin.json")

__all__ = [
    "ALL_PRESET_NAMES",
    "PARSED_PRESET_NAMES",
    "TEXT_ONLY_PRESET_NAMES",
    "RAW_TEXT_BY_NAME",
    "PARSED_BY_NAME",
    "get_preset",
    "get_preset_text",
    "write_preset",
    "COMMAND_CONFIG",
    "COMMAND_CONFIG_TEXT",
    "COMMAND_CONFIG_MB",
    "COMMAND_CONFIG_MB_TEXT",
    "FLYEM_SKELETONIZE",
    "FLYEM_SKELETONIZE_TEXT",
    "SKELETONIZE",
    "SKELETONIZE_TEXT",
    "SKELETONIZE_FIB25_LEN40",
    "SKELETONIZE_FIB25_LEN40_TEXT",
    "SKELETONIZE_MB",
    "SKELETONIZE_MB_TEXT",
    "SKELETONIZE_MB_LEN40",
    "SKELETONIZE_MB_LEN40_TEXT",
    "TRACE",
    "TRACE_TEXT",
    "TRACE_CONFIG",
    "TRACE_CONFIG_TEXT",
    "TRACE_CONFIG_BIOCYTIN",
    "TRACE_CONFIG_BIOCYTIN_TEXT",
]
