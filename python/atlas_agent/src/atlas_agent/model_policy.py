from __future__ import annotations

"""Central model naming and capability policy for Atlas Agent."""

from dataclasses import dataclass


# Keep the default model slug in one place so routine model updates are a
# one-line change unless the new model family has different API behavior.
DEFAULT_MODEL = "gpt-5.5"

GPT_5_4_PREFIX = "gpt-5.4"
GATEWAY_MODEL_REQUIRED_PREFIXES = ("gpt-", "o1", "o3", "o4")
REASONING_SUMMARY_MODEL_PREFIXES = ("gpt-5", "o3", "o4-mini")
TEXT_VERBOSITY_MODEL_PREFIXES = ("gpt-5",)


@dataclass(frozen=True)
class ModelContextBudget:
    context_window: int
    effective_context_window_percent: int = 100

    def effective_tokens(self) -> int:
        return max(
            1,
            (int(self.context_window) * int(self.effective_context_window_percent))
            // 100,
        )


@dataclass(frozen=True)
class ImageTokenCostTileModel:
    base_tokens: int
    tile_tokens: int


@dataclass(frozen=True)
class ImageTokenCostPatchModel:
    multiplier: float
    patch_budget: int
    high_patch_budget: int | None = None
    original_patch_budget: int | None = None
    auto_uses_original: bool = False

    def patch_budget_for_detail(self, detail: str | None) -> int:
        d = str(detail or "auto").strip().lower()
        if d == "original":
            return int(self.original_patch_budget or self.patch_budget)
        if d == "high":
            return int(self.high_patch_budget or self.patch_budget)
        if d == "auto" and self.auto_uses_original:
            return int(self.original_patch_budget or self.patch_budget)
        return int(self.patch_budget)


MODEL_EFFECTIVE_CONTEXT_WINDOW_PERCENT = 95
GPT_5_FAMILY_CONTEXT_BUDGET = ModelContextBudget(
    context_window=272_000,
    effective_context_window_percent=MODEL_EFFECTIVE_CONTEXT_WINDOW_PERCENT,
)
MODEL_CONTEXT_BUDGETS: dict[str, ModelContextBudget] = {
    DEFAULT_MODEL: GPT_5_FAMILY_CONTEXT_BUDGET,
    GPT_5_4_PREFIX: GPT_5_FAMILY_CONTEXT_BUDGET,
    f"{GPT_5_4_PREFIX}-mini": GPT_5_FAMILY_CONTEXT_BUDGET,
}


def normalize_model_id(raw: str | None) -> str:
    """Best-effort normalization for provider/gateway-reported model ids."""

    s = str(raw or "").strip()
    if not s:
        return ""
    # Drop any trailing annotations (rare but seen in some gateways).
    s = s.split()[0]
    # Strip vendor prefixes (keep the last segment).
    #
    # Note: do not split on '@' here. Some gateways use '@' to attach routing
    # metadata or versions (e.g., model@YYYY-MM-DD). Treat it as part of the id.
    for sep in ("/", ":"):
        if sep in s:
            s = s.split(sep)[-1]
    return s.strip().lower()


def _has_prefix(model: str | None, prefixes: tuple[str, ...]) -> bool:
    m = normalize_model_id(model)
    return any(m.startswith(prefix) for prefix in prefixes)


def _gpt5_minor_version(model: str) -> int | None:
    prefix = "gpt-5."
    if not model.startswith(prefix):
        return None
    digits = []
    for ch in model[len(prefix) :]:
        if not ch.isdigit():
            break
        digits.append(ch)
    if not digits:
        return None
    try:
        return int("".join(digits))
    except ValueError:
        return None


def _gpt5_minor_variant_version(model: str, variant: str) -> int | None:
    prefix = "gpt-5."
    if not model.startswith(prefix):
        return None
    rest = model[len(prefix) :]
    digits = []
    for ch in rest:
        if not ch.isdigit():
            break
        digits.append(ch)
    if not digits:
        return None
    suffix = f"-{variant}"
    if not rest[len(digits) :].startswith(suffix):
        return None
    try:
        return int("".join(digits))
    except ValueError:
        return None


def model_requires_gateway_model(requested_model: str | None) -> bool:
    """Return true when missing/mismatched gateway model should be treated as fatal."""

    return _has_prefix(requested_model, GATEWAY_MODEL_REQUIRED_PREFIXES)


def model_supports_reasoning_summaries(model: str | None) -> bool:
    """Best-effort Responses API reasoning-summary capability detection."""

    return _has_prefix(model, REASONING_SUMMARY_MODEL_PREFIXES)


def model_supports_text_verbosity(model: str | None) -> bool:
    """Best-effort Responses API text.verbosity capability detection."""

    return _has_prefix(model, TEXT_VERBOSITY_MODEL_PREFIXES)


