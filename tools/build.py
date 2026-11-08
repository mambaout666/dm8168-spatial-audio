#!/usr/bin/env python3
"""Build the portable C backend and tests without third-party Python packages."""

import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]


def split_command(value):
    """Keep an executable path containing spaces intact; also accept CC flags."""
    if Path(value).is_file():
        return [str(Path(value).resolve())]
    parts = shlex.split(value, posix=os.name != "nt")
    if os.name == "nt":
        parts = [part[1:-1] if len(part) >= 2 and part[0] == part[-1]
                 and part[0] in "\"'" else part for part in parts]
    if not parts:
        raise ValueError("compiler command is empty")
    return parts


def find_compiler(args):
    if args.zig:
        return [str(Path(args.zig).resolve()), "cc"]
    configured = args.cc or os.environ.get("CC")
    if configured:
        return split_command(configured)
    for name in ("cc", "gcc", "clang"):
        executable = shutil.which(name)
        if executable:
            return [executable]
    # Optional local tool cache; it is outside the repository and never shipped.
    for filename in ("zig.exe", "zig"):
        executable = ROOT.parent / "_tools" / "dm8168-build" / "ziglang" / filename
        if executable.is_file():
            return [str(executable), "cc"]
    raise ValueError("no C compiler found; install GCC/Clang, set CC, or pass --zig PATH")


def display_command(command):
    if os.name == "nt":
        return subprocess.list2cmdline([str(item) for item in command])
    return " ".join(shlex.quote(str(item)) for item in command)


def run(command):
    command = [str(item) for item in command]
    print("+ " + display_command(command), flush=True)
    subprocess.run(command, cwd=str(ROOT), check=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    compiler_group = parser.add_mutually_exclusive_group()
    compiler_group.add_argument("--cc", help="compiler executable or command (also reads CC)")
    compiler_group.add_argument("--zig", help="Zig executable, used as 'zig cc'")
    parser.add_argument("--test", action="store_true", help="run the native C test executables")
    parser.add_argument("--sanitize", action="store_true", help="enable AddressSanitizer and UBSan")
    parser.add_argument("--core-only", action="store_true", help="build only the portable FIR core test")
    parser.add_argument("--build-dir", default="build", help="output directory, relative to project root")
    args = parser.parse_args(argv)
    try:
        compiler = find_compiler(args)
        output = Path(args.build_dir)
        if not output.is_absolute():
            output = ROOT / output
        output = output.resolve()
        # Keep generated project files within the workspace repository.
        output.relative_to(ROOT)
        output.mkdir(parents=True, exist_ok=True)
        suffix = ".exe" if os.name == "nt" else ""
        flags = ["-std=c99", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "include")]
        if os.name != "nt":
            flags += ["-D_POSIX_C_SOURCE=200809L", "-D_FILE_OFFSET_BITS=64"]
        if args.sanitize:
            flags += ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        tests = []

        def build(name, sources, extra_flags=(), libraries=()):
            paths = [ROOT / source for source in sources]
            missing = [str(path.relative_to(ROOT)) for path in paths if not path.is_file()]
            if missing:
                raise ValueError("required source files missing: " + ", ".join(missing))
            destination = output / (name + suffix)
            run(compiler + flags + list(extra_flags) + [str(path) for path in paths]
                + ["-o", str(destination)] + list(libraries))
            return destination

        tests.append(build("test-core", ["src/spatial_core.c", "tests/test_core.c"]))
        if not args.core_only:
            build("spatial-audio", ["src/spatial_core.c", "src/bank_io.c", "src/backend.c", "src/main.c"],
                  libraries=[] if os.name == "nt" else ["-ldl", "-lm", "-lrt"])
            tests.append(build("test-dsp", ["src/spatial_core.c", "rpe/spatial_dsp.c", "tests/test_dsp.c"],
                               extra_flags=["-I", str(ROOT / "rpe")]))
        if args.test:
            for executable in tests:
                run([str(executable)])
        print("Build completed: " + str(output))
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, "build error: {}\n".format(error))
    return 0


if __name__ == "__main__":
    sys.exit(main())
