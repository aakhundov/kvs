# Vendored dependencies

Third-party source copied into this tree. It is built by the top-level
`Makefile` but deliberately sits outside `src/`, so it is not swept into
`format`, `tidy`, or the strict warning set those targets imply.

## utest.h

Single-header unit test framework used by the `tests/` suite: tests register
themselves at load time, assertions come in fatal (`ASSERT_*`) and non-fatal
(`EXPECT_*`) forms and print both operands on failure, and the runner can emit
an xunit XML report.

| | |
|---|---|
| Upstream | <https://github.com/sheredom/utest.h> |
| Version | branch `master`, sha256 `469b743a41a66fac605d09a1608eb00cebdf451179505e5cbf22c882b40fb11f` |
| Fetched | 2026-08-20 |
| License | Unlicense (`utest/LICENSE`, also reproduced at the top of the header) |

### What was copied

`utest.h` and `LICENSE`, two files. Upstream publishes no release tags, so the
digest above is the pin: a re-fetch that does not reproduce it is a different
version. Upstream's `test/` and CI configuration are not copied.

Unlike isocline, this header compiles clean under the project's own strict
warning set, so tests are built with `CFLAGS` unchanged rather than with the
relaxed vendor flags.

### Local modifications

None.

### Refreshing

Re-fetch `utest.h` from the upstream default branch and record the new digest
here.
