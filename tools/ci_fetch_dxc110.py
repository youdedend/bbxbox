#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fetch a signed DXC 1.10 (and spirv-tools) from conda-forge into a directory.

The prebaked cache is only usable if every blob in it is signed, and whether a
given `libdxcompiler.so` signs is a property of that build: its `libdxil.so` must
sit beside it, or DXC returns a DXBC container whose digest is zeros and the
console refuses every shader the bake spent an hour making. So the bake does not
take a compiler on trust - `bbx_bake_cache --check-compiler` prints what the one
in hand does - and CI fetches a build that is known to sign on every push, so
the gate is exercised rather than remembered.

A `.conda` file is a zip of zstd-compressed tars. conda-forge publishes a
`.tar.bz2` beside the same build, which the standard library reads whole; that
is tried second, so the fetch works with nothing but Python and, when only the
`.conda` is there, with `zstandard` (or the `zstd` command) available.

Usage:
    python3 tools/ci_fetch_dxc110.py --dir "$RUNNER_TEMP/dxc110"
    # prints the directory on stdout; with --github-output, also sets `dir`.

Exit status is 0 only when the compiler and its signer are both in place and the
compiler's own version string says 1.10.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.error
import urllib.request
import zipfile

CHANNEL = "https://conda.anaconda.org/conda-forge/linux-64"

# The builds CI pins. A DXC that signs and an assembler for the SPIR-V side: the
# versions are part of the cache's stamp (`dxc-<version>`), so moving one must
# move the stamp and invalidate the shaders.
DXC = "directx-shader-compiler-1.10.2605.37-h60be90b_0"
SPIRV_TOOLS = "spirv-tools-2026.4-h7148c6a_2"

# What the bake needs out of each archive, and what a successful fetch proves.
DXC_LIBS = ("libdxcompiler.so", "libdxil.so")
SPIRV_BINS = ("spirv-val",)


def log(message: str) -> None:
    print(f"ci_fetch_dxc110: {message}", file=sys.stderr)


def fetch(url: str, path: str, attempts: int = 3) -> None:
    """Download `url` to `path`, retrying on the network errors a runner has."""
    if url.startswith("file://") and not os.path.exists(url[len("file://"):]):
        raise FileNotFoundError(url)  # a mirror on disk, as the tests use
    for attempt in range(1, attempts + 1):
        try:
            with urllib.request.urlopen(url, timeout=180) as response, open(path, "wb") as out:
                shutil.copyfileobj(response, out)
            return
        except urllib.error.HTTPError as error:
            if error.code == 404:
                raise FileNotFoundError(url) from error
            log(f"{url}: HTTP {error.code} (attempt {attempt} of {attempts})")
        except (urllib.error.URLError, TimeoutError) as error:
            log(f"{url}: {error} (attempt {attempt} of {attempts})")
        if attempt != attempts:
            time.sleep(5 * attempt)
    raise RuntimeError(f"could not download {url}")


