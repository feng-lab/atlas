from __future__ import annotations

"""Gateway model validation helpers.

Some OpenAI-compatible gateways occasionally return Responses/Chat payloads that
are missing the routed model name (resp["model"]) or report an unexpected model.

For OpenAI models, this is usually an upstream routing/proxy issue. We treat it
as a transient error and retry cleanly (discarding the response).
"""

import re


def normalize_model_id(raw: str | None) -> str:
    """Best-effort normalization for gateway-reported model ids.

    Gateways may decorate model names (e.g. vendor prefixes like "openai/gpt-5.2"
    or "openai:gpt-5.2"). We try to normalize without being overly clever.
    """

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


_DATE_SUFFIX_RE = re.compile(r"^(?P<base>.+)-(?P<date>\d{4}-\d{2}-\d{2}|\d{8})$")


def _split_date_suffix(model_id: str) -> tuple[str, str | None]:
    """Return (base, date_suffix) when model_id ends with a date-like suffix."""

    s = str(model_id or "").strip().lower()
    if not s:
        return ("", None)
    m = _DATE_SUFFIX_RE.match(s)
    if not m:
        return (s, None)
    base = str(m.group("base") or "").strip()
    date = str(m.group("date") or "").strip()
    if not base or not date:
        return (s, None)
    return (base, date)


def openai_model_requires_gateway_model(requested_model: str | None) -> bool:
    """Return true when missing/mismatched gateway model should be treated as fatal."""

    m = normalize_model_id(requested_model)
    if not m:
        return False
    if m.startswith("gpt-"):
        return True
    if m.startswith(("o1", "o3", "o4")):
        return True
    return False


def gateway_model_matches_requested(
    requested_model: str | None, gateway_model: str | None
) -> bool:
    """Date-aware model match for OpenAI models.

    Gateways may return:
    - a vendor-prefixed id (e.g. "openai/gpt-5.2")
    - a date-suffixed id (e.g. "gpt-4o-2024-08-06")

    We want to accept *date suffix* differences when the user requested the base
    alias ("gpt-4o"), but we do NOT want to accept variant mismatches like
    "gpt-4o" vs "gpt-4o-mini".

    Policy:
    - If the requested model includes an explicit date suffix, require an exact
      match (after normalization).
    - Otherwise, accept when the gateway model equals the requested alias OR
      equals the alias with a date suffix appended.
    """

    req = normalize_model_id(requested_model)
    got = normalize_model_id(gateway_model)
    if not req or not got:
        return False

    if req == got:
        return True

    req_base, req_date = _split_date_suffix(req)
    got_base, got_date = _split_date_suffix(got)

    # If the caller requested a specific dated model, do not accept a different
    # version or a base alias.
    if req_date is not None:
        return (req_base == got_base) and (req_date == got_date)

    # Requested an alias: accept the routed model when it matches the alias
    # exactly, or matches the alias with a date suffix.
    return req_base == got_base
