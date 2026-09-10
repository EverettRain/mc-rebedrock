#!/usr/bin/env python3
"""Summarise complete PerfTrace spans without cross-thread summation."""

import argparse
import json
import math
import time
from collections import Counter, defaultdict


def percentile(values, fraction):
    if not values:
        return None
    values = sorted(values)
    index = (len(values) - 1) * fraction
    low = math.floor(index)
    high = math.ceil(index)
    if low == high:
        return values[low]
    return values[low] + (values[high] - values[low]) * (index - low)


def duration_stats(values):
    if not values:
        return "n=0"
    return "n={} p50={:.3f}ms p95={:.3f}ms p99={:.3f}ms".format(
        len(values), percentile(values, 0.50) / 1000.0,
        percentile(values, 0.95) / 1000.0, percentile(values, 0.99) / 1000.0)


def complete_spans(events):
    spans = []
    for event in events:
        duration = event.get("dur", 0.0)
        if event.get("ph") != "X" or duration <= 0.0:
            continue
        spans.append({
            "name": event.get("name", ""),
            "tid": event.get("tid"),
            "id": event.get("args", {}).get("id", 0),
            "start": event.get("ts", 0.0),
            "end": event.get("ts", 0.0) + duration,
            "dur": duration,
        })
    return spans


def counter_summaries(events):
    grouped = defaultdict(list)
    for event in events:
        if event.get("ph") != "C":
            continue
        value = event.get("args", {}).get("value")
        if isinstance(value, (int, float)):
            grouped[event.get("name", "")].append(value)
    return grouped


def attribute_frames(spans, frame_name, partial_ids):
    """Sweep each thread once; the innermost active span owns wall-time slices."""
    frames_by_thread = defaultdict(list)
    spans_by_thread = defaultdict(list)
    for span in spans:
        if span["name"] == frame_name and span["id"] not in partial_ids:
            frames_by_thread[span["tid"]].append(span)
        elif span["name"] != frame_name:
            spans_by_thread[span["tid"]].append(dict(span, active=False))

    exclusive_totals = Counter()
    residuals = []
    frame_durations = []
    for thread_id, frames in frames_by_thread.items():
        frames.sort(key=lambda item: (item["start"], item["end"]))
        # Trace timestamps/durations are written with three decimal places in
        # microseconds. Reconstructing end as start+duration can therefore make
        # two adjacent frames overlap by a few nanoseconds. Give both frames a
        # shared midpoint boundary so the sequence remains closed. A real
        # re-entrant/overlapping frame scope must still fail loudly.
        tolerance_us = 0.002 + 1e-12
        for previous_frame, frame in zip(frames, frames[1:]):
            overlap_us = previous_frame["end"] - frame["start"]
            if overlap_us <= 0.0:
                continue
            if overlap_us > tolerance_us:
                raise ValueError("overlapping frame scopes on one thread are ambiguous")
            boundary = (previous_frame["end"] + frame["start"]) * 0.5
            previous_frame["end"] = boundary
            previous_frame["dur"] = previous_frame["end"] - previous_frame["start"]
            frame["start"] = boundary
            frame["dur"] = frame["end"] - frame["start"]
        thread_spans = spans_by_thread[thread_id]
        boundaries = []
        for index, frame in enumerate(frames):
            boundaries.append((frame["start"], 1, index))
            boundaries.append((frame["end"], 0, index))
        for index, span in enumerate(thread_spans):
            boundaries.append((span["start"], 3, index))
            boundaries.append((span["end"], 2, index))

        # At a shared start, add the longer parent before its child. Zero-length
        # X events have already been filtered, so an end never precedes its start.
        boundaries.sort(key=lambda item: (
            item[0], item[1],
            -thread_spans[item[2]]["end"] if item[1] == 3 else 0,
        ))
        active = []
        current_frame = None
        covered_by_frame = Counter()
        previous = boundaries[0][0] if boundaries else 0.0

        for timestamp, kind, index in boundaries:
            if timestamp > previous and current_frame is not None:
                while active and not active[-1]["active"]:
                    active.pop()
                if active:
                    delta = timestamp - previous
                    exclusive_totals[active[-1]["name"]] += delta
                    covered_by_frame[current_frame] += delta

            if kind == 0:
                if current_frame == index:
                    current_frame = None
            elif kind == 1:
                if current_frame is not None:
                    raise ValueError("overlapping frame scopes on one thread are ambiguous")
                current_frame = index
            elif kind == 2:
                thread_spans[index]["active"] = False
            else:
                thread_spans[index]["active"] = True
                active.append(thread_spans[index])
            previous = timestamp

        for index, frame in enumerate(frames):
            frame_durations.append(frame["dur"])
            residuals.append(max(0.0, frame["dur"] - covered_by_frame[index]))

    return exclusive_totals, residuals, frame_durations


