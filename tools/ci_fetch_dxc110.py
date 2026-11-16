#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fetch a signing DXC 1.10 (and spirv-tools) into a directory.

The prebaked cache is usable only if every blob in it is signed, and whether a
given `libdxcompiler.so` signs is a property of that build: its `libdxil.so`
must sit beside it, or DXC returns a DXBC container whose digest is zeros and the
console refuses every shader an hour of baking produced. So the bake does not
take a compiler on trust - `bbx_bake_cache --check-compiler` reports what the one
in hand does - and CI fetches a build that is known to sign on every push, so
the gate is exercised rather than remembered.

The source is Microsoft's own release tarball, pinned by tag and SHA-256.
conda-forge packages the same DXC for conda, but that build ships no signer (the
`directx-shader-compiler-1.10.2605.37` linux package has `libdxcompiler.so` and
no `libdxil.so`), which is exactly the failure this script exists to keep out of
the bake. spirv-tools comes from conda-forge, and is optional: the bake does not
need it, and a channel that stops publishing it must not stop the compiler.

Usage:
    python3 tools/ci_fetch_dxc110.py --dir "$RUNNER_TOOL_CACHE/dxc110"
    # prints the directory on stdout; with --github-output, also sets `dir`.

Exit status is 0 only when the compiler and its signer are both in place.
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
import urllib.parse
import urllib.request
import zipfile

# The release CI pins: tag, asset, and the digest GitHub reports for the asset.
# Moving either moves the cache's stamp (`dxc-<version>`) and invalidates shaders.
DXC_VERSION = "1.10.2605.37"
DXC_URL = (
    "https://github.com/microsoft/DirectXShaderCompiler/releases/download/"
    f"v{DXC_VERSION}/linux_dxc_2026_08_11.x86_64.tar.gz"
)
DXC_SHA256 = "dea02d61c0548a6163a5fb45daa9263faeb5334e35d25a30b0293a32d089ceae"

SPIRV_TOOLS_CHANNEL = "https://conda.anaconda.org/conda-forge/linux-64"
SPIRV_TOOLS = "spirv-tools-2026.4-h7148c6a_2"

# What the bake needs, and where it goes in the fetched directory.
DXC_LIBS = ("libdxcompiler.so", "libdxil.so")
SPIRV_TOOLS_BINS = ("spirv-val",)


def log(message: str) -> None:
    print(f"ci_fetch_dxc110: {message}", file=sys.stderr)


def fetch(url: str, path: str, attempts: int = 3) -> None:
    """Download `url` to `path`, retrying the network errors a runner has."""
    if url.startswith("file://") and not os.path.exists(url[len("file://"):]):
        raise FileNotFoundError(url)  # a mirror on disk, as the tests use
    request = urllib.request.Request(url, headers={"User-Agent": "bbxbox-ci"})
    for attempt in range(1, attempts + 1):
        try:
            with urllib.request.urlopen(request, timeout=300) as response, open(path, "wb") as out:
                shutil.copyfileobj(response, out)
            return
        except urllib.error.HTTPError as error:
            if error.code == 404:
                raise FileNotFoundError(url) from error
            log(f"{urllib.parse.urlsplit(url).netloc}: HTTP {error.code} (attempt {attempt} of {attempts})")
        except (urllib.error.URLError, TimeoutError, OSError) as error:
            log(f"{os.path.basename(url)}: {error} (attempt {attempt} of {attempts})")
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
    """One zstd frame, with the module or the command, whichever is here."""
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


def extract(path: str, dest: str) -> None:
    """Extract a release tarball, a plain zip, or a conda package, by its name."""
    if path.endswith((".tar.gz", ".tgz")):
        with tarfile.open(path, mode="r:gz") as tar:
            extract_all(tar, dest)
    elif path.endswith(".tar.bz2"):
        with tarfile.open(path, mode="r:bz2") as tar:
            extract_all(tar, dest)
    elif path.endswith(".conda"):
        # A .conda is a zip of zstd-compressed tars: the package's own tar, not
        # the info one.
        with zipfile.ZipFile(path) as package:
            inner = [n for n in package.namelist() if n.startswith("pkg-") and n.endswith(".tar.zst")]
            if len(inner) != 1:
                raise RuntimeError(f"{path}: expected one pkg-*.tar.zst, found {len(inner)}")
            raw = zstd_decompress(package.read(inner[0]))
        with tarfile.open(fileobj=io.BytesIO(raw), mode="r:") as tar:
            extract_all(tar, dest)
    elif path.endswith(".zip"):
        with zipfile.ZipFile(path) as package:
            package.extractall(dest)
    else:
        raise RuntimeError(f"{path}: no extractor for this archive")


def locate(extracted: str, name: str) -> str:
    """The file called `name` (or its versioned form) anywhere under `extracted`."""
    plain = []
    versioned = []
    for root, _, names in os.walk(extracted):
        for entry in names:
            if entry == name:
                plain.append(os.path.join(root, entry))
            elif entry.startswith(name + "."):
                versioned.append(os.path.join(root, entry))
    for candidate in plain + sorted(versioned):
        if os.path.isfile(candidate):  # follows the archive's own links
            return candidate
    raise RuntimeError(f"{name} is not in the archive (found: {', '.join(sorted(_listing(extracted))[:12])})")


