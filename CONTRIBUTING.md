# Contributing to konCePCja

Contributions — bug reports, fixes, features, hardware-accuracy improvements —
are welcome.

## Why this file exists

konCePCja's source is offered under the
[konCePCja Source License 1.0.0](LICENSE.md) (source-available; based on
PolyForm Internal Use 1.0.0 with an added Personal & Hobby Use grant). That
license lets people **read**, **use**, and **modify** the source for personal,
hobby, and internal purposes — but it does **not** grant the right to
**distribute** the software or sublicense it. That protects the project from
competing forks and rival builds — but it also means the outbound license can't
be the vehicle for the maintainer to ship official builds incorporating your
change or to relicense it. So contributions are handled by the inbound grant
below.

## How to contribute

1. Open an issue to discuss anything substantial first (so effort isn't wasted).
2. Fork/branch locally, make your change, keep the build green
   (`scripts/build-macos.sh`, `make debug` → `./test_runner`), and match the
   surrounding style.
3. Submit a pull request with a clear description.
4. **Sign off every commit** with a `Signed-off-by` line (`git commit -s`):

   ```
   Signed-off-by: Your Name <you@example.com>
   ```

   This certifies the Developer Certificate of Origin (below) **and**, for this
   project, constitutes the inbound license grant below.

## Inbound license grant

By submitting a contribution to this project (a "Contribution") and signing off
your commits, you represent that you are legally entitled to submit it and you
grant the project maintainer a perpetual, worldwide, non-exclusive,
royalty-free, irrevocable license to use, reproduce, modify, prepare derivative
works of, publicly display, sublicense, and distribute your Contribution and
derivative works thereof, including the right to license it to others under the
project's chosen license (currently the konCePCja Source License 1.0.0) or any other terms
the maintainer selects. You retain all other rights in your Contribution.

This lets the maintainer incorporate your work into a project whose public
license does not itself permit modification or redistribution.

## Developer Certificate of Origin 1.1

By making a contribution to this project, I certify that:

- (a) The contribution was created in whole or in part by me and I have the
  right to submit it under the terms of the inbound license grant above; or
- (b) The contribution is based upon previous work that, to the best of my
  knowledge, is covered under an appropriate license and I have the right under
  that license to submit that work with modifications, whether created in whole
  or in part by me, under the terms of the inbound license grant above; or
- (c) The contribution was provided directly to me by some other person who
  certified (a), (b) or (c) and I have not modified it.
- (d) I understand and agree that this project and the contribution are public
  and that a record of the contribution (including all personal information I
  submit with it, including my sign-off) is maintained indefinitely and may be
  redistributed consistent with this project and the inbound license grant.