def run_self_test():
    fixture = [
        {"name": "frame.wall", "ph": "X", "ts": 0, "dur": 10000, "tid": 1, "args": {"id": 1}},
        {"name": "outer", "ph": "X", "ts": 1000, "dur": 8000, "tid": 1, "args": {}},
        {"name": "child", "ph": "X", "ts": 2000, "dur": 1000, "tid": 1, "args": {}},
        {"name": "zero", "ph": "X", "ts": 3000, "dur": 0, "tid": 1, "args": {}},
        {"name": "same_parent", "ph": "X", "ts": 4000, "dur": 1000, "tid": 1, "args": {}},
        {"name": "same_child", "ph": "X", "ts": 4000, "dur": 500, "tid": 1, "args": {}},
        {"name": "other", "ph": "X", "ts": 2000, "dur": 5000, "tid": 2, "args": {}},
        {"name": "frame.wall", "ph": "X", "ts": 11000, "dur": 1000, "tid": 1, "args": {"id": 2}},
        {"name": "frame_partial", "ph": "C", "tid": 1, "args": {"id": 2, "value": 1}},
    ]
    totals, residuals, frames = attribute_frames(complete_spans(fixture), "frame.wall", {2})
    assert frames == [10000]
    assert totals["child"] == 1000 and totals["outer"] == 6000
    assert totals["same_parent"] == 500 and totals["same_child"] == 500
    assert "zero" not in totals and "other" not in totals and residuals == [2000]

    one_ns_overlap = [
        {"name": "frame.wall", "tid": 1, "id": 1, "start": 0.0, "end": 1000.0, "dur": 1000.0},
        {"name": "frame.wall", "tid": 1, "id": 2, "start": 999.999, "end": 1999.999, "dur": 1000.0},
    ]
    _, _, normalized_frames = attribute_frames(one_ns_overlap, "frame.wall", set())
    assert len(normalized_frames) == 2 and abs(sum(normalized_frames) - 1999.999) < 1e-9
    real_overlap = [
        {"name": "frame.wall", "tid": 1, "id": 1, "start": 0.0, "end": 1000.0, "dur": 1000.0},
        {"name": "frame.wall", "tid": 1, "id": 2, "start": 990.0, "end": 1990.0, "dur": 1000.0},
    ]
    try:
        attribute_frames(real_overlap, "frame.wall", set())
        raise AssertionError("real overlapping frames must be rejected")
    except ValueError:
        pass

    synthetic = []
    for index in range(250000):
        synthetic.append({
            "name": "frame.wall" if index % 100 == 0 else "work",
            "tid": 1 if index % 5 else 2,
            "id": index,
            "start": float(index),
            "end": float(index + 1),
            "dur": 1.0,
        })
    started = time.perf_counter()
    attribute_frames(synthetic, "frame.wall", set())
    elapsed = time.perf_counter() - started
    assert elapsed < 15.0, elapsed
    print("self-test passed: 250k-event sweep {:.3f}s".format(elapsed))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", nargs="?")
    parser.add_argument("--frame-name", default="frame.wall")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        run_self_test()
        return
    if not args.trace:
        parser.error("trace required unless --self-test")

    with open(args.trace, encoding="utf-8") as source:
        events = json.load(source)["traceEvents"]
    metadata = next((event.get("args", {}) for event in events
                     if event.get("name") == "perf_trace_metadata"), {})
    print("events={} maxEvents={} dropped={} contentionDropped={} filtered={} truncated={}".format(
        metadata.get("recorded", len(events)), metadata.get("maxEvents", "?"),
        metadata.get("dropped", 0), metadata.get("contentionDropped", 0),
        metadata.get("filtered", 0), metadata.get("truncated", False)))
    if metadata.get("dropped", 0):
        print("WARNING: dropped events make capture incomplete.")

    partial_ids = {event.get("args", {}).get("id", 0) for event in events
                   if event.get("ph") == "C" and event.get("name") == "frame_partial"}
    if partial_ids:
        print("WARNING: excluded {} partial frame id(s).".format(len(partial_ids)))

    spans = complete_spans(events)
    span_durations = defaultdict(list)
    for span in spans:
        span_durations[span["name"]].append(span["dur"])
    print("scope wall spans:")
    for name, durations in sorted(span_durations.items(), key=lambda item: sum(item[1]), reverse=True):
        print("  {}: {}".format(name, duration_stats(durations)))

    counters = counter_summaries(events)
    if counters:
        print("counter values (units follow the counter name):")
        for name, values in sorted(counters.items()):
            print("  {}: n={} min={} max={} last={}".format(
                name, len(values), min(values), max(values), values[-1]))

    totals, residuals, frames = attribute_frames(spans, args.frame_name, partial_ids)
    print("complete {}: {}".format(args.frame_name, duration_stats(frames)))
    print("same-thread exclusive wall attribution:")
    for name, duration in totals.most_common():
        print("  {}: {:.3f}ms".format(name, duration / 1000.0))
    print("unattributed residual: {}".format(duration_stats(residuals)))
    print("WARNING: wall spans are not CPU running time; do not add across threads. "
          "Use an OS profiler for scheduling/CPU samples.")


if __name__ == "__main__":
    main()
