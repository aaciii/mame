REPOSITORY COPY — DEVELOPMENT TOOLING
====================================

These helpers are preserved from the local MAME 0.289 video-preview
development and release-preparation workflow.

Their repository location is tools/video-preview-release/.
The instructions below describe the original external-folder layout;
the C:/mame-dev paths are examples, not required installation locations.

The current feature commit contains 16 source/build files. The older
"15 feature files" reference below is historical; inspect the script's
actual file list before relying on it.

The full modified executable has been built and tested locally.
A stripped copy resolved the observed startup delay.
Dependency staging, clean-Windows testing, and public-release licensing
checks have not yet been completed.

These helpers have not yet been revalidated for running from inside
this fork. In particular, snapshot validation may need adjustment to
allow the added tooling files. This commit preserves the tools; it
does not certify that the packaging workflow is release-ready.

Original instructions follow.
--------------------------------------------------------------------

Video Preview release-preparation tools
=======================================

This is a development/release-preparation toolkit, not the installable overlay.
It captures YOUR working source rather than reconstructing earlier installers.
The full Windows executable must be built on your computer.

Extract as C:\mame-dev\video-release-tools.
Close MAME and do not edit source while taking the snapshot/building.

1. SNAPSHOT AND CONSOLIDATE (UCRT64)
cd /c/mame-dev/mame0289
python /c/mame-dev/video-release-tools/prepare_release.py \
  --source . \
  --output /c/mame-dev/video-release-0289 \
  --source-archive

Requires a fresh output directory outside the checkout. Rejects unexpected
tracked changes and unrelated untracked source. Captures the 15 feature files,
including previously untracked new source, using a TEMPORARY Git index. Your
real staging index, HEAD and branch are unchanged; Git objects may be created.
Historical scripts, media, INI files, backups and build output are not in patch.
The generated patch is applied against the base in a temporary index and its
resulting tree ID is checked against the snapshot tree ID.
The full source archive may take time and substantial disk space; omit
--source-archive for patch/manifest only if space is limited.
External FFmpeg/runtime-library corresponding source is NOT in this archive.
Existing videotest.exe/mame.exe in the development source root are preserved
as baseline-* executables in the release-work directory, if present.

2. FULL BUILD
Inspect useroptions.mak, if present, for any SOURCES/SOURCEFILTER restrictions.
Then run:
set -o pipefail
env -u SOURCES -u SOURCEFILTER MAME_VIDEO_SNAPS=1 make -j2 \
  TARGET=mame SUBTARGET=mame BUILDDIR=build-release REGENIE=1 \
  2>&1 | tee /c/mame-dev/video-release-0289/full-build.log

Do not pass the old videotest subtarget or Pac-Man SOURCES filter. This is a full
build, with separate intermediate output. Main output is mame.exe in the source
root. Any preexisting mame.exe there will be replaced; the snapshot preserves it.
Your separate playing installation is not modified.
Do not disable PortAudio. Use your working UCRT64 and FFmpeg installation.
Do not update toolchain packages midway between snapshot and packaging.

3. VERIFY SOURCE DID NOT CHANGE
python /c/mame-dev/video-release-tools/verify_source.py \
  --source /c/mame-dev/mame0289 \
  --manifest /c/mame-dev/video-release-0289/source-manifest.json

4. RUN FULL BUILD FROM UCRT64 FIRST
cd /c/mame-dev/mame0289
./mame.exe
Confirm a full game list, Video/Images/Info, settings, video/audio/fallbacks,
switching games/tabs and game launches. Launch a few non-Pac-Man systems too.
This is still a developer environment, not a dependency-completeness test.

5. DLL AUDIT/STAGING (ONLY AFTER SUCCESSFUL FULL BUILD)
python /c/mame-dev/video-release-tools/stage_overlay.py \
  --exe /c/mame-dev/mame0289/mame.exe \
  --runtime-bin /ucrt64/bin \
  --output /c/mame-dev/video-release-0289/overlay-candidate

Review any missing imports. If all static imports resolve, run the same command
with --stage to copy mame.exe and the recursive non-system imported DLLs.
No existing MAME installation is touched. No configuration or media is copied.
No final ZIP is made. The candidate is explicitly marked UNPUBLISHED.

The DLL tool uses objdump, inspects recursive PE imports, resolves from the EXE
directory and supplied runtime directory, and identifies files in System32.
It rejects MSYS/Cygwin runtime imports and unresolved DLLs. It is not a proof
of runtime completeness: dynamically loaded libraries, optional plugins,
System32 dependencies installed only on the development PC, and Windows-version
differences require clean-machine tests. This is Windows x64-specific packaging;
source portability remains a separate validation target.
The candidate contains no replacement ui.ini/mame.ini and no ROMs/videos.

6. REMAINING RELEASE GATES
- Record/audit exact bundled package versions, FFmpeg build configuration,
  licenses and corresponding source for every redistributed dependency.
- Identify the build clearly as an unofficial modified distribution.
- Test on clean Windows without MSYS2/FFmpeg, over stock MAME 0.289 x64.
- Keep Windows system directories intact; never test by deleting system DLLs.
- Verify installation via extraction, INI preservation, and rollback by restoring
  the original executable and removing only overlay-added files.
- Final notices/licenses/source-access instructions, file manifest and hashes.
- Final ZIP with mame.exe at its root, not an extra enclosing directory.

The consolidated source patch is the replacement for the old incremental
installers; it must be tested in a fresh stock checkout as an additional gate.
A source snapshot is not a claim of reproducible binaries. Exact dependency
sources/build inputs still need preservation. Review local package/build reports
for information you do not want to publish before redistribution.

TEST SCOPE
prepare/verify scripts were tested in a small synthetic Git repository,
including patch round-trip, archive generation and real-index preservation.
Stage tool syntax was checked; Windows PE/DLL staging and full MAME builds
are not executed in the assistant environment.
