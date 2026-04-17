# PES-VCS: A Version Control System from Scratch

**Student:** Akulkrishna M S  
**SRN:** PES1UG24CS554  
**Platform:** Ubuntu 22.04

---

## Table of Contents

1. [Getting Started](#getting-started)
2. [Phase 1: Object Store](#phase-1-object-store)
3. [Phase 2: Tree Objects](#phase-2-tree-objects)
4. [Phase 3: Staging Area (Index)](#phase-3-staging-area-index)
5. [Phase 4: Commits and History](#phase-4-commits-and-history)
6. [Phase 5 & 6: Analysis Questions](#phase-5--6-analysis-questions)
7. [Submission Checklist](#submission-checklist)

---

## Getting Started

### Prerequisites

```bash
sudo apt update && sudo apt install -y gcc build-essential libssl-dev
```

### Building

```bash
make          # Build the pes binary
make all      # Build pes + test binaries
make clean    # Remove all build artifacts
```

### Author Configuration

```bash
export PES_AUTHOR="Akulkrishna M S <PES1UG24CS554>"
```

If unset, defaults to `"PES User <pes@localhost>"`.

---

## Phase 1: Object Store

**Concepts Covered:** SHA-256 content addressing, sharded directory layout, atomic writes, deduplication

### Implementation Summary

**`object_write`** — Builds the object by prepending a `"type size\0"` header to the raw data, computes the SHA-256 hash of the full buffer, checks for deduplication via `object_exists`, then writes atomically using a temp file + `rename`.

**`object_read`** — Reads the file at the sharded path, verifies integrity by re-hashing, then splits on the null byte to separate the header from the data payload and returns typed content.

Key design decisions:
- Objects are stored at `.pes/objects/XX/YYYY...` where `XX` is the first two hex chars (sharding prevents filesystem slowdown from too many files in one directory).
- `mkstemp` + `fsync` + `rename` ensures writes are atomic — a crash mid-write never leaves a corrupt object.
- Deduplication: if the hash already exists on disk, the write is skipped entirely.

### Screenshot 1A — Phase 1 Tests Passing

![Phase 1 test output](1A.png)

`make test_objects` compiles and runs the test binary. All three tests (blob storage, deduplication, integrity check) pass.

### Screenshot 1B — Sharded Object Store

![find .pes/objects output](1B.png)

Three objects are visible after the test run, stored under their respective two-character shard directories (e.g., `.pes/objects/d5/...`, `.pes/objects/25/...`, `.pes/objects/2a/...`).

---

## Phase 2: Tree Objects

**Concepts Covered:** Binary tree serialization, recursive directory structures, deterministic hashing

### Implementation Summary

**`tree_serialize`** — Sorts entries alphabetically (required for deterministic hashes), then writes each entry as `"<octal-mode> <name>\0<32-byte-binary-hash>"`.

**`tree_parse`** — Parses the binary format safely using `memchr` to locate delimiters, reading mode, null-terminated name, and raw 32-byte hash for each entry.

**`tree_from_index`** — Loads the index, then calls the recursive helper `write_tree_level`. The helper groups entries by their directory prefix at each depth level, recursing into subdirectories and building tree objects bottom-up before assembling the root tree.

### Screenshot 2A — Phase 2 Tests Passing

![Phase 2 test output](2A.png)

Both tests pass: `tree serialize/parse roundtrip` and `tree deterministic serialization`. The serialized tree is 139 bytes.

### Screenshot 2B — Raw Tree Object (xxd)

![xxd of raw tree object](2B.png)

The hex dump shows the binary tree object stored on disk. The octal mode (`100644`) and filenames (`file1.txt`, `file2.txt`) are visible in the ASCII column, followed by the raw 32-byte hashes.

---

## Phase 3: Staging Area (Index)

**Concepts Covered:** File format design, atomic writes, change detection via metadata

### Implementation Summary

**`index_load`** — Opens `.pes/index` and parses each line with the format `<mode> <hash-hex> <mtime> <size> <path>`. Returns success (empty index) if the file doesn't exist yet.

**`index_save`** — Makes a sorted copy of the entries (alphabetical by path), writes to a temp file with `fsync`, then atomically renames it to `.pes/index`.

**`index_add`** — Reads the file, calls `object_write` to store it as a blob, then updates (or inserts) the index entry with the new hash, mtime, and size. Sets mode `100755` for executables, `100644` otherwise.

### Screenshot 3A — `pes status` Output

![pes status output](3A.png)

After `./pes init` and `./pes add file1.txt file2.txt`, status correctly shows both files as staged, nothing unstaged, and all other project files as untracked.

### Screenshot 3B — `.pes/index` Content

![cat .pes/index](3B.png)

The index file shows two entries in plain-text format: mode, SHA-256 hex hash, mtime, size, and path — one per line, sorted alphabetically.

---

## Phase 4: Commits and History

**Concepts Covered:** Linked structures on disk, reference files, atomic pointer updates

### Implementation Summary

**`commit_create`** —

1. **Tree:** Calls `tree_from_index` to build and write the staged snapshot, obtaining its root tree hash.
2. **Parent:** Calls `head_read` to get the current HEAD commit hash. If it succeeds, sets `has_parent = 1`; for the first commit, HEAD doesn't exist so `has_parent = 0`.
3. **Metadata:** Fills in author (from `pes_author()`), Unix timestamp (from `time(NULL)`), and message.
4. **Serialize & Store:** Calls `commit_serialize` to produce the text buffer, then `object_write(OBJ_COMMIT, ...)` to persist it.
5. **Update HEAD:** Calls `head_update` to atomically move the branch ref to the new commit hash.

### Screenshot 4A — `pes log` with Three Commits

![pes log output](4A.png)

Three commits are shown in reverse chronological order, each with full hash, author, Unix timestamp, and message. The parent chain is correctly maintained.

### Screenshot 4B — Object Store After Three Commits

![find .pes -type f sorted](4B.png)

After three commits, the object store contains 10 objects (blobs, trees, and commits) plus the index and HEAD ref. Object counts grow as expected: each commit adds a commit object, a tree object, and blob objects only for changed files.

### Screenshot 4C — Reference Chain

![cat .pes/refs/heads/main and HEAD](4C.png)

`cat .pes/refs/heads/main` shows the hash of the latest commit. `cat .pes/HEAD` shows `ref: refs/heads/main`, confirming the symbolic reference chain is intact.

### Screenshot — Integration Test

![make test-integration full output](final1.png)
![make test-integration continued](final2.png)

`make test-integration` runs `test_sequence.sh` end-to-end. All phases pass: repository initialization, staging, first commit, second commit, third commit, full history traversal, reference chain verification, and object store count.

---

## Phase 5 & 6: Analysis Questions

### Phase 5: Branching and Checkout

**Q5.1: How would you implement `pes checkout <branch>`?**

A branch is just a file at `.pes/refs/heads/<branch>` containing a commit hash. To implement checkout:

1. **Read the target branch file** to get the target commit hash (or fail if it doesn't exist).
2. **Walk the target commit's tree** to enumerate all files and their blob hashes.
3. **Update the working directory:** for each file in the target tree, read the blob from the object store and write it to disk. Delete any tracked files not present in the target tree.
4. **Rebuild the index** to reflect the target tree's contents (paths, hashes, modes, mtimes, sizes).
5. **Update HEAD** to `ref: refs/heads/<branch>` (the new branch name).

What makes this complex: the working directory update must be done carefully to avoid data loss. If a file exists on disk but is untracked, checkout should not delete it. If a tracked file has been modified but not staged, the new content would be overwritten — this must be detected and refused (see Q5.2). Additionally, the operation should be transactional: a partial checkout (crash midway) should not leave the repository in a broken state, which typically requires staging files before overwriting them.

---

**Q5.2: How do you detect a "dirty working directory" conflict before switching branches?**

Detection requires comparing three versions of every tracked file:

1. **Index entry:** the staged blob hash for a file.
2. **HEAD tree entry:** the blob hash recorded in the current branch's latest commit tree.
3. **Working directory:** the file's current on-disk mtime and size (from `stat`).

The conflict arises when switching branches would overwrite an on-disk change that hasn't been committed. The check is:

- For each file in the **target branch's tree** that differs from the **current branch's tree** (i.e., the blob hash changes between commits):
  - Check whether the working directory version matches the current HEAD version by comparing mtime/size (fast path) or re-hashing (slow path).
  - If the working directory diverges from HEAD, the user has local modifications. Since checkout would overwrite that file with a different version from the target branch, refuse and print an error.

Files that are the same between both branches don't need checking — checkout won't touch them. Untracked files are also left alone (checkout only updates tracked files). This approach uses only the index and object store — no diffs are computed, just hash comparisons.

---

**Q5.3: What happens if you commit in detached HEAD state? How do you recover?**

In detached HEAD mode, `HEAD` contains a raw commit hash instead of `ref: refs/heads/<branch>`. Commits still work: `head_read` parses the hash directly, `commit_create` writes the new commit and calls `head_update`, which writes the new hash back to `HEAD`. The commits exist as a valid chain in the object store.

The problem: no branch ref points to these commits. If you checkout another branch, `HEAD` is overwritten to point to that branch. The detached commits are now unreachable — no branch, tag, or ref leads to them.

**Recovery options:**

1. **If you remember the hash:** `git branch recovery-branch <hash>` creates a new branch pointing at the last detached commit, restoring the whole chain through parent pointers.
2. **Via reflog (if implemented):** Git logs every HEAD movement in `.git/logs/HEAD`. You can find the lost commit hash in the reflog and create a branch from it.
3. **Via `git fsck --lost-found`:** This scans the entire object store for commits unreachable from any ref and writes them to `.git/lost-found/`. The commits persist until garbage collection runs (usually after 30 days by default in Git).

---

### Phase 6: Garbage Collection

**Q6.1: Algorithm to find and delete unreachable objects**

This is a **mark-and-sweep** garbage collection:

**Mark phase:**
1. Collect the set of all reachable root hashes: every ref in `.pes/refs/` (branches, tags) plus HEAD if it contains a raw commit hash.
2. Use a **hash set** (e.g., a hash table of ObjectID values) to track visited objects.
3. For each root commit: walk the commit chain (following `parent` pointers). For each commit, add its tree hash to a work queue. For each tree, recursively add all blob and sub-tree hashes. Mark every visited hash in the set.

**Sweep phase:**
4. Walk the entire `.pes/objects/` directory (all shard folders).
5. For each object file, convert the path back to its hash. If the hash is **not** in the reachable set, delete the file.

**Data structure:** A hash set (or sorted array with binary search) of `ObjectID` values (32-byte hashes). This gives O(1) or O(log n) lookup during the sweep.

**Estimation for 100,000 commits, 50 branches:**
- Each commit references 1 tree; each tree averages ~10 entries (mix of blobs and sub-trees). Assuming a medium-size repo with ~200,000 blobs and ~20,000 trees total, the mark phase visits roughly 320,000 objects. The sweep phase scans all objects on disk — potentially millions if there is significant garbage accumulated.

---

**Q6.2: Race condition between GC and a concurrent commit**

**The race:**

1. A commit operation calls `object_write` for a new blob. The blob is hashed and found not to exist yet (`object_exists` returns false). The write is about to begin.
2. GC runs its mark phase **right now**. Since the blob hasn't been written yet, it's not in the object store and is not reachable from any ref. The GC's reachable set does not include this hash.
3. The commit operation writes the blob to disk.
4. GC runs its sweep phase and deletes the blob — it wasn't in the reachable set.
5. The commit continues: it writes a tree that references the now-deleted blob, then writes the commit object and updates HEAD.
6. The repository is now corrupt: HEAD points to a commit whose tree references a missing object.

**How Git avoids this:**

Git uses a **grace period**: objects written to disk are considered safe for a configurable window (default 2 weeks) regardless of reachability. GC only deletes objects older than this window. Since a real commit takes milliseconds, freshly written objects are always protected.

Additionally, Git's GC checks object `mtime` during the sweep — any object created within the grace period is kept. This means even if a blob was written but its parent commit hasn't been written yet, the blob survives the current GC run and will be reachable once the commit is eventually written. For concurrent safety in multi-process environments, Git also uses lock files on refs to prevent simultaneous mutations.

---

## Submission Checklist

### Screenshots

| Phase | ID  | Description | Status |
|-------|-----|-------------|--------|
| 1 | 1A | `./test_objects` — all tests passing | ✅ |
| 1 | 1B | `find .pes/objects -type f` — sharded structure | ✅ |
| 2 | 2A | `./test_tree` — all tests passing | ✅ |
| 2 | 2B | `xxd` of raw tree object (first 20 lines) | ✅ |
| 3 | 3A | `pes init` → `pes add` → `pes status` sequence | ✅ |
| 3 | 3B | `cat .pes/index` — text-format index | ✅ |
| 4 | 4A | `pes log` — three commits with hashes and messages | ✅ |
| 4 | 4B | `find .pes -type f \| sort` — object store growth | ✅ |
| 4 | 4C | `cat .pes/refs/heads/main` and `cat .pes/HEAD` | ✅ |
| Final | — | `make test-integration` — all tests completed | ✅ |

### Code Files

| File | Description | Status |
|------|-------------|--------|
| `object.c` | `object_write`, `object_read` | ✅ |
| `tree.c` | `tree_from_index`, `write_tree_level` | ✅ |
| `index.c` | `index_load`, `index_save`, `index_add` | ✅ |
| `commit.c` | `commit_create` | ✅ |

### Analysis Questions

| Section | Questions | Status |
|---------|-----------|--------|
| Branching | Q5.1, Q5.2, Q5.3 | ✅ |
| Garbage Collection | Q6.1, Q6.2 | ✅ |

---

## Further Reading

- [Git Internals (Pro Git)](https://git-scm.com/book/en/v2/Git-Internals-Plumbing-and-Porcelain)
- [Git from the inside out](https://codewords.recurse.com/issues/two/git-from-the-inside-out)
- [The Git Parable](https://tom.preston-werner.com/2009/05/19/the-git-parable.html)
