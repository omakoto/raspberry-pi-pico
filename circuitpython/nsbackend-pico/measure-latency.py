#!/usr/bin/env python3
#
# Latency Measurement Tool for nsbackend-pico.
#
# Measures TCP command round-trip latency (RTT) between the host PC and
# nsbackend-pico running on the microcontroller over Wi-Fi.
#
# NOTE: Requires 'enable_echo = true' in config.toml on the board so that
# incoming commands are echoed back to the client for RTT calculation.
#

import argparse
import socket
import statistics
import sys
import time


def measure_discrete_latency(
    sock: socket.socket,
    commands: list[str],
    interval_s: float = 0.01,
) -> list[float]:
    # Measures round-trip time for discrete individual commands.
    latencies: list[float] = []

    for cmd in commands:
        msg: bytes = (cmd + "\n").encode("utf-8")
        t0: float = time.perf_counter()
        sock.sendall(msg)
        _ = sock.recv(1024)
        t1: float = time.perf_counter()

        rtt_ms: float = (t1 - t0) * 1000.0
        latencies.append(rtt_ms)
        time.sleep(interval_s)

    return latencies


def measure_streaming_latency(
    sock: socket.socket,
    samples: int = 100,
    rate_hz: float = 100.0,
) -> list[float]:
    # Simulates continuous analog stick streaming at a given frequency.
    latencies: list[float] = []
    interval_s: float = 1.0 / rate_hz if rate_hz > 0 else 0.01

    for i in range(samples):
        # Generate sweeping analog stick X coordinate in [-1.0 .. 1.0]
        val: float = ((i % 20) - 10) / 10.0
        cmd: str = f"lx {val:.2f}"
        msg: bytes = (cmd + "\n").encode("utf-8")

        t0: float = time.perf_counter()
        sock.sendall(msg)
        _ = sock.recv(1024)
        t1: float = time.perf_counter()

        rtt_ms: float = (t1 - t0) * 1000.0
        latencies.append(rtt_ms)
        time.sleep(interval_s)

    return latencies


def print_stats(title: str, latencies: list[float]) -> None:
    if not latencies:
        print(f"{title}: No data collected.")
        return

    sorted_lats: list[float] = sorted(latencies)
    count: int = len(sorted_lats)
    p95_idx: int = min(count - 1, int(count * 0.95))
    p99_idx: int = min(count - 1, int(count * 0.99))

    print(f"\n--- {title} ({count} samples) ---")
    print(f"  Min:     {min(sorted_lats):.2f} ms")
    print(f"  Median:  {statistics.median(sorted_lats):.2f} ms")
    print(f"  Avg:     {statistics.mean(sorted_lats):.2f} ms")
    print(f"  P95:     {sorted_lats[p95_idx]:.2f} ms")
    print(f"  P99:     {sorted_lats[p99_idx]:.2f} ms")
    print(f"  Max:     {max(sorted_lats):.2f} ms")
    if count > 1:
        print(f"  StdDev:  {statistics.stdev(sorted_lats):.2f} ms")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Measure round-trip latency to nsbackend-pico over TCP. (Requires enable_echo = true in config.toml)"
    )
    parser.add_argument("--host", type=str, default="nscon.local", help="Target hostname or IP (default: nscon.local)")
    parser.add_argument("--port", type=int, default=10100, help="Target TCP port (default: 10100)")
    parser.add_argument("--count", type=int, default=50, help="Number of discrete test samples (default: 50)")
    parser.add_argument("--stream-samples", type=int, default=100, help="Number of streaming test samples (default: 100)")
    parser.add_argument("--stream-hz", type=float, default=100.0, help="Streaming rate in Hz (default: 100.0)")
    parser.add_argument("--timeout", type=float, default=5.0, help="Socket timeout in seconds (default: 5.0)")

    args = parser.parse_args()

    print(f"Connecting to {args.host}:{args.port}...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(args.timeout)

    try:
        sock.connect((args.host, args.port))
    except Exception as e:
        print(f"Error: Failed to connect to {args.host}:{args.port}: {e}", file=sys.stderr)
        print("Ensure nsbackend-pico is running and connected to Wi-Fi.", file=sys.stderr)
        return 1

    print("Connected! Verifying echo support...")
    try:
        sock.sendall(b"# ping\n")
        resp: bytes = sock.recv(1024)
        if not resp:
            print("Error: No echo response received. Ensure 'enable_echo = true' in config.toml on the board.", file=sys.stderr)
            sock.close()
            return 1
    except socket.timeout:
        print("Error: Timed out waiting for echo response. Ensure 'enable_echo = true' in config.toml on the board.", file=sys.stderr)
        sock.close()
        return 1

    print("Echo confirmed. Running latency benchmarks...\n")

    # 1. Discrete Commands Benchmark
    sample_pool: list[str] = ["a", "b", "x", "y", "lx 0.5", "ly -0.5", "pu", "pd", "l1", "r1"]
    discrete_commands: list[str] = [sample_pool[i % len(sample_pool)] for i in range(args.count)]

    discrete_latencies: list[float] = measure_discrete_latency(sock, discrete_commands)
    print_stats("Discrete Commands", discrete_latencies)

    # 2. Continuous 100Hz Streaming Benchmark
    streaming_latencies: list[float] = measure_streaming_latency(sock, samples=args.stream_samples, rate_hz=args.stream_hz)
    print_stats(f"Continuous Streaming ({args.stream_hz:.0f} Hz)", streaming_latencies)

    sock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
