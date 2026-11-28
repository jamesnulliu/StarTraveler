# Denoiser branch review / 降噪分支审查

Target: `jamesnulliu/StarTraveler`, branch `feature/eval-denoiser`.
Prepared from upstream commit `dde6e1bb85e6afa33fa2d1241d55b947ad46b8c3`.
This delivery is for branch review. No PR or merge into `main` is requested.

## Start here

- [Final denoiser report](docs/final-denoiser-report.md)
- [New fixed-input Quality results](docs/quality-fixed-exposure-results.md)
- [R2 presentation, figures and speaker script](docs/final-materials/README.md)
- [Demo profiles 1–7 and F2 status](docs/demo-presets.md)
- [Remaining issues](OPEN_ISSUES.md)

The branch contains source, measurement/rehearsal tools, documentation, editable
PPTX and selected evidence from the R2 source handoff. The selected `renders/`
files are intentionally versioned; the general ignore rule prevents accidentally
adding new bulk captures. This is not the approximately 25 GB raw-frame archive.
Some preserved audit paths refer to the original machine and excluded raw data.

The 182 MB runnable handoff ZIP, EXE/DLL files, flight MP4, build/dependency caches
and user configuration are not committed. The separately prepared full ZIP can
be shared with the team or attached to a future Release. No Release is created
by this branch upload. The earlier ZIP receiving instructions describe the local
archives, rather than files automatically available from this Git clone.

## Validation and remaining limits

Upload preparation reran the measurement utility tests: 15 passed. The staged
changes pass Git whitespace checks, and 178 selected evidence/material files
match their working-copy bytes exactly. No staged file exceeds 100 MiB.

The final 27 Quality runs, three calibration sequences and three exact endpoint
repeats pass unchanged limits. Independent verification checks 264 hashes,
2493 timestamp and state rows, 54 directly recomputed FLIP scores (zero difference)
and nine summary rows. The separate production settling study and R2 materials
retain their own verification records. The game profile tests and seven automatic
window checks are documented in the demo guide.

Ordinary DLSS explicitly uses K in the formal comparison. The known
Performance-M color regression remains a failure, not a relaxed threshold or
full GPU-suite pass. Team direct-light/shared-tool integration, a clean-machine
build and manual presentation-machine checks remain open. The local source
snapshot and its earlier ZIPs have no Git history; this branch preserves the
actual upstream history and adds the reviewed snapshot as a new commit.
