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
    "description": "Camera framing constraints used by the engine-side validator.",
    "properties": {
        "keep_visible": {
            "type": "boolean",
            "default": True,
            "description": (
                "When true, require the target bbox to remain fully within the camera frame (no cropping). "
                "When false, the validator does not enforce framing constraints (but may still report metrics)."
            ),
        },
        "margin": {
            "type": "number",
            "default": 0.0,
            "minimum": 0.0,
            "description": (
                "Optional extra padding around the target bbox (fraction of bbox size). "
                "Higher margins require the camera to back off more."
            ),
        },
        "min_frame_coverage": {
            "type": "number",
            "default": 0.0,
            "minimum": 0.0,
            "maximum": 1.0,
            "description": (
                "Minimum on-screen size of the target bbox (0..1), measured as dominant-dimension fill: "
                "max(projected_width_frac, projected_height_frac). Higher values push toward tighter framing "
                "(larger subjects). Set to 0.0 to disable."
            ),
        },
    },
}


CAMERA_POLICIES_SCHEMA: Dict[str, Any] = {
    "type": "object",
    "additionalProperties": False,
    "description": "Camera adjustment policies used during validation.",
    "properties": {
        "adjust_distance": {
            "type": "boolean",
            "default": False,
            "description": (
                "Allow the validator to suggest a new camera value by dollying (changing eye distance to center) "
                "to satisfy constraints."
            ),
        },
    },
}
