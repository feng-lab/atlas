from __future__ import annotations

"""Gateway model validation helpers.

Some OpenAI-compatible gateways occasionally return Responses/Chat payloads that
are missing the routed model name (resp["model"]) or report an unexpected model.

For model families that require gateway validation, this is usually an upstream
routing/proxy issue. We treat it as a transient error and retry cleanly
(discarding the response).
"""

import re

from .model_policy import normalize_model_id


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


def gateway_model_matches_requested(
    requested_model: str | None, gateway_model: str | None
) -> bool:
    """Date-aware model match for gateway-reported models.

    Gateways may return:
    - a vendor-prefixed id (e.g. "openai/<model>")
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
