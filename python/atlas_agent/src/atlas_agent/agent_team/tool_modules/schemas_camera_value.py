from __future__ import annotations

from typing import Any, Dict

VEC3_NUMBER_SCHEMA: Dict[str, Any] = {
    "type": "array",
    "items": {"type": "number"},
    "minItems": 3,
    "maxItems": 3,
}


# Typed 3D camera value schema (stable, engine-defined)
#
# This mirrors the JSON emitted/consumed by Z3DCameraParameter::jsonValue/readValue.
# Property names intentionally include the parameter-type suffixes (e.g. "Vec3",
# "Float") because those are the canonical json_keys used by Atlas.
CAMERA_TYPED_VALUE_SCHEMA: Dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "properties": {
        "Projection Type StringIntOption": {
            "type": "string",
            "enum": ["Perspective", "Orthographic"],
            "description": "Camera projection type.",
        },
        "Eye Position Vec3": {
            **VEC3_NUMBER_SCHEMA,
            "description": "Camera eye position [x,y,z] in world space.",
        },
        "Center Position Vec3": {
            **VEC3_NUMBER_SCHEMA,
            "description": "Camera look-at center position [x,y,z] in world space.",
        },
        "Up Vector Vec3": {
            **VEC3_NUMBER_SCHEMA,
            "description": (
                "Camera up vector [x,y,z]. Atlas normalizes this vector when applying the camera."
            ),
        },
        "Eye Separation Angle Float": {
            "type": "number",
            "minimum": 1.0,
            "maximum": 80.0,
            "description": "Stereo eye separation angle in degrees.",
        },
        "Field of View Float": {
            "type": "number",
            "minimum": 10.0,
            "maximum": 170.0,
            "description": "Field of view in degrees.",
        },
    },
    "required": [
        "Projection Type StringIntOption",
        "Eye Position Vec3",
        "Center Position Vec3",
        "Up Vector Vec3",
        "Eye Separation Angle Float",
        "Field of View Float",
    ],
}


CAMERA_CONSTRAINTS_SCHEMA: Dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "description": "Camera visibility/coverage constraints.",
    "properties": {
        "keep_visible": {
            "type": "boolean",
            "default": True,
            "description": "When true, validate that the target bbox stays within frame.",
        },
        "margin": {
            "type": "number",
            "default": 0.0,
            "description": "Optional margin around the bbox (fraction of bbox size).",
        },
        "min_coverage": {
            "type": "number",
            "default": 0.95,
            "description": "Minimum required bbox coverage within frame (0..1).",
        },
    },
}


CAMERA_POLICIES_SCHEMA: Dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "description": "Camera adjustment policies used during validation.",
    "properties": {
        "adjust_fov": {
            "type": "boolean",
            "default": False,
            "description": "Allow the validator to adjust field of view to satisfy constraints.",
        },
        "adjust_distance": {
            "type": "boolean",
            "default": False,
            "description": "Allow the validator to adjust camera distance to satisfy constraints.",
        },
    },
}
