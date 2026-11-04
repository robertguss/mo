#!/usr/bin/env python3
"""The between-round comparison for the Phase 4 evaluation (learnability/phase4/protocol.md).

  python3 learnability/phase4/compare.py K1 N1 K2 N2
        the baseline rate K1/N1, the final rate K2/N2, the difference and its 95% Newcombe
        hybrid score interval, and the protocol's verdict: improved when the interval's
        lower bound is above zero
  python3 learnability/phase4/compare.py --power P1 P2 N
        the exact probability that the verdict is improved when the true rates are P1 and P2
        and each round has N sessions
  python3 learnability/phase4/compare.py --size P1 P2 POWER
        the fewest sessions per round whose exact power for true rates P1 and P2 is at least
        POWER, searching up to 400 sessions
  python3 learnability/phase4/compare.py --room K1 N1 N2
        whether a frozen baseline of K1/N1 leaves room for the verdict: the fewest final
        successes out of N2 that would give improved, or none

The interval is method 10 of Newcombe (1998), "Interval estimation for the difference between
independent proportions", Statistics in Medicine 17: each rate's Wilson score interval,
combined by the square-and-add rule. The verdict is one-sided at 2.5%: the two-sided 95%
interval's lower bound. Power is computed by summing the two binomial distributions over every
outcome, with no simulation. A rate is written as a fraction (0.35), a percentage (35%) or a
ratio (7/20). The exit status is 0 for any result and 2 for a usage error.
"""

import functools
import math
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
import wilson  # noqa: E402

LARGEST_SIZE = 400
score = functools.lru_cache(maxsize=None)(wilson.wilson)


def difference(k1, n1, k2, n2):
    """The difference K2/N2 - K1/N1 and its 95% Newcombe hybrid score interval."""
    p1, p2 = k1 / n1, k2 / n2
    l1, u1 = score(k1, n1)
    l2, u2 = score(k2, n2)
    d = p2 - p1
    low = d - math.sqrt((p2 - l2) ** 2 + (u1 - p1) ** 2)
    high = d + math.sqrt((u2 - p2) ** 2 + (p1 - l1) ** 2)
    return d, max(-1.0, low), min(1.0, high)


def improved(k1, n1, k2, n2):
    """The protocol's verdict: the interval's lower bound is above zero."""
    return difference(k1, n1, k2, n2)[1] > 0


def binomial(n, p):
    """The probabilities of 0..n successes in n trials with rate p."""
    return [math.comb(n, k) * p ** k * (1 - p) ** (n - k) for k in range(n + 1)]


def power(p1, p2, n):
    """The exact probability of an improved verdict with true rates p1, p2 and n per round."""
    if n <= 0:
        raise ValueError("need N > 0")
    first, second = binomial(n, p1), binomial(n, p2)
    total = 0.0
    for k1 in range(n + 1):
        least = room(k1, n, n)
        if least is not None:
            total += first[k1] * sum(second[least:])
    return total


def room(k1, n1, n2):
    """The fewest final successes out of n2 giving an improved verdict over k1/n1, or None."""
    for k2 in range(n2 + 1):
        if improved(k1, n1, k2, n2):
            return k2
    return None


def size(p1, p2, target):
    """The fewest sessions per round whose exact power reaches target, or None."""
    for n in range(1, LARGEST_SIZE + 1):
        if power(p1, p2, n) >= target:
            return n
    return None


def parse_rate(text):
    return float(wilson.parse_rate(text))


def count(text):
    if not (text.isascii() and text.isdigit()):
        raise ValueError(f"a count is a whole number, not {text}")
    return int(text)


def main(args):
    if len(args) == 4 and args[0] == "--power":
        p1, p2, n = parse_rate(args[1]), parse_rate(args[2]), count(args[3])
        print(f"{power(p1, p2, n):.4f}")
        return 0
    if len(args) == 4 and args[0] == "--size":
        p1, p2, target = parse_rate(args[1]), parse_rate(args[2]), parse_rate(args[3])
        n = size(p1, p2, target)
        print(n if n is not None else f"more than {LARGEST_SIZE}")
        return 0
    if len(args) == 4 and args[0] == "--room":
        k1, n1, n2 = count(args[1]), count(args[2]), count(args[3])
        if k1 > n1 or n1 == 0 or n2 == 0:
            raise ValueError("need 0 <= K1 <= N1, N1 > 0 and N2 > 0")
        least = room(k1, n1, n2)
        print(f"none of 0..{n2}" if least is None else f"{least}/{n2}")
        return 0
    if len(args) == 4 and not args[0].startswith("-"):
        k1, n1, k2, n2 = (count(a) for a in args)
        if k1 > n1 or k2 > n2 or n1 == 0 or n2 == 0:
            raise ValueError("need 0 <= K <= N and N > 0 for both rounds")
        d, low, high = difference(k1, n1, k2, n2)
        verdict = "improved" if low > 0 else "not shown"
        print(f"{k1}/{n1} -> {k2}/{n2}: difference {100 * d:+.1f} points, 95% Newcombe interval "
              f"[{100 * low:+.1f}, {100 * high:+.1f}]: {verdict}")
        return 0
    raise ValueError("usage: compare.py K1 N1 K2 N2 | --power P1 P2 N | --size P1 P2 POWER | "
                     "--room K1 N1 N2")


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except (ValueError, OSError, ZeroDivisionError) as error:
        print(f"compare.py: {error}", file=sys.stderr)
        sys.exit(2)
