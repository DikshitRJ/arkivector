## ADDED Requirements

### Requirement: One source for user guides
User guides SHALL live in `docs/guide/` and be published unchanged to the website (`/docs`) and
the GitHub wiki, so the three never disagree.

#### Scenario: Edit a guide
- **WHEN** `docs/guide/python.md` changes on main
- **THEN** the website build renders the new text and the wiki workflow publishes it as `Python-SDK`

### Requirement: Examples are executable
Every request and code example in the guides SHALL run as written against a daemon with the same
version.

#### Scenario: Copy an example
- **WHEN** a reader copies the curl walkthrough into a shell with a running daemon and a signer
- **THEN** each command succeeds with a response shaped like the one shown