def model_context_budget(model: str | None) -> ModelContextBudget | None:
    """Return model context budget metadata when known."""

    m = normalize_model_id(model)
    if not m:
        return None
    budget = MODEL_CONTEXT_BUDGETS.get(m)
    if budget is not None:
        return budget
    if m.startswith("gpt-5"):
        return GPT_5_FAMILY_CONTEXT_BUDGET
    return None


def model_context_window_tokens(model: str | None) -> int | None:
    budget = model_context_budget(model)
    return int(budget.context_window) if budget is not None else None


def model_effective_context_window_tokens(model: str | None) -> int | None:
    budget = model_context_budget(model)
    return int(budget.effective_tokens()) if budget is not None else None


def guess_model_effective_input_budget_tokens(model: str | None) -> int | None:
    """Best-effort effective *input* token budget guess from model name.

    This fallback follows checked model metadata when available. It is used for
    proactive prompt-budget compaction when the provider cannot report token
    limits, or as the cap for providers that advertise a larger public maximum.
    """

    m = normalize_model_id(model)
    if not m:
        return None
    policy_budget = model_effective_context_window_tokens(m)
    if policy_budget is not None:
        return policy_budget
    if m.startswith("gpt-3.5"):
        return 16_384
    if m.startswith(("gpt-4o", "gpt-4.1", "gpt-4")):
        return 128_000
    if m.startswith(("o3", "o4")):
        return 128_000
    # Unknown provider/model: fall back to a reasonable modern default.
    return 128_000


def image_token_cost_profile_for_model(
    model: str | None,
) -> ImageTokenCostTileModel | ImageTokenCostPatchModel | None:
    """Best-effort model image token profile for proactive estimation."""

    m = normalize_model_id(model)
    if not m:
        return None

    # Patch-based models. Order matters: these prefixes overlap with broader
    # GPT-5 family names.
    gpt5_minor_mini = _gpt5_minor_variant_version(m, "mini")
    gpt5_minor_nano = _gpt5_minor_variant_version(m, "nano")
    if gpt5_minor_mini is not None and gpt5_minor_mini >= 4:
        return ImageTokenCostPatchModel(multiplier=1.62, patch_budget=1536)
    if gpt5_minor_nano is not None and gpt5_minor_nano >= 4:
        return ImageTokenCostPatchModel(multiplier=2.46, patch_budget=1536)
    if m.startswith("gpt-5-mini"):
        return ImageTokenCostPatchModel(multiplier=1.62, patch_budget=1536)
    if m.startswith("gpt-5-nano"):
        return ImageTokenCostPatchModel(multiplier=2.46, patch_budget=1536)
    if m.startswith("gpt-4.1-mini"):
        return ImageTokenCostPatchModel(multiplier=1.62, patch_budget=1536)
    if m.startswith("gpt-4.1-nano"):
        return ImageTokenCostPatchModel(multiplier=2.46, patch_budget=1536)
    if m.startswith("o4-mini"):
        return ImageTokenCostPatchModel(multiplier=1.72, patch_budget=1536)
    minor = _gpt5_minor_version(m)
    if minor is not None and minor >= 5:
        return ImageTokenCostPatchModel(
            multiplier=1.0,
            patch_budget=2500,
            high_patch_budget=2500,
            original_patch_budget=10_000,
            auto_uses_original=True,
        )
    if minor is not None and minor == 4:
        return ImageTokenCostPatchModel(
            multiplier=1.0,
            patch_budget=2500,
            high_patch_budget=2500,
            original_patch_budget=10_000,
        )
    if minor is not None and 1 <= minor <= 3:
        return ImageTokenCostPatchModel(multiplier=1.0, patch_budget=1536)

    # Tile-based models.
    if m.startswith("gpt-4o-mini"):
        return ImageTokenCostTileModel(base_tokens=2833, tile_tokens=5667)
    if m.startswith("computer-use-preview"):
        return ImageTokenCostTileModel(base_tokens=65, tile_tokens=129)
    if m.startswith("gpt-5-chat-latest"):
        return ImageTokenCostTileModel(base_tokens=70, tile_tokens=140)
    if m.startswith("gpt-5"):
        return ImageTokenCostTileModel(base_tokens=70, tile_tokens=140)
    if m.startswith("gpt-4.5"):
        return ImageTokenCostTileModel(base_tokens=85, tile_tokens=170)
    if m.startswith("gpt-4.1"):
        return ImageTokenCostTileModel(base_tokens=85, tile_tokens=170)
    if m.startswith("gpt-4o"):
        return ImageTokenCostTileModel(base_tokens=85, tile_tokens=170)
    if m.startswith("o1-pro"):
        return ImageTokenCostTileModel(base_tokens=75, tile_tokens=150)
    if m.startswith("o1"):
        return ImageTokenCostTileModel(base_tokens=75, tile_tokens=150)
    if m.startswith("o3"):
        return ImageTokenCostTileModel(base_tokens=75, tile_tokens=150)

    return None
