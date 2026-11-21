# Working with agents: test-driven, three roles, one orchestrator

How a piece of this project is built when the work is shared among automated agents. It is the
test-driven rule of `CONTRIBUTING.md` §4 (tests first, the minimum implementation, then refactor)
made into four roles, so that no one marks their own work. Every phase from 2a on ran this way:
the plans in `docs/superpowers/plans/` are its record (each task: a red commit, a green commit, a
review, a fix round, a checkpoint).

The rules of `CONTRIBUTING.md` bind every agent exactly as they bind a person: the first-use C++
comments (§1), the model explained in the code (§2), the compiler defects and their workarounds
(§3), `Pattern:` comments (§4), the web rules (§5). A brief says which of them the task touches.

## 1. The four roles

| Role | Does | Never does |
|---|---|---|
| **Orchestrator** | Designs, writes the spec and the plan, cuts the plan into tasks, writes each task's brief, runs the checkpoints, reads every result, decides what a review finding is worth, talks to the user | Writes implementation code or tests; edits a result it disagrees with (it sends it back with the reason) |
| **Test-writer** | Turns the task's properties into failing tests, named after the property each proves; commits them as the **red commit** | Reads or writes the implementation; changes anything outside the task's test files |
| **Implementer** | Makes the red tests pass with the minimum code; refactors with the tests green; commits the **green commit** | Edits, deletes, skips or weakens a test; adds behaviour no test asks for |
| **Reviewer** | Reads the finished diff with fresh eyes and tries to break it; reports findings ranked by severity | Fixes what it finds; sees the implementer's reasoning, only the diff and the spec |

The separation is the point. A test written by the author of the code proves what the author
believed. A test written from the spec by someone who has not seen the code proves what the spec
says. A reviewer who watched the code being written shares its blind spots, so the reviewer starts
from nothing but the diff.

## 2. The loop of one task

```
orchestrator: brief ─> test-writer ─> RED commit ─> (orchestrator checks the red)
              ─> implementer ─> GREEN commit ─> (orchestrator checks the checks)
              ─> reviewer ─> findings ─> orchestrator triages
              ─> implementer fix round (tests first for each finding) ─> FIX commit
              ─> checkpoint with the user
```

1. **Brief** (§3). One task, a few hours of work at most. A task that cannot be briefed in a page
   is two tasks.
2. **Red.** The test-writer works from the brief and the spec, never from the implementation. Its
   tests must fail *for the stated reason*: a missing function or an assertion that does not hold,
   not a typo. Where the API does not exist yet it writes the smallest stub that compiles (a
   function that throws, returns nothing, or a type with no members) so that the failure is an
   assertion, and says so in the commit. The orchestrator runs the tests and reads the failures
   before the implementer starts: a red that fails for the wrong reason is sent back.
3. **Green.** The implementer sees the failing tests, the brief and the spec. It writes the
   minimum that passes, then refactors with the suite green. If it believes a test is wrong, it
   **stops and reports** which test and why; it does not change it. The orchestrator decides, and
   if the test was wrong, the test-writer fixes it in a new commit.
4. **Review.** The reviewer gets the diff between the task's base and the green commit, the
   brief, and the spec sections the brief names. It works in its own worktree and its own build
   directory. It is told to be adversarial: look for the input the tests do not cover, the
   assumption the model makes silently, the module-boundary and link problems of
   `CONTRIBUTING.md` §3, a comment that explains the wrong thing. Its report is a list, each
   finding with a severity (blocker, important, minor), the file and line, and the concrete failing
   scenario. A finding with no scenario is a remark, not a finding.
5. **Fix round.** The orchestrator triages: accept, decline with a reason, or defer. For each
   accepted finding the test-writer first adds a test that fails because of it, then the
   implementer fixes it. The round's notes (what was found, what was done, what was declined and
   why) go into the plan under the task, as the earlier phases' "Review of Task N" entries do.
6. **Checkpoint.** The orchestrator reports to the user: what was done, the numbers (tests
   added, whole-suite count), what the review found, what the next step is and how large. It then
   waits for the go-ahead. Nothing is pushed without the user's say-so.

