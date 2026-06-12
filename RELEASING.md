# Creating a new release

Update the version in the `VERSION` file:

```
$ echo "X.Y.Z" > VERSION
```

Commit and tag:

```
$ git add VERSION
$ git commit -m "Release vX.Y.Z"
$ git tag vX.Y.Z
$ git push --tags
```

GitHub Actions will build and package for Linux, macOS (with notarization) and Windows (MSVC) automatically on tag push. The release artifacts appear on the GitHub Releases page.

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
