#!/usr/bin/env python3
"""Mutants of the pool's free-list fast path (pool_class::allocate and deallocate).

usage: tools/pool_mutants.py [REPO_DIR [COMPILER_FLAGS...]]

Breaks the fast path six ways in a private copy of allocators.h, builds test/pool.cc and
test/propagation.cc against each copy and requires one of them to fail (a non-zero exit or a
hang past 60 seconds). Exits 1 if a mutant survives or a build fails. CXX picks the compiler
(default clang++); the default flags build with ASan and UBSan, which turn a double handout or a
lost link into a crash. Not part of CTest.
"""
import os, subprocess, sys, tempfile, shutil

repo = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
flags = sys.argv[2:] or ["-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"]
cxx = os.environ.get("CXX", "clang++")
header = open(os.path.join(repo, "allocators.h")).read()

FAST_ALLOC = """				if (free_head) {
					free_node* block = free_head;
					free_head = block->next;
					return block;
				}
"""
FAST_FREE = """				auto* block = static_cast<free_node*>(p);
				block->next = free_head;
				free_head = block;
"""
assert FAST_ALLOC in header and FAST_FREE in header

mutants = {
    "alloc pops the list without advancing the head": (FAST_ALLOC, FAST_ALLOC.replace("					free_head = block->next;\n", "")),
    "alloc uses the list only when the chunk is full": (FAST_ALLOC, FAST_ALLOC.replace("				if (free_head) {", "				if (free_head && bump == bump_end) {")),
    "free does not link the block": (FAST_FREE, FAST_FREE.replace("				block->next = free_head;\n", "")),
    "free does not make the block the head": (FAST_FREE, FAST_FREE.replace("				free_head = block;\n", "")),
    "free links the block to itself": (FAST_FREE, FAST_FREE.replace("				block->next = free_head;\n", "				block->next = block;\n")),
    "free puts the block below the head": (FAST_FREE, """				auto* block = static_cast<free_node*>(p);
				if (free_head) {
					block->next = free_head->next;
					free_head->next = block;
				} else {
					free_head = block;
					block->next = nullptr;
				}
"""),
}

tests = ["pool", "propagation"]
work = tempfile.mkdtemp(prefix="pool-mutants-")
bad = 0
try:
    for i, (name, (old, new)) in enumerate(mutants.items()):
        d = os.path.join(work, "m%d" % i)
        os.makedirs(d)
        open(os.path.join(d, "allocators.h"), "w").write(header.replace(old, new, 1))
        failed = []
        for t in tests:
            exe = os.path.join(d, t)
            cc = subprocess.run([cxx, "-std=c++20", "-DALLOCATORS_POOL_CHECK=1", "-pthread", "-I", d, *flags,
                                 os.path.join(repo, "test", t + ".cc"), os.path.join(repo, "allocators.cc"), "-o", exe], capture_output=True, text=True)
            if cc.returncode != 0:
                print("BUILD FAILED", name, t, cc.stderr[-400:])
                bad += 1
                continue
            try:
                r = subprocess.run([exe], capture_output=True, text=True, timeout=60)
                if r.returncode != 0:
                    failed.append(t)
            except subprocess.TimeoutExpired:
                failed.append(t + " (hangs)")
        status = "CAUGHT by " + ", ".join(failed) if failed else "SURVIVED"
        if not failed:
            bad += 1
        print("%-55s %s" % (name, status), flush=True)
finally:
    shutil.rmtree(work)
sys.exit(1 if bad else 0)
