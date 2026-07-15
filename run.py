import argparse
import os
import secrets
import shlex
import subprocess
import sys
import time

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--profile", default="profiles/A.json")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--delay_ms", type=float, default=60)
    ap.add_argument("--duration", type=float, default=30)
    ap.add_argument("--sender_cmd", default="./sender")
    ap.add_argument("--receiver_cmd", default="./receiver")
    args = ap.parse_args()

    here = os.path.dirname(os.path.abspath(__file__)) or "."

    for cmd, flag in ((args.sender_cmd, "--sender_cmd"),
                      (args.receiver_cmd, "--receiver_cmd")):
        exe = shlex.split(cmd)[0]
        if os.sep in exe and not os.path.exists(os.path.join(here, exe)):
            sys.exit(f"{exe} not found")

    for f in ("playout_log.json", "relay_stats.json"):
        try:
            os.unlink(os.path.join(here, f))
        except FileNotFoundError:
            pass

    stream_seed = secrets.token_hex(8)
    t0 = time.time() + 1.5
    student_env = dict(os.environ, T0=str(t0), DURATION_S=str(args.duration),
                       DELAY_MS=str(args.delay_ms))

    procs = []
    try:
        relay = subprocess.Popen(
            [sys.executable, "relay.py", "--profile", args.profile,
             "--seed", str(args.seed), "--duration", str(args.duration)],
            cwd=here)
        procs.append(relay)
        recv = subprocess.Popen(shlex.split(args.receiver_cmd), cwd=here,
                                env=student_env)
        procs.append(recv)
        send = subprocess.Popen(shlex.split(args.sender_cmd), cwd=here,
                                env=student_env)
        procs.append(send)
        time.sleep(0.3)
        for p, what, hint in ((relay, "relay", ""),
                              (recv, "receiver", ""),
                              (send, "sender", "")):
            if p.poll() is not None:
                sys.exit(f"{what} exited")
        
        endpoints = subprocess.Popen(
            [sys.executable, "endpoints.py", "--t0", str(t0),
             "--duration", str(args.duration), "--delay_ms", str(args.delay_ms)],
            cwd=here, stdin=subprocess.PIPE)
        procs.append(endpoints)
        endpoints.stdin.write((stream_seed + "\n").encode())
        endpoints.stdin.close()

        endpoints.wait(timeout=args.duration + 60)
        for p in (send, recv):
            p.kill()
        relay.wait(timeout=30)
        if endpoints.returncode != 0:
            sys.exit("endpoints failed")
        if relay.returncode != 0:
            sys.exit("relay failed")
    finally:
        for p in procs:
            if p.poll() is None:
                p.kill()

    subprocess.run([sys.executable, "score.py", "--stream_seed", stream_seed,
                    "--duration", str(args.duration)], cwd=here, check=True)

if __name__ == "__main__":
    main()
