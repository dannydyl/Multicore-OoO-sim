#!/usr/bin/env python3
"""Regenerate branchsim.champsimtrace from a deterministic project2 text trace.

Usage:
    python3 make_trace.py > branchsim.trace
    proj2_to_champsim --in branchsim.trace --out branchsim.champsimtrace

The trace mixes four branch PCs with distinct, deterministic patterns so each
predictor type is exercised meaningfully:
  - 0x401000 always taken
  - 0x402000 always not-taken
  - 0x403000 alternates taken / not-taken
  - 0x404000 follows a period-5 pattern (T T T T N), repeating

Four ALU ops fill in between each branch group to keep the dyn_inst counter
ticking and to make the trace look less artificially branch-heavy.

The corresponding project2 reference numbers in branchsim.expected.txt were
captured by running:
    project2_v2.1.0_all/proj2sim -x -i branchsim.trace -b <0..3> \\
        -H 10 -P 5 -G 9 -N 7 -T 2
"""

import sys

def main():
    out = []
    pc_alu_base = 0x400000
    pc_branch = {
        "alwT":  0x401000,
        "alwN":  0x402000,
        "alt":   0x403000,
        "per5":  0x404000,
    }
    period5 = [1, 1, 1, 1, 0]
    n_iters = 1000
    dyn = 0
    for i in range(n_iters):
        for k in range(4):
            pc = pc_alu_base + k * 4
            out.append(f"{pc:x} 2 1 2 3 0 0 0 1 1 {dyn}")
            dyn += 1
        for tag, pc in pc_branch.items():
            if tag == "alwT":
                taken = 1
            elif tag == "alwN":
                taken = 0
            elif tag == "alt":
                taken = i % 2
            else:  # per5
                taken = period5[i % 5]
            target = pc + 0x10
            out.append(f"{pc:x} 6 -1 4 5 0 {taken} {target:x} 1 1 {dyn}")
            dyn += 1

    sys.stdout.write("\n".join(out) + "\n")

if __name__ == "__main__":
    main()