def _listing(extracted: str) -> list[str]:
    out = []
    for root, _, names in os.walk(extracted):
        for entry in names:
            out.append(os.path.relpath(os.path.join(root, entry), extracted))
    return out


def install(extracted: str, target: str, names: tuple[str, ...], subdir: str) -> list[str]:
    """Copy `names` out of `extracted` into `target`/`subdir`, as real files."""
    out_dir = os.path.join(target, subdir)
    os.makedirs(out_dir, exist_ok=True)
    found = []
    for name in names:
        source = locate(extracted, name)
        destination = os.path.join(out_dir, name)
        if os.path.lexists(destination):
            os.remove(destination)
        shutil.copyfile(source, destination, follow_symlinks=True)
        os.chmod(destination, 0o755)
        found.append(destination)
    return found


def package(name: str, url_base: str, downloads: str) -> str:
    """Download one conda-forge build, trying its `.conda` then its `.tar.bz2`."""
    work = os.path.join(downloads, name)
    os.makedirs(work, exist_ok=True)
    for extension in (".conda", ".tar.bz2"):
        archive = os.path.join(downloads, name + extension)
        if not os.path.exists(archive):
            try:
                log(f"fetching {name}{extension}")
                fetch(f"{url_base}/{name}{extension}", archive)
            except FileNotFoundError:
                log(f"not published: {name}{extension}")
                continue
        log(f"{os.path.basename(archive)}: sha256 {sha256(archive)}")
        try:
            extract(archive, work)
        except (RuntimeError, tarfile.TarError, zipfile.BadZipFile) as error:
            # A .conda this machine cannot open (no zstd) is not a reason to
            # stop: the .tar.bz2 beside it reads with the standard library.
            log(f"{os.path.basename(archive)}: {error}")
            continue
        return work
    raise RuntimeError(f"conda-forge has neither a usable {name}.conda nor a {name}.tar.bz2")


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
        wanted.append(os.path.join(target, "bin", SPIRV_TOOLS_BINS[0]))
    if all(os.path.exists(path) for path in wanted) and not arguments.force:
        log(f"already in {target}")
    else:
        with tempfile.TemporaryDirectory(prefix="dxc110-") as downloads:
            archive = os.path.join(downloads, os.path.basename(DXC_URL))
            log(f"fetching DXC {DXC_VERSION}")
            fetch(DXC_URL, archive)
            digest = sha256(archive)
            if digest != DXC_SHA256:
                raise RuntimeError(f"{os.path.basename(DXC_URL)}: sha256 {digest}, expected {DXC_SHA256}")
            log(f"{os.path.basename(DXC_URL)}: sha256 {digest} (pinned)")
            dxc = os.path.join(downloads, "dxc")
            extract(archive, dxc)
            for path in install(dxc, target, DXC_LIBS, "lib"):
                log(f"{path}: {os.path.getsize(path)} bytes")
            if arguments.with_spirv_tools:
                try:
                    tools = package(SPIRV_TOOLS, SPIRV_TOOLS_CHANNEL, downloads)
                    install(tools, target, SPIRV_TOOLS_BINS, "bin")
                except RuntimeError as error:
                    log(f"spirv-tools not fetched: {error}")

    # What a self-test needs to be true, checked here so that a wrong build is a
    # fetch failure with a name, not a bake that signs nothing an hour later.
    # Whether it *signs* is `bbx_bake_cache --check-compiler`'s answer: it
    # compiles a shader through the bake's own door, which a version string does
    # not prove.
    compiler = os.path.join(target, "lib", DXC_LIBS[0])
    signer = os.path.join(target, "lib", DXC_LIBS[1])
    for path in (compiler, signer):
        if not os.path.exists(path):
            raise RuntimeError(f"missing {path} after the fetch")
    if os.path.getsize(compiler) < (4 << 20):
        raise RuntimeError(f"{compiler} is {os.path.getsize(compiler)} bytes: not a DXC build")
    with open(compiler, "rb") as f:
        blob = f.read()
    if b"1.10." in blob:
        log(f"{compiler}: the 1.10 version string is in the library")
    else:
        log(f"{compiler}: WARNING no '1.10.' string in the library; --check-compiler decides")
    validator = os.path.join(target, "bin", SPIRV_TOOLS_BINS[0])
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


def annotate(message: str) -> None:
    """A failure a runner's log may not be reachable for: say it in the annotation."""
    if os.environ.get("GITHUB_ACTIONS") == "true":
        encoded = message.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
        print(f"::error title=ci_fetch_dxc110::{encoded}", file=sys.stderr)
    log(message)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:  # noqa: BLE001 - a fetch failure is one failure
        annotate(f"{type(error).__name__}: {error}")
        sys.exit(1)
