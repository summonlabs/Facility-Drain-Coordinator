# Contributing to Facility Drain Coordinator

Thank you for your interest in contributing to Facility Drain Coordinator.
This document describes the contribution terms and the engineering
expectations for this repository.

## License

By contributing to this project, you agree that your contributions are
licensed under the **Apache License, Version 2.0**. Section 5 of that license
governs submissions: unless you explicitly state otherwise, any contribution
you intentionally submit for inclusion in the work is under the terms and
conditions of the Apache License 2.0, without any additional terms or
conditions. See the `LICENSE` file for the full license text and the
`NOTICE` file for attribution and license notices.

There is **no separate Contributor License Agreement (CLA)** and **no
copyright assignment**. You retain ownership of your contributions and grant
the project a license to use them under the terms of the Apache License 2.0.

Do not add `Co-authored-by:` trailers or any other co-author attribution to
commits, and do not add copyright lines for other people or organizations to
source files. Attribution belongs in the `NOTICE` file and in the copyright
line of the file header.

## File headers

Every source file begins with these three lines:

```
// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.
```

CMake files carry the same three lines with `#` comments.

## What belongs here

Facility Drain Coordinator owns the physical fleet lifecycle of a drain: the
durable record of what was asked, which owning system reported what, what
remains unresolved, and whether the evidence justifies removal authority.

It does not own capacity calculation, placement, reservations, actuation,
maintenance orchestration, tenancy, incident response, dashboards or
multi-site federation. Changes that reach into those areas belong in the
component that owns them. The coordinator never performs physical action and
never opens a network connection: a drain happens because the owning system
says it did.

## Coding standards

- C++20 and CMake only. No third-party dependencies. A new dependency needs a
  compelling systems reason, argued in the change itself, and the standard
  library alone is almost always the right answer.
- The public headers in `include/facilitydrain` are a frozen contract. Do not
  change them to make an implementation easier; report the mismatch instead.
- Build cleanly with `/W4 /WX` on MSVC and `-Wall -Wextra -Wpedantic -Werror`
  elsewhere. Warnings are defects: fix the cause rather than suppressing the
  warning, and a build that emits any warning is not finished.
- Identities, generations, revisions and epochs are distinct strong types. Do
  not add implicit conversions between them, and do not compare one against
  another.
- All external input is untrusted. Validate before allocating, use checked
  arithmetic for externally influenced sizes, and reject malformed input
  instead of normalising it.
- Decisions are made from recorded evidence. Never infer an effect from a
  request, from an acknowledgement, or from the absence of a complaint.
- Behaviour must be deterministic: no reliance on wall clock ordering, hash
  container iteration order, or thread scheduling for anything observable.

## Tests

Every behaviour change needs a test that fails without it. Add the smallest
test that proves the change, and keep it passing on its own. A hanging test is
a defect to diagnose, not to bound with a timeout.

## Documentation

Keep the documentation honest about what was actually validated. State what
was built, what was tested, and on which platforms; do not describe a
capability that has not been exercised. Never claim validation that was not
performed.

## Pull requests

Keep changes focused, include the tests that prove them, and make sure the
repository builds and tests cleanly in Release and Debug with warnings treated
as errors. Do not commit build output, install trees, benchmark residue,
editor files or machine-specific paths.
