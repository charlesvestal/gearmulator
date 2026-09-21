# This fork, and who depends on it

`charlesvestal/gearmulator` carries the Move and iOS work on top of
[dsp56300/gearmulator](https://github.com/dsp56300/gearmulator). Five repositories
build from it, and **each one pins a different commit by SHA** — a submodule
gitlink names a commit, never a branch. A branch here has exactly one job: keep a
pinned commit reachable so GitHub does not garbage-collect it.

Delete or force-move the wrong branch and a consumer's `git submodule update`
starts failing, with nothing in that repo to say why. Hence this file.

## Who pins what

| Consumer repo | Its branch | gearmulator pin | Kept alive by | dsp56300 gitlink | Kept alive by (in the dsp56300 fork) |
|---|---|---|---|---|---|
| `gearmulator-ios` | `main`, `ios-bench` | `ef9d658b` | `integration/ios-move` (head) | `2155545e` | **`ios-move-on-sep21` — only ref** |
| `schwung-jp8000` | `ios-auv3` | `ef72c4f0` | `integration/ios-move` (ancestor) | `657f4004` | **`integration/ios-move` — only ref** |
| `je8086-ios` | `ios-auv3` | `5a9512f2` | `integration/ios-move` (ancestor) | `317b84d3` | `ios-asmjit-bump`, `interp-dispatch-port` |
| `schwung-vavra` | `main` | `d7c692c1` | `dsp56k-bench` (head), also an ancestor of `integration/ios-move` | `317b84d3` | as above |
| `schwung-virus` | `fix/worker-threads-not-realtime` | `c445e838` | **`legacy/schwung-move-pr` (head) — only ref** | `f8a44801`, at the pre-restructure path `source/dsp56300` | upstream's own history — safe |

Recheck the table with:

```bash
git rev-parse <pin>:source/cpu/dsp56300      # the dsp56300 commit that pin needs
git branch -r --contains <pin>               # what is keeping it reachable
```

## Rules that follow from the table

- **Never delete a branch without running `git branch -r --contains` on every pin
  above.** Three of them are held by a single ref.
- **`integration/ios-move` is the integration branch.** New Move/iOS work lands
  here, and this is the branch `gearmulator-ios/.gitmodules` declares.
- **A consumer moves at its own pace** by bumping its own gitlink. Nothing here
  pushes a change onto them, which is why old pins must stay reachable for as long
  as any repo still names them.
- **Platform-specific work is guarded, not branched** — `#if JUCE_IOS` in C++,
  `if(IOS)` in CMake. About two thirds of what this fork carries is
  platform-neutral engine work (interpreter opcode cache, multi-threaded DSP,
  input-backlog recovery, FTZ on aarch64), shared by Move and iOS alike. That is
  the reason there is one fork and not two: splitting would mean merging upstream
  twice and cherry-picking the shared majority in both directions.

## Merging upstream

Expect the dsp56300 submodule to be the hard part. Upstream rewrote that history
in September 2026, so our branch there has **no merge base** against it and `git
merge` refuses outright. What works is re-applying our change set as a patch:

```bash
# in source/cpu/dsp56300 — find the upstream commit our work was based on,
# by tree distance, since the real base commit no longer exists upstream
for c in $(git log --format=%h --since=<date> --until=<date> <upstream-tip>); do
  echo "$(git diff --numstat $c <our-root> -- source | awk '{a+=$1; d+=$2} END {print a+d}') $c"
done | sort -n | head
git diff <closest> <our-root> -- source ':!source/asmjit' > ours.patch
git apply -3 ours.patch
```

Then run `dsp56kTestRunner` before trusting the result — the September 2026 merge
looked clean and still regressed `UnitTests::blockOnExtensionWord`, which was the
only signal that anything was wrong.

Also watch `source/asmjit`: our pointer is a fork carrying
`virtmem: include OSCacheControl.h on every Apple target`. Upstream's includes it
on macOS only, and without it an iOS build has no `sys_icache_invalidate`.
