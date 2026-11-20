import statistics, subprocess, sys, time

mo, program, port = sys.argv[1], sys.argv[2], sys.argv[3]
server = subprocess.Popen([sys.executable, "learnability/phase7/preflight/http-in-mo/server.py", port])
time.sleep(1)
try:
    for size in (1048576, 102400):
        for kind in ("fixed", "chunked"):
            walls, outs = [], set()
            for _ in range(5):
                start = time.perf_counter()
                done = subprocess.run([mo, "run", program, port, f"/{kind}/{size}"], capture_output=True, text=True)
                walls.append(time.perf_counter() - start)
                outs.add((done.returncode, done.stdout.strip(), done.stderr.strip()))
            print(kind, size, f"median {statistics.median(walls):.2f} s", " ".join(f"{w:.2f}" for w in walls), outs)
    for kind in ("fixed", "chunked"):
        low, high = 1 << 20, 256 << 20
        while high - low > (1 << 20):
            middle = (low + high) // 2
            done = subprocess.run([mo, "run", "--max-heap", str(middle), program, port, f"/{kind}/1048576"], capture_output=True, text=True)
            if done.returncode == 0:
                high = middle
            else:
                low = middle
        print(kind, "smallest --max-heap that passes, within 1 MiB:", high / 2**20, "MiB")
finally:
    server.kill()
