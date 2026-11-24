# Best-effort manager ownership persistence

## Requirement

Failures of `agora_shm_manager_state_open`, `open_at`, or `add` must not
prevent manager startup or media Writer creation. The user selected media
availability over mandatory ownership persistence after reviewing leak risks.

## Design

Keep the state helpers' error returns intact so failures remain diagnosable.
The manager treats ownership persistence as best effort: startup logs state
initialization failure and continues with an invalid state lock descriptor;
Writer creation logs registration failure and still installs its live SHM in
the write table. No state failure rolls back an otherwise valid media object.
When state initialization failed, subsequent state updates are skipped.

Media mappings and the write table remain authoritative for normal cleanup.
`remove` still reports notification and actual SHM cleanup failures, but only
logs manifest-update failures. `close` still unlinks every owned Writer;
failed updates retain existing state files for later recovery where possible.
SHM creation, mapping, initialization, API validation, and transport errors
retain their existing semantics.

No directory recreation, lock replacement, automatic unlink on EEXIST, or
background persistence retry is added. A missing manifest can mean abandoned
SHM is not reclaimed after a crash and can block later same-name creation.

## Validation

The default state root is `/var/lib/agora/shm-manager`. Missing ancestors are
created with mode 0700 without changing existing permissions or ownership.
Creation still requires filesystem write permissions; failure preserves the
best-effort media behavior. Tests compile with an isolated state-root override
and never initialize the host production state directory. The old `/tmp` state
root is not migrated automatically.

Use test-only wrappers and isolated temporary state roots to exercise state
open failures and real `open_at` failure. Exercise both client and server
Writers, injected registration failures, and deletion of a live instance's
directory while its lock descriptor remains open. Verify real SHM creation,
frame delivery, duplicate rejection, remove/recreate, and close cleanup.
Run existing state, poll, transport-recovery, and socket-error regressions.
