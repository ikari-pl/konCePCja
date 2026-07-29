# The version lives in exactly one file

`.release-please-manifest.json` is the **only** source of truth for the version:

```json
{
  ".": "6.1.0"
}
```

Everything else derives from it — do not hand-edit a version anywhere else:

| Consumer | How it reads the version |
|---|---|
| `makefile` | `KONCPC_VERSION` (sed over the manifest) → `-DKONCPC_VERSION_STRING` |
| `makefile` | `VERSION` (archive/.deb/.dmg names) defaults to the same value |
| `CMakeLists.txt` | `string(REGEX MATCH ...)` over the manifest → `project(... VERSION)` → CPack |
| `vcpkg.json` | `version-string`, bumped by release-please via `extra-files` |

`test/version_source_test.cpp` fails the build if the version compiled into the
binary, or `vcpkg.json`, disagrees with the manifest. This is not ceremony: a
top-level `VERSION` file used to hold this role, release-please never updated
it, and it silently sat at `5.10.0` while shipped releases were at `6.1.0` — so
every v6 binary misreported its own version. If that test fails, fix the
parser or the manifest; never "fix" it by editing a second file to match.

# Creating a new release

Releases are cut by release-please, not by hand. Merge conventional commits to
`master`; release-please opens a release PR that bumps
`.release-please-manifest.json` (and `vcpkg.json`) and updates the changelog.
Merging that PR tags the release.

To force a specific version, put `Release-As: X.Y.Z` in a commit message.

GitHub Actions then builds and packages for Linux, macOS (with notarization) and
Windows (MSVC) automatically on tag push. The release artifacts appear on the
GitHub Releases page.

# Creating a minor release

Create a branch from the existing tag:

```
$ git checkout -b vX.Y vX.Y.Z
```

Apply fixes, then tag and push:

```
$ git tag vX.Y.Z
$ git push --tags
```

Verify the GitHub Actions workflows complete successfully on the release page.
