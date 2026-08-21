# Contributing to l0destar

Thank you for your interest in helping build l0destar. This document explains how to contribute and what you need to know about licensing before you do.

## Developer Certificate of Origin (DCO)

l0destar uses the [Developer Certificate of Origin](https://developercertificate.org/) instead of a Contributor License Agreement. There is no document to sign and no bot to authorise - every commit you submit must simply carry a `Signed-off-by:` trailer.

By adding `Signed-off-by: Your Name <your@email>` to a commit, you certify that:

- The contribution is your original work, or
- It is based on work licensed under a compatible open-source licence and you have the right to submit it, **and**
- You understand the contribution is public and may be redistributed under the project's licences.

The full text is at <https://developercertificate.org/>. It is one screen long and worth reading once.

### How to sign off

The standard way is to pass `-s` to `git commit`:

    git commit -s -m "Add foo to bar"

This appends a `Signed-off-by:` line using your configured `user.name` and `user.email`. If you forget, amend with `git commit --amend -s` before pushing.

### Note on employer-owned IP

If your employment contract assigns IP rights to your employer, you may need their written permission to contribute. Contributing from a work computer or work email is not by itself permission. If in doubt, ask your employer first - sorting this out before merging is much cheaper than sorting it out afterwards.

## Pull request expectations

- One logical change per PR. Split large changes into reviewable steps.
- Write a commit message that explains *why*, not just *what*.
- All commits must be signed off (`git commit -s`). PRs with unsigned commits will be asked to amend before merge.
- For hardware changes: include rendered schematics and PCB images in the PR description.
- For firmware changes: explain the power impact (sleep current, peak current, duty cycle) where relevant.
- For server changes: include migration plan if schema or API changes.

## Licensing your contributions

By submitting a contribution, you agree it is licensed under the appropriate licence for that artifact type - see [`LICENSE.md`](LICENSE.md):

- Hardware design files → `CERN-OHL-P-2.0`
- Firmware and server code → `Apache-2.0`
- Documentation, enclosure CAD, media → `CC-BY-4.0`

Please include an SPDX header in new source files. Examples are in [`LICENSE.md`](LICENSE.md).

## Code of conduct

Be kind, be specific, be patient. Substantive technical disagreement is welcome; personal hostility is not.
