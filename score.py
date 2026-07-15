import argparse
import hashlib
import json
import common

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stream_seed", required=True)
    ap.add_argument("--duration", type=float, default=30)
    ap.add_argument("--playout_log", default="playout_log.json")
    ap.add_argument("--relay_stats", default="relay_stats.json")
    args = ap.parse_args()

    log = json.load(open(args.playout_log))
    stats = json.load(open(args.relay_stats))
    frames = log["frames"]
    n = len(frames)

    misses = 0
    for fr in frames:
        if not fr["present"]:
            misses += 1
            continue
        want = hashlib.sha256(
            common.frame_payload(args.stream_seed, fr["i"])).hexdigest()
        if fr["sha"] != want:
            misses += 1
    miss_rate = misses / max(1, n)

    raw = n * common.PAYLOAD_BYTES
    overhead = (stats["up_bytes"] + stats["down_bytes"]) / max(1, raw)

    valid = miss_rate <= 0.01 and overhead <= 2.0
    print(f"Miss rate: {miss_rate*100:.2f}% (limit <= 1%)")
    print(f"Overhead:  {overhead:.2f}x (limit <= 2.0x)")
    print(f"Delay:     {log['delay_ms']} ms")
    if valid:
        print("VALID")
    else:
        print("INVALID")

if __name__ == "__main__":
    main()
