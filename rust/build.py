# Copyright (c) 2026 OceanBase.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Build the Cargo workspace and export the C++ artifacts in exports.json."""

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys

WORKSPACE = Path(__file__).absolute().parent
EXPORTS = json.loads((WORKSPACE / "exports.json").read_text())


def build(args):
    output = Path(args.output).resolve()
    target_dir = Path(args.target_dir).resolve()
    command = [args.cargo, "build", "--locked", "--workspace", "--profile", args.profile,
               "--manifest-path", str(WORKSPACE / "Cargo.toml"),
               "--target-dir", str(target_dir), "--message-format=json-render-diagnostics"]
    if args.target:
        command.extend(["--target", args.target])
    command.extend(args.cargo_args)
    libraries = {}
    packages = {}
    build_dirs = {}
    with subprocess.Popen(command, cwd=WORKSPACE, stdout=subprocess.PIPE,
                          text=True) as cargo:
        for line in cargo.stdout:
            message = json.loads(line)
            if message["reason"] == "build-script-executed":
                build_dirs[message["package_id"]] = Path(message["out_dir"])
            elif message["reason"] == "compiler-artifact":
                packages[message["target"]["name"]] = message["package_id"]
                if "staticlib" in message["target"]["crate_types"]:
                    archive = next(Path(p) for p in message["filenames"]
                                   if p.endswith((".a", ".lib")))
                    libraries[message["target"]["name"]] = archive
        if cargo.wait():
            raise SystemExit(cargo.returncode)
    output.mkdir(parents=True, exist_ok=True)
    copied = set()
    for name, export in EXPORTS.items():
        archive = libraries[export.get("library", name)]
        package = packages[export["crate"].replace("-", "_")]
        library_name = archive.name
        if library_name not in copied:
            shutil.copyfile(archive, output / library_name)
            copied.add(library_name)
        for header, source in export["headers"].items():
            destination = output / "include" / header
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(build_dirs[package] / source, destination)


def header_names():
    return sorted({header for export in EXPORTS.values() for header in export["headers"]})


def check_headers(args):
    stale = []
    for name in header_names():
        checked = WORKSPACE / "include" / name
        generated = Path(args.generated) / name
        if (not checked.is_file()
                or checked.read_text(encoding="utf-8") != generated.read_text(encoding="utf-8")):
            stale.append(name)
    if stale:
        raise SystemExit("Rust interface headers need updating: " + ", ".join(stale)
                         + "\nRun: python3 rust/build.py update-headers")
    Path(args.stamp).write_text("")


def update_headers(args):
    launcher = [sys.executable, str(WORKSPACE.parent / "build.py")]
    subprocess.run(launcher + ["build", "//rust:generated_headers"],
                   cwd=WORKSPACE.parent, check=True)
    output = subprocess.check_output(launcher + ["info", "bazel-bin"],
                                     cwd=WORKSPACE.parent, text=True).strip()
    for name in header_names():
        destination = WORKSPACE / "include" / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(Path(output) / "rust/cargo/include" / name, destination)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    compile_parser = commands.add_parser("build", help="build Rust libraries and C++ headers")
    compile_parser.add_argument("--cargo", default="cargo")
    compile_parser.add_argument("--profile", default="release")
    compile_parser.add_argument("--target")
    compile_parser.add_argument("--target-dir", required=True)
    compile_parser.add_argument("--output", required=True)
    compile_parser.add_argument("cargo_args", nargs="*",
                                help="extra cargo build arguments, given after --")
    compile_parser.set_defaults(run=build)
    check = commands.add_parser("check-headers", help="check the committed C++ interface")
    check.add_argument("--generated", required=True)
    check.add_argument("--stamp", required=True)
    check.set_defaults(run=check_headers)
    update = commands.add_parser("update-headers", help="regenerate the committed C++ interface")
    update.set_defaults(run=update_headers)
    args = parser.parse_args()
    args.run(args)


if __name__ == "__main__":
    main()
