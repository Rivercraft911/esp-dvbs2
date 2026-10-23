"""Slim, device-free checks of TS rejection and the receive process boundary."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
EXAMPLE = ROOT / "examples/live-video"
sys.path.insert(0, str(EXAMPLE))
from check_ts import check
from make_ts import make_ts
import receive


def rejects(data, reference=None):
    try:
        check(data, reference)
    except ValueError:
        return
    raise AssertionError("corrupt TS passed")


def test_ts():
    ts = make_ts(5)
    r = check(ts, ts)
    assert r["video_pids"] == [256] and r["tables"] == {"PAT": 5, "PMT": 5}
    rejects(ts[:-1])
    bad = bytearray(ts)
    bad[187] ^= 1  # PAT CRC (adaptation stuffing precedes the section)
    rejects(bad)
    packets = [ts[k:k+188] for k in range(0, len(ts), 188)]
    video = [n for n, p in enumerate(packets) if (p[1] & 31) * 256 + p[2] == 256]
    # Remove a video packet: PID-specific CC check must detect it.
    rejects(b"".join(packets[:video[1]] + packets[video[1]+1:]))
    # Interior null loss does not affect CC, but exact comparison must reject it.
    null = next(n for n, p in enumerate(packets) if p[1] & 31 == 31 and p[2] == 255)
    rejects(b"".join(packets[:null] + packets[null+1:]), ts)
    # Tail-trimmed reception remains legal when all required tables are present.
    assert check(ts[:-188], ts)["exact_reference_match"]


def test_processes():
    # Fake UHD writes diagnostics on stdout and data to its filename, just like
    # uhd_rx_cfile. A fake decoder copies input to expose any log contamination.
    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp)
        ts = make_ts(5)
        (folder / "source.ts").write_bytes(ts)
        source = folder / "source"
        source.write_text("#!" + sys.executable + "\nimport pathlib,sys\n"
                          "print('UHD status must not become IQ', flush=True)\n"
                          f"data=pathlib.Path({str(folder / 'source.ts')!r}).read_bytes()\n"
                          "with open(sys.argv[-1], 'wb') as f: f.write(data)\n")
        decoder = folder / "decoder"
        decoder.write_text("#!" + sys.executable + "\nimport sys,shutil\n"
                           "shutil.copyfileobj(sys.stdin.buffer,sys.stdout.buffer)\n")
        for path in (source, decoder):
            path.chmod(0o755)
        cmd = [sys.executable, str(EXAMPLE / "receive.py"), "--args", "fake",
               "--freq", "1", "--uhd-rx-cfile", str(source), "--receiver", str(decoder)]
        result = subprocess.run(cmd, capture_output=True, timeout=10, check=True)
        assert result.stdout == ts
        assert b"UHD status" in result.stderr
        # A cold player launch must not consume the capture's queue headroom: receive.run starts
        # the player before the source and the decoder. Check that order where it is decided, in
        # Popen, rather than by a write deadline: a deadline also measured the fake decoder's own
        # startup, and under load that flaked (UHD exit 7). The deadline stays as a hang guard.
        player = folder / "player"
        player.write_text("#!" + sys.executable + "\nimport sys\n"
                          f"open({str(folder/'played.ts')!r}, 'wb').write(sys.stdin.buffer.read())\n")
        player.chmod(0o755)
        source.write_text("#!" + sys.executable + "\nimport os,pathlib,sys,time\n"
                          f"data=memoryview(pathlib.Path({str(folder/'source.ts')!r}).read_bytes())\n"
                          "fd=os.open(sys.argv[-1],os.O_WRONLY|os.O_NONBLOCK)\n"
                          "deadline=time.monotonic()+10\n"
                          "while data:\n"
                          " if time.monotonic()>deadline: raise SystemExit(7)\n"
                          " try: data=data[os.write(fd,data):]\n"
                          " except BlockingIOError: time.sleep(.001)\n"
                          "os.close(fd)\n")
        popen = subprocess.Popen
        launched = []
        def cold_player(command, **kwargs):
            launched.append(Path(command[0]).name)
            if command[0] == str(player):
                time.sleep(.6)
            return popen(command, **kwargs)
        args = receive.parser().parse_args(cmd[2:] + ["--player", str(player),
                                           "--output", str(folder/'saved.ts')])
        receive.validate(args)
        with patch.object(receive.subprocess, 'Popen', cold_player):
            receive.run(args)
        assert launched == ["player", "source", "decoder"], launched
        assert (folder/'saved.ts').read_bytes() == (folder/'played.ts').read_bytes() == ts
        # A source which fails before opening its FIFO must not hang.
        source.write_text("#!" + sys.executable + "\nraise SystemExit(3)\n")
        result = subprocess.run(cmd, capture_output=True, timeout=10)
        assert result.returncode == 1 and b"failed to start" in result.stderr
    plan = subprocess.check_output([sys.executable, "-S", str(EXAMPLE / "receive.py"),
                                    "--args", "type=b200", "--freq", "2400000000", "--plan"])
    plan = json.loads(plan)
    assert "--output-shorts" in plan["source"] and "--s16" in plan["decoder"]
    assert plan["decoder"][plan["decoder"].index("--modcods")+1] == "128"


if __name__ == "__main__":
    test_ts()
    test_processes()
    print("live video: TS corruption rejected; FIFO data/log isolation and source failure pass; no devices opened")
