#!/usr/bin/env python3
"""Check a relocatable Linux/macOS bundle, its C ABI and GUI startup."""
import argparse
import ctypes
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


class Buffer(ctypes.Structure):
    _fields_ = [("data", ctypes.c_void_p), ("length", ctypes.c_size_t)]


def check_core(library, sample):
    core = ctypes.CDLL(str(library))
    core.pdscope_version.restype = ctypes.c_char_p
    core.pdscope_abi_version.restype = ctypes.c_uint32
    core.pdscope_open_file.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
    core.pdscope_open_file.restype = ctypes.c_void_p
    core.pdscope_decode.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(Buffer)]
    core.pdscope_decode.restype = ctypes.c_int32
    core.pdscope_close.argtypes = [ctypes.c_void_p]
    core.pdscope_buf_free.argtypes = [ctypes.POINTER(Buffer)]
    core.pdscope_str_free.argtypes = [ctypes.c_void_p]
    assert core.pdscope_abi_version() == 1, "unsupported ABI"
    error = ctypes.c_void_p()
    session = core.pdscope_open_file(os.fsencode(sample), ctypes.byref(error))
    if not session:
        message = ctypes.string_at(error).decode("utf-8") if error.value else "open failed"
        core.pdscope_str_free(error)
        raise RuntimeError(message)
    stats = Buffer()
    try:
        status = core.pdscope_decode(session, None, ctypes.byref(stats))
        if status != 0:
            raise RuntimeError(f"decode failed: {status}")
        result = json.loads(ctypes.string_at(stats.data, stats.length))
        if result.get("packetCount", 0) <= 0:
            raise RuntimeError("sample decoded no packets")
        print(f"Bundled core {core.pdscope_version().decode()}: {result['packetCount']} packets")
    finally:
        core.pdscope_buf_free(ctypes.byref(stats))
        core.pdscope_close(session)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path, required=True)
    parser.add_argument("--sample", type=Path, required=True)
    args = parser.parse_args()
    bundle = args.bundle.resolve(strict=True)
    sample = args.sample.resolve(strict=True)
    if sys.platform == "darwin":
        executable = bundle / "Contents/MacOS/PDScope"
        library = bundle / "Contents/Frameworks/libpdscope.dylib"
    elif sys.platform.startswith("linux"):
        executable = bundle / "PDScope"
        library = bundle / "lib/libpdscope.so"
    else:
        parser.error("use smoke-shell.py on Windows")
    for path in (executable, library):
        if not path.is_file():
            raise RuntimeError(f"missing bundle file: {path}")
    check_core(library, sample)
    # A different cwd and no dev override ensure the packaged library is used.
    env = os.environ.copy()
    env.pop("PDSCOPE_LIB_DIR", None)
    with tempfile.TemporaryDirectory(prefix="pdscope-smoke-") as directory:
        log = Path(directory) / "startup.log"
        with log.open("wb") as output:
            process = subprocess.Popen(
                [str(executable), str(sample)], cwd=directory, env=env,
                stdout=output, stderr=subprocess.STDOUT,
            )
            try:
                deadline = time.monotonic() + 30
                ready = False
                while time.monotonic() < deadline:
                    if process.poll() is not None:
                        raise RuntimeError(f"GUI exited ({process.returncode}):\n{log.read_text(errors='replace')}")
                    if "PDScope ready" in log.read_text(errors="replace"):
                        ready = True
                        break
                    time.sleep(0.2)
                text = log.read_text(errors="replace")
                if any(message in text for message in (
                    "Unhandled exception", "找不到核心动态库", "PDScope startup failed:",
                )):
                    raise RuntimeError(text)
                if not ready:
                    raise RuntimeError(f"GUI did not render its first frame within 30 seconds:\n{text}")
                print("GUI loaded the core and rendered its first frame outside the source directory")
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()


if __name__ == "__main__":
    main()
