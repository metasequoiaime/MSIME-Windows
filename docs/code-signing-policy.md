# Code signing policy

Free code signing provided by [SignPath.io](https://about.signpath.io/), certificate by [SignPath Foundation](https://signpath.org/).

**Current status.** The application to the SignPath Foundation open source program is under review, and no release has been signed through SignPath yet. Releases up to and including v0.9.3 were built on a maintainer's machine and signed there with the project's Certum Open Source Developer certificate (`CN=Open Source Developer LU FAN`) through SimplySign, using `installer/package-simplysign.ps1`. Once the application is approved, releases signed with the SignPath Foundation certificate will be built and signed only by the CI process described below, never on a local machine.

## What gets signed

Only the project's own binaries, built from source in this repository:

- the Server (`MetasequoiaImeServer.exe`) and the helper executables shipped with it;
- the TSF text service DLLs (`MetasequoiaImeTsf.dll`, x64 and Win32);
- the installer `MetasequoiaIME_Setup_v<version>.exe`.

Third-party binaries, such as the libraries vcpkg builds for the Server, are shipped as their upstream built them and are never submitted for signing. The exact list is in [`scripts/ci/signpath-files.ps1`](../scripts/ci/signpath-files.ps1), and the matching SignPath artifact configurations are in [`installer/signpath/`](../installer/signpath/).

## Source repository

<https://github.com/metasequoiaime/MSIME-Windows>, licensed under GPL-3.0. The default branch is `develop`; `main` is the release branch.

## Team roles

| Role | Members | What the role may do |
| --- | --- | --- |
| Committers and reviewers | [fanlusky](https://github.com/fanlusky), [houko](https://github.com/houko), [jsfaint](https://github.com/jsfaint), [linyanm](https://github.com/linyanm), [Neptrue-Lin](https://github.com/Neptrue-Lin) | Push branches and merge pull requests into `develop` |
| Approvers | [fanlusky](https://github.com/fanlusky), [houko](https://github.com/houko) | Promote `develop` to `main`, which is what releases a signed build; approve signing requests |

Approvers are the members of the `@metasequoiaime/maintainers` team, which owns the release-critical paths in [`.github/CODEOWNERS`](../.github/CODEOWNERS): the workflows, `installer/`, `scripts/`, `product-lock.json` and `version.txt`. The organization account `metasequoiaime-dev` is used only by the release automation and is not a person; it holds no role above. How roles are granted is described in the organization's [GOVERNANCE.md](https://github.com/metasequoiaime/.github/blob/main/GOVERNANCE.md).

Contributors without write access can still propose changes through pull requests. Their changes are reviewed and merged by a committer before they can reach a release.

## Build and release process

This is the process every SignPath-signed release goes through.

1. Every change lands in `develop` through a pull request. Direct pushes and force pushes to `develop` and `main` are blocked by a branch ruleset, and the pull request must pass the required CI checks (Windows x64 and Win32 builds, Server, GUI framework, settings page, product inputs, package contents, workflow validation and dependency review). Repository admins can bypass the ruleset only when merging a pull request, not by pushing directly. CodeQL scans the default branch daily.
2. An approver promotes `develop` to `main`. Feature branches cannot target `main` directly.
3. The [release workflow](../.github/workflows/release.yml) runs in GitHub Actions: release-please opens and tests the release pull request, merges it, creates a draft release, then builds every component from source at that commit.
4. The workflow refuses to build a commit that is not in the history of `main`, submits the payload binaries and then the installer to SignPath as two signing requests, each of which an approver must approve, attaches a [build provenance attestation](https://docs.github.com/actions/security-for-github-actions/using-artifact-attestations) to the installer, and publishes the release with the installer's SHA256.

Dictionaries are not built in CI. They are downloaded from the release pinned in [`product-lock.json`](../product-lock.json) and verified against the SHA256 recorded there. See [product-release.md](product-release.md) for the full release design.

## Privacy policy

Some features of the program send data over the network, and cloud candidates are on by default. What each feature sends, its default, and how to turn it off are documented in [PRIVACY.md](../PRIVACY.md).

## Reporting a problem

To report a signed binary that you believe was not built by this process, or any other security issue, see the organization's [security policy](https://github.com/metasequoiaime/.github/blob/main/SECURITY.md).
