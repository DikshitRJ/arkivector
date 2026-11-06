## ADDED Requirements

### Requirement: Exact mirror
For each followed collection the daemon SHALL hold exactly the live member entities of Arkiv at its published `as_of_block`, with identical attributes, owners and expiries.

#### Scenario: Bootstrap
- **WHEN** a collection with 1,000 member entities is followed
- **THEN** after bootstrap the daemon's live key set equals the node's

#### Scenario: Restart
- **WHEN** the daemon is killed at a random point and restarted
- **THEN** it resumes from its cursor and converges to the node's live set without a full re-bootstrap

### Requirement: TTL liveness
An entity with `expiresAt = e` SHALL be visible at block e-1 and absent at block e.

#### Scenario: Expiry
- **WHEN** an entity reaches its expiry block
- **THEN** reads at that block no longer return it

### Requirement: Reorg safety
After a chain reorganisation of depth ≤ K the daemon SHALL converge to the canonical chain's state. Deeper reorgs SHALL trigger a re-bootstrap.

#### Scenario: Conflicting reorg
- **WHEN** a depth-5 reorg replaces blocks with conflicting operations
- **THEN** the mirror equals the node's canonical state after catch-up
