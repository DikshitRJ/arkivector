## ADDED Requirements

### Requirement: Static, lightweight site
The site SHALL build to static files with no backend and SHALL ship under 100 kB of gzipped JavaScript (React 19 itself is about 70 kB).

#### Scenario: Build
- **WHEN** `npm run build` runs in `web/`
- **THEN** `web/dist` contains the complete site and the type check passes

### Requirement: Black and white only
All rendered color SHALL be #000000 or #ffffff. Tones SHALL come from line weight, dashes, hatching, and dots.

#### Scenario: No grey
- **WHEN** the stylesheets and SVGs are inspected
- **THEN** no other color value, gradient, or opacity-based grey appears

### Requirement: Accurate content
Install instructions SHALL match the repository README. Numbers on the site SHALL be ones the project has verified (97 parity tests, 4 vector types, 6 operators, 18 MB image).

#### Scenario: Install commands
- **WHEN** a reader copies the Docker command from the install page
- **THEN** it is the command from the README quick start

### Requirement: Architecture film
The architecture page SHALL play a silent, scrubbable animation in chapters with time-synced subtitles below it, and SHALL offer the same content as readable text.

#### Scenario: Seek
- **WHEN** the reader clicks a chapter or drags the progress bar
- **THEN** the stage and the subtitle jump to that moment

### Requirement: Responsive and accessible
Every page SHALL work from 360 px phone width to desktop without horizontal page scroll. Animations SHALL respect `prefers-reduced-motion`.

#### Scenario: Phone
- **WHEN** the site is viewed at 375 px wide
- **THEN** text is readable, buttons do not wrap, and the film stage fits the width
