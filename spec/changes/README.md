# Contract Change Requests (CCR)

`spec/` and `docs/api/` are the **frozen contract** between the daemon track and the SDK track.
Neither track edits them directly. A change goes through a CCR:

1. Copy the template below to `spec/changes/CCR-NNNN-short-name.md` (next free number).
2. Fill in motivation, exact diff to `spec/openapi.yaml` / `docs/api/*.md` / golden files, and the
   compatibility class: `patch` (docs only), `minor` (additive, optional fields/endpoints/capabilities),
   `major` (breaking; not allowed before v2).
3. Open a PR titled `contract: CCR-NNNN <title>` that contains the CCR file **and** the spec diff.
4. The orchestrator (human owner) approves. Both track leads acknowledge in the PR.
5. On merge: bump `spec/VERSION`, regenerate golden files if touched
   (`python3 -I spec/tools/gen_golden.py`), add an entry to `docs/api/changelog.md`.
6. Each track rebases and implements. A daemon advertises new behaviour only through a capability in
   `GET /v1/version`, so SDKs and daemons of different minor versions keep working together.

## Template

```markdown
# CCR-NNNN: <title>
- Status: proposed | accepted | rejected
- Class: patch | minor | major
- Requested by: daemon | sdk-<lang> | orchestrator
## Motivation
## Change (exact)
## Capability flag (if new behaviour)
## Impact on daemon
## Impact on SDKs
```