def sha256(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def zstd_decompress(data: bytes) -> bytes:
    """Decompress one zstd frame, with the module or the command, whichever is here."""
    try:
        import zstandard  # type: ignore[import-not-found]
    except ImportError:
        exe = shutil.which("zstd")
        if exe is None:
            raise RuntimeError("a .conda archive needs zstandard (pip install zstandard) or the zstd command")
        return subprocess.run([exe, "-d", "-c", "-q"], input=data, check=True, capture_output=True).stdout
    return zstandard.ZstdDecompressor().decompress(data, max_output_size=1 << 30)


def extract_all(archive: tarfile.TarFile, dest: str) -> None:
    """Extract, with 3.12's sanitising filter when the interpreter has it."""
    if hasattr(tarfile, "tar_filter"):
        archive.extractall(dest, filter="tar")
    else:
        archive.extractall(dest)


def extract_conda(path: str, dest: str) -> None:
    """Extract a `.conda` (a zip of zstd tars): the package's own tar, not the info."""
    with zipfile.ZipFile(path) as archive:
        inner = [name for name in archive.namelist() if name.startswith("pkg-") and name.endswith(".tar.zst")]
        if len(inner) != 1:
            raise RuntimeError(f"{path}: expected one pkg-*.tar.zst, found {len(inner)}")
        raw = zstd_decompress(archive.read(inner[0]))
    with tarfile.open(fileobj=io.BytesIO(raw), mode="r:") as tar:
        extract_all(tar, dest)


def extract_tarbz2(path: str, dest: str) -> None:
    with tarfile.open(path, mode="r:bz2") as tar:
        extract_all(tar, dest)


def package(name: str, downloads: str) -> str:
    """Download `name` (the build, without an extension) and extract it under `downloads`."""
    work = os.path.join(downloads, name)
    os.makedirs(work, exist_ok=True)
    for extension, extract in ((".conda", extract_conda), (".tar.bz2", extract_tarbz2)):
        archive = os.path.join(downloads, name + extension)
        if not os.path.exists(archive):
            try:
                log(f"fetching {name}{extension}")
                fetch(f"{CHANNEL}/{name}{extension}", archive)
            except FileNotFoundError:
                log(f"not published: {name}{extension}")
                continue
        log(f"{os.path.basename(archive)}: sha256 {sha256(archive)}")
        try:
            extract(archive, work)
        except (RuntimeError, tarfile.TarError, zipfile.BadZipFile) as error:
            # A .conda that this machine cannot open (no zstd) is not a reason to
            # stop: the .tar.bz2 beside it reads with the standard library.
            log(f"{os.path.basename(archive)}: {error}")
            continue
        return work
    raise RuntimeError(f"conda-forge has neither a usable {name}.conda nor {name}.tar.bz2")


def install(extracted: str, target: str, names: tuple[str, ...], subdir: str) -> list[str]:
    """Copy `names` out of `extracted` into `target`/`subdir`, following the archive's own links."""
    out_dir = os.path.join(target, subdir)
    os.makedirs(out_dir, exist_ok=True)
    found = []
    for name in names:
        source = os.path.join(extracted, subdir, name)
        if not os.path.exists(source):
            # A versioned soname with an unversioned link: take the link's target
            # when the archive kept only the file, and the file when it kept only
            # the link.
            candidates = [
                entry for entry in os.listdir(os.path.join(extracted, subdir))
                if entry == name or entry.startswith(name + ".")
            ]
            if not candidates:
                raise RuntimeError(f"{os.path.join(extracted, subdir)} has no {name}")
            source = os.path.join(extracted, subdir, sorted(candidates)[-1])
        destination = os.path.join(out_dir, name)
        if os.path.lexists(destination):
            os.remove(destination)
        shutil.copyfile(source, destination, follow_symlinks=True)
        os.chmod(destination, 0o755)
        found.append(destination)
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dir", required=True, help="where the compiler and the tools go")
    parser.add_argument("--force", action="store_true", help="fetch again even when the directory is full")
    parser.add_argument("--github-output", action="store_true", help="also set `dir` for the next step")
    parser.add_argument("--with-spirv-tools", action="store_true", default=True, help=argparse.SUPPRESS)
    arguments = parser.parse_args()

    target = os.path.abspath(arguments.dir)
    wanted = [os.path.join(target, "lib", name) for name in DXC_LIBS]
    if arguments.with_spirv_tools:
        wanted.append(os.path.join(target, "bin", SPIRV_BINS[0]))
    if all(os.path.exists(path) for path in wanted) and not arguments.force:
        log(f"already in {target}")
    else:
        with tempfile.TemporaryDirectory(prefix="dxc110-") as downloads:
            dxc = package(DXC, downloads)
            for path in install(dxc, target, DXC_LIBS, "lib"):
                log(f"{path}: {os.path.getsize(path)} bytes")
            if arguments.with_spirv_tools:
                try:
                    tools = package(SPIRV_TOOLS, downloads)
                except RuntimeError as error:
                    log(f"spirv-tools not fetched: {error}")
                else:
                    install(tools, target, SPIRV_BINS, "bin")

    # What a self-test needs to be true, checked here so that a wrong build is a
    # fetch failure with a name, not a bake that signs nothing an hour later.
    compiler = os.path.join(target, "lib", DXC_LIBS[0])
    signer = os.path.join(target, "lib", DXC_LIBS[1])
    for path in (compiler, signer):
        if not os.path.exists(path):
            log(f"missing {path}")
            return 1
    if os.path.getsize(compiler) < (4 << 20):
        log(f"{compiler} is {os.path.getsize(compiler)} bytes: not a DXC build")
        return 1
    with open(compiler, "rb") as f:
        blob = f.read()
    # `--version`'s string is baked into the library; the console's stamp reads it.
    if b"1.10." not in blob:
        log(f"{compiler} carries no 1.10 version string")
        return 1
    validator = os.path.join(target, "bin", SPIRV_BINS[0])
    if arguments.with_spirv_tools and os.path.exists(validator):
        try:
            version = subprocess.run([validator, "--version"], capture_output=True, text=True, timeout=60)
            log(f"{validator}: {(version.stdout or version.stderr).strip()}")
        except (OSError, subprocess.SubprocessError) as error:
            log(f"{validator}: {error}")

    print(target)
    if arguments.github_output:
        output = os.environ.get("GITHUB_OUTPUT")
        if output:
            with open(output, "a", encoding="utf-8") as f:
                f.write(f"dir={target}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
