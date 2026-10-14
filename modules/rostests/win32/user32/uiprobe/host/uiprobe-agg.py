#!/usr/bin/env python3
"""uiprobe-agg.py: aggregate uiprobe -raw output (UIPROBE-SAMPLE and UIPROBE lines) into one
median / p95 per metric and phase. All times are seconds on the probe's own clock.

  uiprobe-agg.py FILE [FILE...]                       one phase "all"
  uiprobe-agg.py FILE --launch S [--start-len 60] [--after-len 60]
        phases pre [0,S), start [S,S+start-len), after [S+start-len, S+start-len+after-len)
  uiprobe-agg.py A@S1 B@S2 C@S3      pool several runs, each with its own launch offset
Output: one "AGG" line per phase and metric, then a Markdown table (--md FILE writes it).
"""
import argparse
import math
import re
import sys

KV = re.compile(r"(\w+)=(\S+)")


def pct(v, p):
    v = sorted(v)
    return v[max(1, math.ceil(p * len(v) / 100.0)) - 1]


def median(v):
    v = sorted(v)
    n = len(v)
    return v[n // 2] if n & 1 else (v[n // 2 - 1] + v[n // 2]) / 2.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+", help="FILE, or FILE@LAUNCH_SECONDS to pool runs with their own launch offsets")
    ap.add_argument("--launch", type=float, help="launch offset for files without @")
    ap.add_argument("--start-len", type=float, default=60)
    ap.add_argument("--after-len", type=float, default=60)
    ap.add_argument("--md")
    ap.add_argument("--label", default="")
    a = ap.parse_args()

    def phase_of(t, launch):
        if launch is None:
            return "all"
        if t < launch:
            return "pre"
        if t < launch + a.start_len:
            return "start"
        if t < launch + a.start_len + a.after_len:
            return "after"
        return None

    samples = {}   # (phase, metric) -> [values]
    counts = {}    # (phase, metric) -> [fail, timeout]
    for spec in a.files:
        fn, _, off = spec.partition("@")
        launch = float(off) if off else a.launch
        for line in open(fn, errors="replace"):
            line = line.strip()
            if line.startswith("UIPROBE-SAMPLE "):
                d = dict(KV.findall(line))
                ph = phase_of(float(d["t"]), launch)
                if ph:
                    samples.setdefault((ph, d["metric"]), []).append(float(d["v"]))
            elif line.startswith("UIPROBE metric="):
                d = dict(KV.findall(line))
                ph = phase_of(float(d["t"]), launch)
                if ph:
                    c = counts.setdefault((ph, d["metric"]), [0, 0])
                    c[0] += int(d["fail"])
                    c[1] += int(d["timeout"])

    order = {"all": 0, "pre": 0, "start": 1, "after": 2}
    rows = []
    for ph, m in sorted(set(samples) | set(counts), key=lambda k: (order[k[0]], k[1])):
        v = samples.get((ph, m), [])
        fail, tmo = counts.get((ph, m), [0, 0])
        r = dict(phase=ph, metric=m, n=len(v), fail=fail, timeout=tmo,
                 median=median(v) if v else float("nan"), p95=pct(v, 95) if v else float("nan"),
                 max=max(v) if v else float("nan"))
        r["fps"] = 1000.0 / r["median"] if m.startswith(("bitblt", "patblt")) and v and r["median"] > 0 else None
        rows.append(r)
        print("AGG label=%s phase=%s metric=%s n=%d fail=%d timeout=%d median=%.2f p95=%.2f max=%.2f%s"
              % (a.label, ph, m, r["n"], fail, tmo, r["median"], r["p95"], r["max"],
                 " fps=%.1f" % r["fps"] if r["fps"] else ""))

    md = ["| phase | metric | n | fail | timeout | median | p95 | max |", "|---|---|---|---|---|---|---|---|"]
    for r in rows:
        med = "%.2f" % r["median"] + (" (%.0f fps)" % r["fps"] if r["fps"] else "")
        md.append("| %s | %s | %d | %d | %d | %s | %.2f | %.2f |" % (r["phase"], r["metric"], r["n"], r["fail"], r["timeout"], med, r["p95"], r["max"]))
    text = "\n".join(md) + "\n"
    if a.md:
        open(a.md, "w").write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
