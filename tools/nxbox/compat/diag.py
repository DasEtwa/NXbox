"""Parse native diagnostics and score observed boot/presentation evidence."""

import math
import re

FIELDS = re.compile(r"\b([a-z_]+)=(-?\d+(?:\.\d+)?)")


def parse_diag(text):
    result = {
        "boot_ok": False,
        "first_frame": False,
        "presents": [],
        "game_fail": [],
        "memory_peak_bytes": None,
    }
    elapsed = 0.0
    for line in text.splitlines():
        fields = {key: float(value) for key, value in FIELDS.findall(line)}
        if re.search(r"\bGAME_RUNNING\b", line):
            result["boot_ok"] = True
        if re.search(r"\bGAME_FIRST_FRAME\b", line):
            result["first_frame"] = True
        if re.search(r"\bGAME_(?:FAIL|LOAD_FAILED|MISSING)\b", line):
            result["game_fail"].append(line)
        if re.search(r"\bGAME_PRESENT\b", line):
            frames, seconds = fields.get("frames", -1), fields.get("seconds", 0)
            if frames >= 0 and seconds > 0 and all(math.isfinite(v) for v in fields.values()):
                sample = {
                    "window_start_seconds": elapsed,
                    "window_end_seconds": elapsed + seconds,
                    "frames": int(frames),
                    "seconds": seconds,
                    "presents_per_5s": frames * 5 / seconds,
                }
                for key in ("tick_ms", "fps", "memory", "memory_limit"):
                    if key in fields:
                        sample[key] = fields[key]
                result["presents"].append(sample)
                elapsed += seconds
            memory = fields.get("memory")
            if memory is not None and math.isfinite(memory) and memory >= 0:
                result["memory_peak_bytes"] = max(result["memory_peak_bytes"] or 0, int(memory))
        match = re.search(r"\bMEM\s+.*?commit=(\d+)\s*MiB", line)
        if match:
            result["memory_peak_bytes"] = max(result["memory_peak_bytes"] or 0, int(match[1]) << 20)
    return result


def score_game(result):
    """0..100 evidence score, not a claim of visual correctness or playability."""
    if result.get("runner_error"):
        return 0
    score = 20 if result.get("boot_ok") else 0
    score += 20 if result.get("first_frame") else 0
    samples = result.get("presents", [])
    if samples:
        # Reward continued presentation, including zero-frame/stalled intervals.
        active = sum(sample["frames"] > 0 for sample in samples) / len(samples)
        fps = sum(sample["frames"] for sample in samples) / sum(s["seconds"] for s in samples)
        score += round(30 * active) + round(20 * min(fps / 30, 1))
    score += 10 if result.get("frames") else 0
    if result.get("game_fail") or result.get("crash_file"):
        score = min(score, 20)
    return score
