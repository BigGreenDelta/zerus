"""Project native OpenCode diagnostics without filtering conversation text."""

INSPECTION = "OpenCode session details are unavailable; check OpenCode on the session's computer"
ACTION_FAILED = "OpenCode could not complete this action; inspect the session on its computer"
ACTION_UNCERTAIN = "OpenCode action outcome is uncertain; inspect the session before creating a new request"


def row(value):
    # Native cmd identifies the provider, never an error's contents or name.
    if isinstance(value, dict) and value.get("cmd") == "opencode" and "inspection_reason" in value:
        value["inspection_reason"] = INSPECTION
        value["activity_detail"] = INSPECTION


def action(result, known):
    # The caller has already checked the exact native lifecycle identities.
    # The source marker is written by hgs, not copied from the provider body.
    if not known and result.get("error_source") != "opencode":
        return
    status = result.get("status")
    if status in ("failed", "uncertain") and result.get("error") is not None:
        result["error"] = ACTION_FAILED if status == "failed" else ACTION_UNCERTAIN
