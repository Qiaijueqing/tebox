#!/usr/bin/env python3
"""Connect QEMU ADB, record a strict H.264 smoke test, then open scrcpy.

Use --launch after stopping the existing VM to boot with loopback TCP ADB.
Without --launch, attach to a VM already booted with ADB (default; or QEMU_ADB=1).
The launched VM uses VirGL and snapshot mode and stops when this script exits.
Logs, a recording and a decoded PNG are retained in out/codec-probe/<timestamp>.
"""
import argparse
import datetime
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--variant", default=os.environ.get(
        "VARIANT", "aosp_arm64-BP4A.251205.006"))
    parser.add_argument("--launch", action="store_true")
    parser.add_argument("--port", type=int, default=5555)
    parser.add_argument("--encoder", help="explicit MediaCodec encoder name")
    parser.add_argument("--seconds", type=int, default=12)
    parser.add_argument("--test-only", action="store_true")
    args = parser.parse_args()
    if not 1 <= args.port <= 65535 or not 1 <= args.seconds <= 120:
        parser.error("port must be 1..65535 and seconds must be 1..120")
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", args.variant):
        parser.error("invalid variant")
    for tool in ("adb", "scrcpy", "ffmpeg", "ffprobe"):
        if not shutil.which(tool):
            parser.error(f"missing tool: {tool}")
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    output = ROOT / "out" / "codec-probe" / stamp
    output.mkdir(parents=True)
    serial = f"127.0.0.1:{args.port}"
    adb = ["adb", "-s", serial]
    vm = None
    logcat = None
    handles = []
    result = {"serial": serial, "variant": args.variant,
              "guest_test": "not_run", "launched_vm": args.launch}

    def run(command, name, timeout=30, required=True):
        completed = subprocess.run(command, cwd=ROOT, text=True,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   timeout=timeout)
        (output / name).write_text(completed.stdout)
        if required and completed.returncode:
            raise RuntimeError(f"command failed ({completed.returncode}); see {output / name}")
        return completed

    def log_handle(name):
        handle = (output / name).open("w")
        handles.append(handle)
        return handle

    try:
        if sys.platform == "darwin":
            host = run(["ffmpeg", "-hide_banner", "-v", "verbose", "-f", "lavfi",
                        "-i", "testsrc2=size=1080x2400:rate=30", "-frames:v", "60",
                        "-pix_fmt", "nv12", "-c:v", "h264_videotoolbox",
                        "-allow_sw", "0", "-realtime", "1", "-b:v", "8M",
                        "-an", "-y", str(output / "host-h264-hardware.mp4")],
                       "host-h264-hardware.log", timeout=45, required=False)
            result["host_hardware_encode"] = "passed" if host.returncode == 0 else "failed"
            print(f"Host VideoToolbox hardware encode: {result['host_hardware_encode']}",
                  flush=True)
        if args.launch:
            # Fail closed when host process inspection is unavailable. Never
            # let ensure-runtime-imgs replace images under an unobserved VM.
            check = subprocess.run(["pgrep", "-f", f"qemu-system-aarch64.*{args.variant}"],
                                   capture_output=True, text=True)
            if check.returncode == 0:
                raise RuntimeError("stop the existing QEMU before --launch; userdata is preserved")
            if check.returncode != 1 or check.stderr.strip():
                raise RuntimeError(f"cannot check for running QEMU: {check.stderr.strip()}")
            env = os.environ.copy()
            env.update(QEMU_ADB="1", ADB_PORT=str(args.port), FORCE_VIRGL="1",
                       SNAPSHOT="1", QEMU_DEBUG="1",
                       QEMU_SERIAL=f"file:{output / 'serial.log'}")
            vm = subprocess.Popen([str(ROOT / "run"), args.variant], cwd=ROOT, env=env,
                                  stdin=subprocess.DEVNULL,
                                  stdout=log_handle("launch.log"), stderr=subprocess.STDOUT)
        run(["adb", "start-server"], "adb-server.log")
        deadline = time.monotonic() + 180
        while True:
            if vm and vm.poll() is not None:
                raise RuntimeError(f"QEMU exited ({vm.returncode}); see {output / 'launch.log'}")
            connected = run(["adb", "connect", serial], "adb-connect.log", required=False)
            if connected.returncode or "Operation not permitted" in connected.stdout:
                raise RuntimeError(f"ADB connection failed; see {output / 'adb-connect.log'}")
            state = run(adb + ["get-state"], "adb-state.log", required=False)
            if "unauthorized" in state.stdout:
                print("ADB still unauthorized; expect ro.adb.secure=0 from initramfs.", flush=True)
            if state.returncode == 0 and state.stdout.strip() == "device":
                boot = run(adb + ["shell", "getprop", "sys.boot_completed"],
                           "boot-completed.txt", required=False)
                if boot.returncode == 0 and boot.stdout.strip() == "1":
                    break
            if time.monotonic() >= deadline:
                raise RuntimeError("ADB/boot timed out. Ensure ADB is enabled (default) and initramfs sets ro.adb.secure=0.")
            time.sleep(2)
        print(f"Connected {serial}; evidence: {output}", flush=True)
        run(adb + ["shell", "getprop"], "properties.txt")
        run(adb + ["shell", "wm size; wm density; dumpsys SurfaceFlinger"], "display.txt")
        if args.launch:
            run([sys.executable, str(ROOT / "scripts" / "qemu-console.py"),
                 "getprop sys.boot_completed; wm size; wm density; "
                 "dumpsys SurfaceFlinger | grep -i GLES; logcat -d -b crash",
                 "--socket", str(ROOT / "out" / f"test-{args.variant}" / "debug.sock")],
                "console-check.txt", required=False)
        run(adb + ["shell", "service list; lshal; ls -l /dev/video* /dev/goldfish* /dev/vsock"],
            "hal-and-devices.txt", required=False)
        run(adb + ["shell", "dumpsys media.codec; dumpsys media.metrics"],
            "media-before.txt", required=False)
        run(adb + ["logcat", "-d", "-b", "crash"], "crash-before.txt")
        logcat = subprocess.Popen(adb + ["logcat", "-v", "threadtime", "-T", "1"],
                                  stdout=log_handle("logcat.txt"), stderr=subprocess.STDOUT)
        encoders = run(["scrcpy", "-s", serial, "--list-encoders"], "encoders.txt")
        print(encoders.stdout, flush=True)
        base = ["scrcpy", "-s", serial, "--no-audio", "--video-codec=h264",
                "--video-bit-rate=8M", "--max-fps=30", "--video-codec-options=frame-rate=30", "--print-fps",
                "--no-downsize-on-error", "--verbosity=debug",
                "--window-width=360", "--window-height=800"]
        if args.encoder:
            base += [f"--video-encoder={args.encoder}"]
        display_base = base
        for label, size, extra in (("native", (1080, 2400), []),
                                   ("supported", (720, 1600), ["--max-size=1600"])):
            test_result = {"expected_size": list(size)}
            result[label] = test_result
            recording = output / f"guest-{label}.mp4"
            try:
                test = run(base + extra + ["--no-control", f"--time-limit={args.seconds}",
                                          f"--record={recording}"],
                           f"scrcpy-{label}.log", timeout=args.seconds + 45, required=False)
                if test.returncode:
                    raise RuntimeError(f"scrcpy failed ({test.returncode})")
                probe = run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-count_frames",
                             "-show_entries", "stream=codec_name,width,height,nb_read_frames",
                             "-of", "json", str(recording)], f"ffprobe-{label}.json")
                streams = json.loads(probe.stdout).get("streams", [])
                if not streams or int(streams[0].get("nb_read_frames", "0")) < 1:
                    raise RuntimeError("recording contains no decodable video frames")
                test_result["stream"] = streams[0]
                if (streams[0]["width"], streams[0]["height"]) != size:
                    raise RuntimeError(f"unexpected capture size: {streams[0]}")
                frame = output / f"guest-{label}-frame.png"
                run(["ffmpeg", "-hide_banner", "-v", "error", "-i", str(recording),
                     "-frames:v", "1", "-y", str(frame)], f"frame-{label}-decode.log")
                # Packets alone do not establish that the mapper returned real pixels.
                stats = output / f"luma-{label}.txt"
                run(["ffmpeg", "-hide_banner", "-v", "error", "-i", str(recording),
                     "-vf", f"signalstats,metadata=print:file={stats}", "-f", "null", "-"],
                    f"luma-{label}-decode.log")
                luma = stats.read_text()
                low = re.findall(r"lavfi.signalstats.YMIN=([0-9.]+)", luma)
                high = re.findall(r"lavfi.signalstats.YMAX=([0-9.]+)", luma)
                if not low or not high or len(low) != len(high):
                    raise RuntimeError("missing decoded luma statistics")
                test_result["max_luma_range"] = max(float(b) - float(a) for a, b in zip(low, high))
                if test_result["max_luma_range"] < 8:
                    raise RuntimeError("nearly uniform frames; inspect PNG and mapper logs")
                test_result["status"] = "decodable_nonflat_video"
                test_result["visual_check"] = "inspect PNG / scrcpy for text and colors"
                result["guest_test"] = label
                display_base = base + extra
                print(f"{label}: decoded H.264 at {size}; inspect {frame}.", flush=True)
                break
            except (RuntimeError, ValueError) as error:
                test_result.update(status="failed", error=str(error))
                print(f"{label}: {error}; evidence retained.", flush=True)
        run(adb + ["shell", "dumpsys media.codec; dumpsys media.metrics"],
            "media-after.txt", required=False)
        run(adb + ["logcat", "-d", "-b", "crash"], "crash-after.txt", required=False)
        if result["guest_test"] == "not_run":
            result["guest_test"] = "failed"
            raise RuntimeError("both native and supported-size encoder probes failed")
        if not args.test_only:
            print("Opening scrcpy. Closing it stops the VM started by --launch.", flush=True)
            subprocess.run(display_base, cwd=ROOT, check=True)
    except (RuntimeError, subprocess.SubprocessError, OSError, ValueError) as error:
        result["error"] = str(error)
        print(f"ERROR: {error}\nEvidence: {output}", file=sys.stderr)
        return 1
    finally:
        for child in (logcat, vm):
            if child and child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()
        for handle in handles:
            handle.close()
        (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