A small, mechanical change (a rename, a doc fix) skips the review and the red commit; the
orchestrator says so in the checkpoint. A new behaviour never does.

## 3. The brief

An agent starts with nothing but its brief, so the brief carries everything. The orchestrator
writes it from the plan's task section, and does not say "see above".

```
Task:      one sentence, then the plan's task text
Role:      test-writer | implementer | reviewer
Branch:    the branch, the base commit, the commit identity to use
Read:      the spec sections and the files to read, by path and line range
Touch:     the files it may change (an exhaustive list); everything else is off limits
Properties the work must establish (test-writer) / the red tests to satisfy (implementer)
Rules:     the CONTRIBUTING.md sections that bind this task (first-use comments, Pattern:
           comments, the model explained, test names without brackets, ...)
Checks:    the exact commands to run before committing, and their expected result
Build dir: its own (`build/gcc14-<task>`), never a shared one
Report:    what to send back (commit hashes, test counts, deviations from the plan, doubts)
Stop and ask if: the plan is wrong, a test looks wrong, the task needs a file outside Touch
```

Rules for writing one:

- **Numbers are derived, not remembered.** A test's expected value comes with its derivation in
  the brief (`3 medium web at $140 → $420`), so the test-writer can check it, and a reviewer can
  tell a wrong test from a wrong implementation.
- **Say what to do on a surprise.** The default is "stop and report", never "work around it".
- **No hidden context.** An agent does not know what was decided in the conversation. A decision
  that matters (a flag's refusal message, a column order) is written into the brief.
- **Match effort to the task.** A well-specified task (a parser, a view over a pure function) goes
  to a faster, cheaper agent; a task whose design is still partly open (a loader with line
  tracking, a verdict that ranks bottlenecks) goes to the most capable one. The orchestrator
  decides per task and says so in the plan.

## 4. Parallel work

Agents may work at the same time when the files they touch do not overlap.

- Each has **its own build directory** and, for a reviewer or a risky change, **its own git
  worktree**. Two builds in one directory corrupt each other.
- Each **stages files by name** (`git add path/to/file`), never `git add -A`, so a neighbour's
  half-written file is not committed under its name.
- Tasks that change the same file, or one that needs the other's interface, run in sequence.
- The orchestrator merges nothing blind: after parallel tasks it builds and runs the whole suite
  on the combined branch before the next step.

## 5. What the orchestrator checks, and does not trust

An agent's report says what it intended. The orchestrator verifies what happened:

- the commit exists, on the right branch, with the expected files and no others (`git show --stat`);
- the test count went up by the number the brief promised, and no test was removed or weakened
  (`git diff` of the test files between the red and the green commit must be empty);
- the checks pass when the orchestrator runs them, not only in the report;
- the code follows the rules the brief named: spot-check the first-use comments, a `Pattern:`
  comment, the model's header comment;
- for the compiler defects of `CONTRIBUTING.md` §3: Release is built after a task that adds a type
  crossing a module boundary.

## 6. Commits

- The red commit contains tests only; the green commit contains the implementation and no test
  changes; a fix round is one commit per finding or per small group.
- The message says **why**; the diff shows what. A red commit says which property it pins; a green
  commit says what design choice the minimum took.
- Commits use the project owner's identity. No agent signs a commit or adds a trailer, a
  generated-by line or a name of a tool or model to a commit, a file or a pull request text.

## 7. When to stop and ask the user

Before every `git push`; before anything that rewrites history, deletes a branch or a file the
user did not mention, or touches the remote; when the spec and the plan disagree and the choice
changes what is built; when a task turns out to need a decision the spec leaves open (a price, a
default, a user-visible message). Everything else the orchestrator decides, and reports at the
checkpoint.

## 8. Definition of done

A task is done when: the red and green commits exist and the suite is green on Debug and Release
(and the sanitizer and Clang presets before a push); the review's accepted findings are fixed with
tests; the plan's task entry has its notes; the checkpoint has been reported. A phase is done when
the whole branch has had one more review against the spec, CI is green on the pull request, and
the user has merged it.
