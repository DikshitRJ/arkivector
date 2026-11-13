# Releasing

Releases are cut from `main` by pushing a version tag:

```bash
git checkout main && git pull
git tag -a v1.2.0 -m "Arkivector v1.2.0"
git push origin v1.2.0
```

The `release` workflow then builds and publishes:

| Artifact | Where |
| :--- | :--- |
| `arkivectord-<v>-linux-x86_64.tar.gz` (libcurl bundled) | GitHub Release |
| Container image (linux/amd64) | `ghcr.io/dikshitrj/arkivectord:<v>`, `:<major>`, `:latest` (smoke-tested after push) |
| Python wheel and sdist | GitHub Release, and PyPI via trusted publishing (environment `pypi`) |
| Node package | GitHub Packages (`@dikshitrj/arkivector`) and `arkivector-node-<v>.tgz` on the release |
| `SHA256SUMS` | GitHub Release |

Version numbers are stamped at build time: the daemon reports the tag in `/v1/version`, and the SDK versions are set from the tag. The release body is rendered from [`.github/release-notes.md`](https://github.com/DikshitRJ/arkivector/blob/main/.github/release-notes.md) and links [`CHANGELOG.md`](https://github.com/DikshitRJ/arkivector/blob/main/CHANGELOG.md), so update the changelog in the pull request before tagging.

Without push access to tags, run the workflow from the Actions tab (**release → Run workflow** on `main`) with `version` = `1.2.0` and **create_release** checked: it builds and publishes everything above, then creates the tag `v1.2.0` on that commit together with the GitHub Release. With `create_release` unchecked it only builds and publishes the packages (useful to test the pipeline).

## Checklist

1. CHANGELOG entry and docs updated, merged to `main`.
2. `daemon`, `sdk` and `contract` workflows green on `main`.
3. Tag and push.
4. Check the release page, `docker pull ghcr.io/dikshitrj/arkivectord:<v>`, `pip install arkivector==<v>`.
