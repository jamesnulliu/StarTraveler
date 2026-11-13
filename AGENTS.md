# AGENTS.md

Instructions for coding agents working in this repository. `README.md` describes the project,
how to build it and how the renderers work; read the parts you touch before changing them.

## Start from TODO.md

`TODO.md` is the project's list of goals: the question it answers, what is required, the
deadlines, and every item with `[x]` (in the code) or `[ ]` (to do). Read it before any task.

- Its "Order" section says what comes first. Take items in that order unless the user picks one.
- Check the code before starting an item: the list can lag behind (an item may be half done,
  or the code may have moved since it was written).
- When an item is done, tick it (`[x]`) in the same pull request, and correct its text where
  what landed differs (the option's name, the measured numbers). Work found along the way goes
  into `TODO.md` as new `[ ]` items in the section it belongs to.
- Don't drop or reword the goals themselves (Required, Demo, the dates) unless the user asks.

## Branches and pull requests: never commit to main

`main` changes only by merging a pull request on GitHub
(`github.com/jamesnulliu/StarTraveler`). Never commit to `main` and never push to it.

1. Start from an up-to-date `main`: `git switch main`, `git pull --ff-only`.
2. Make a branch for the item: `git switch -c feature/<name>`, with a short kebab-case name that
   says which item it is (`feature/camera-paths`, `feature/flip-evaluation`, `feature/gtao`). One
   TODO item per branch, or a few that cannot land apart.
3. Commit on that branch. Work that is not a TODO item (a bug fix, docs) gets a branch of its
   own as well; nothing is committed on `main`.
4. Before the pull request: it builds (`msvc` preset), the tests pass, and `README.md` and
   `TODO.md` say what the code now does.
5. Push the branch (`git push -u origin feature/<name>`) and open a pull request into `main`:
   `gh pr create --base main`, titled like a commit subject; the body says what changed, how it
   was checked (tests, renders, measured numbers) and which TODO items it ticks. Without `gh`,
   push and give the user
   `https://github.com/jamesnulliu/StarTraveler/compare/main...feature/<name>?expand=1`.
6. Don't merge the pull request yourself unless the user asks. Review fixes go on the same
   branch.

## Building and testing

```powershell
cmake --preset msvc                       # first configure installs the vcpkg dependencies
cmake --build --preset msvc-release
ctest --preset msvc-release
build\msvc\bin\Release\StarTraveler.exe   # the game
build\msvc\bin\Release\starcore-render.exe scene.yaml --hybrid ...   # offscreen renders
uv run pytest StarCore/python/tests       # after: uv venv; uv pip install -e "./StarCore[test]"
```

Adding or removing source files re-runs the configure, which can wait a long time on vcpkg's
lock. GPU tests need a GPU with ray queries; without one they are skipped.

## Conventions

- Commit subjects: `Area: what changed, as a plain sentence` (see `git log`), e.g.
  `Hybrid: half trace resolution (paths from one pixel of each 2x2)`. The body says why, and the
  numbers when something was measured.
- C++23, lines up to 100 columns, LF line endings. `.clang-format` says 80 columns but the code
  is written at 100: don't run clang-format over whole files.
- A new rendering method lands behind an option (`GpuRenderOptions`, a `starcore-render` flag, a
  control in the game), off by default until it is measured; off, it renders the same images as
  before, bit for bit (compiled out with specialization constants, as `lightCandidates` and
  `probes` are).
- Docs and comments describe StarTraveler as a project of its own; they don't mention the
  project it was rewritten from.

## Measuring

Most TODO items end in numbers (FLIP, GPU time per pass, stability), so:

- GPU times drift 10-20% between runs. Compare builds alternately, several runs each, and take
  medians; another program using the GPU invalidates the timings.
- Make sure the binary is the one you think: after reverting or swapping sources, rebuild and
  check the exe's timestamp is later than the last edit. Don't pipe a build into
  `Select-Object -First N` (the build keeps running while the next command starts the old exe).
- Check a measuring method on a case whose answer you know before trusting what it says.
