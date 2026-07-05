#!/usr/bin/python
"""Discover upstream detached GPG signatures for package tarballs and pin
them into PKG_SOURCE_SIG/PKG_VALIDPGPKEYS, reusing the existing
download.mk/fetch-gpg-key.sh/check-gpg-key.sh machinery to do the actual
fetching, discovery and key confirmation.
"""

import argparse
import concurrent.futures
import os
import re
import signal
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SCRIPT_DIR = REPO_ROOT / "scripts"
SIGNED_OFF_BY = "John Audia <therealgraysky@proton.me>"

# Ordered by how common each convention is across the ecosystems this tree
# pulls from (GNU/Savannah/SF favor .sig; kernel.org/e2fsprogs sign the
# uncompressed tarball with .sign).
SIG_PATTERNS = [
    ("{source}.sig", "$(PKG_SOURCE).sig"),
    ("{source}.asc", "$(PKG_SOURCE).asc"),
    ("{base}.sign", "$(basename $(PKG_SOURCE)).sign"),
    ("{base}.sig", "$(basename $(PKG_SOURCE)).sig"),
    ("{base}.asc", "$(basename $(PKG_SOURCE)).asc"),
    ("{source}.sign", "$(PKG_SOURCE).sign"),
]

FINGERPRINT_RE = re.compile(r"^[0-9A-F]{40}$")


def gnu_basename(name):
    # GNU Make's $(basename ...) strips only the last dot-suffix.
    return name.rsplit(".", 1)[0] if "." in name else name


class CompletedProcess:
    def __init__(self, returncode, stdout, stderr):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


def run_cmd(cmd, timeout, cwd=None, env=None):
    """Like subprocess.run(capture_output=True, text=True, timeout=...), but
    kills the *entire* process group on timeout instead of just the direct
    child.

    `make`/`download.pl` fork curl several levels down (make -> sh -> perl
    -> curl). A stalled connection (e.g. downloads.sourceforge.net under
    concurrent load) means curl can hang indefinitely since --connect-timeout
    only bounds the initial TCP handshake, not the transfer. subprocess.run's
    own timeout handling only kills the immediate child (make); the orphaned
    curl grandchild keeps the inherited stderr pipe open, so the follow-up
    communicate() call it makes to drain output blocks forever waiting for
    EOF that never comes - permanently wedging the calling thread even
    though a timeout was specified. Running in our own process group and
    killing that whole group on timeout avoids both problems.
    """
    proc = subprocess.Popen(
        cmd, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, start_new_session=True,
    )
    try:
        stdout, stderr = proc.communicate(timeout=timeout)
        return CompletedProcess(proc.returncode, stdout, stderr)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        stdout, stderr = proc.communicate()
        return CompletedProcess(124, stdout, (stderr or "") + "\n[timed out]")


def make_val(pkgdir, var, timeout=20):
    """Query one Makefile variable via rules.mk's `val.%` pattern rule.
    Returns None if the variable is undefined, "" if defined-but-empty.
    """
    proc = run_cmd(
        ["make", "--no-print-directory", "-C", str(pkgdir), f"TOPDIR={REPO_ROOT}",
         f"val.{var}", "V=s"],
        timeout=timeout,
    )
    if proc.returncode == 124 or f"{var} undefined" in proc.stderr:
        return None
    return proc.stdout.rstrip("\n")


def run_make(pkgdir, targets, extra_vars=None, timeout=20):
    # `make -C <pkgdir>` (not `make <pkgdir>/download` from the repo root)
    # deliberately skips toplevel.mk's global host-prereq gate, which this
    # tool has no business enforcing.
    cmd = ["make", "--no-print-directory", "-C", str(pkgdir), f"TOPDIR={REPO_ROOT}",
           "CONFIG_DOWNLOAD_VERIFY_SIGNATURES=y"]
    cmd += list(targets)
    for k, v in (extra_vars or {}).items():
        cmd.append(f"{k}={v}")
    cmd.append("V=99")
    return run_cmd(cmd, timeout=timeout)


def ensure_mkhash():
    staging = make_val(REPO_ROOT, "STAGING_DIR_HOST") or "staging_dir/host"
    mkhash = Path(staging) / "bin" / "mkhash"
    if not mkhash.is_absolute():
        mkhash = REPO_ROOT / mkhash
    if mkhash.exists():
        return
    print(" >>> mkhash host tool missing, building it via `make prereq FORCE=1`...")
    subprocess.run(["make", "-C", str(REPO_ROOT), "prereq", "FORCE=1"], timeout=600)
    if not mkhash.exists():
        print(" >>> WARNING: mkhash still missing; downloads needing hash checks may fail.",
              file=sys.stderr)


def find_candidates(root):
    candidates = []
    for makefile in sorted(root.rglob("Makefile")):
        text = makefile.read_text(errors="replace")
        if not re.search(r"^\s*PKG_SOURCE_URL\s*:?=", text, re.M):
            continue
        if re.search(r"^\s*PKG_SOURCE_SIG\s*:?=", text, re.M):
            continue
        candidates.append(makefile)
    return candidates


def insert_sig_lines(text, sig_expr):
    lines = text.splitlines(keepends=True)
    hash_idx = None
    for i, line in enumerate(lines):
        if re.match(r"^\s*PKG_HASH\s*:?=", line):
            hash_idx = i
    if hash_idx is None:
        raise RuntimeError("no PKG_HASH assignment found")

    insert_at = hash_idx + 1
    new_lines = []
    if insert_at < len(lines) and lines[insert_at].strip() == "":
        insert_at += 1
    else:
        new_lines.append("\n")
    new_lines.append(f"PKG_SOURCE_SIG:={sig_expr}\n")
    new_lines.append("PKG_VALIDPGPKEYS:=skip\n")
    new_lines.append("\n")
    return "".join(lines[:insert_at] + new_lines + lines[insert_at:])


def probe_signature(urls, sigfile, timeout=20):
    with tempfile.TemporaryDirectory() as tmp:
        proc = run_cmd(
            ["perl", str(SCRIPT_DIR / "download.pl"), tmp, sigfile, "skip", sigfile, *urls],
            cwd=REPO_ROOT, timeout=timeout,
            env={**os.environ, "TOPDIR": str(REPO_ROOT)},
        )
        fetched = Path(tmp) / sigfile
        if proc.returncode != 0 or not fetched.exists() or fetched.stat().st_size == 0:
            return False
        gpg = subprocess.run(["gpg", "--batch", "--list-packets", str(fetched)],
                              capture_output=True, text=True, timeout=15)
        return gpg.returncode == 0 and "signature packet" in gpg.stdout


def derive_commit_prefix(pkgdir, pkg_name):
    rel_parts = pkgdir.relative_to(REPO_ROOT).parts
    if rel_parts[0] == "toolchain":
        return f"toolchain/{pkgdir.name}"
    if rel_parts[0] == "tools":
        return f"tools/{pkgdir.name}"
    return pkg_name or pkgdir.name


def find_git_root(path):
    """Walk up from `path` to find its enclosing git repo. Feed packages
    (feeds/<name>/...) live in their own clone, separate from and ignored
    by the main tree's .git - commits for those must land there, not in
    the outer repo.
    """
    for candidate in (path, *path.parents):
        if (candidate / ".git").exists():
            return candidate
    return None


def commit_message(prefix):
    return (
        f"{prefix}: add gpg verification of source data\n\n"
        "Add signature and key fingerprint variables\n\n"
        f"Signed-off-by: {SIGNED_OFF_BY}\n"
    )


def skip(reason):
    return {"status": "skipped", "detail": reason}


def failed(reason):
    return {"status": "failed", "detail": reason}


def tail(text, n=15):
    return "\n".join(text.strip().splitlines()[-n:])


def process_package(makefile, commit, git_lock, timeout=20):
    pkgdir = makefile.parent
    original_text = makefile.read_text()

    if len(re.findall(r"^\s*PKG_HASH\s*:?=", original_text, re.M)) != 1:
        return skip("multiple PKG_HASH assignments (per-version blocks) - "
                     "needs manual key pinning, see toolchain/gcc or toolchain/binutils")

    proto = make_val(pkgdir, "PKG_SOURCE_PROTO", timeout=timeout)
    if proto == "git":
        return skip("git-based source, no detached signature to pin")

    pkg_name = make_val(pkgdir, "PKG_NAME", timeout=timeout)
    source = make_val(pkgdir, "PKG_SOURCE", timeout=timeout)
    url = make_val(pkgdir, "PKG_SOURCE_URL", timeout=timeout)
    if not source or not url:
        return skip("PKG_SOURCE or PKG_SOURCE_URL not resolvable")

    urls = url.split()
    base = gnu_basename(source)
    seen = set()
    sig_expr = None
    for pattern, expr in SIG_PATTERNS:
        candidate = pattern.format(source=source, base=base)
        if candidate in seen:
            continue
        seen.add(candidate)
        if probe_signature(urls, candidate, timeout=timeout):
            sig_expr = expr
            break

    if sig_expr is None:
        return skip("no signature file found for any known naming convention")

    makefile.write_text(insert_sig_lines(original_text, sig_expr))

    dl_dir = Path(make_val(pkgdir, "DL_DIR", timeout=timeout) or (REPO_ROOT / "dl"))
    sigfile = sig_expr.replace("$(PKG_SOURCE)", source).replace(
        "$(basename $(PKG_SOURCE))", base)

    def revert():
        makefile.write_text(original_text)

    proc = run_make(pkgdir, ["download"], timeout=timeout)
    if proc.returncode != 0:
        revert()
        return failed(f"download (PKG_VALIDPGPKEYS=skip) failed:\n{tail(proc.stdout + proc.stderr)}")

    proc = run_make(pkgdir, ["check"], extra_vars={"FIXUP": "1"}, timeout=timeout)
    if proc.returncode != 0:
        revert()
        return failed(f"check FIXUP=1 failed:\n{tail(proc.stdout + proc.stderr)}")

    fingerprint = make_val(pkgdir, "PKG_VALIDPGPKEYS", timeout=timeout)
    if not fingerprint or not FINGERPRINT_RE.match(fingerprint):
        revert()
        return failed(f"key discovery did not confirm a fingerprint (got {fingerprint!r}):\n"
                       f"{tail(proc.stdout + proc.stderr)}")

    for name in (source, sigfile):
        f = dl_dir / name
        if f.exists():
            f.unlink()

    proc = run_make(pkgdir, ["download"], timeout=timeout)
    if proc.returncode != 0:
        revert()
        return failed(f"clean re-verify of pinned key {fingerprint} failed:\n"
                       f"{tail(proc.stdout + proc.stderr)}")

    prefix = derive_commit_prefix(pkgdir, pkg_name)
    message = commit_message(prefix)
    new_text = makefile.read_text()

    if not commit:
        revert()
        return {
            "status": "verified (dry-run)",
            "detail": f"fingerprint {fingerprint}, sig {sig_expr}",
            "message": message,
            "diff_preview": new_text,
        }

    git_root = find_git_root(pkgdir)
    if git_root is None:
        revert()
        return failed(f"no enclosing git repo found for {pkgdir}")

    with git_lock:
        subprocess.run(["git", "-C", str(git_root), "add", str(makefile.relative_to(git_root))],
                        check=True)
        subprocess.run(["git", "-C", str(git_root), "commit", "-F", "-"],
                        input=message, text=True, check=True, cwd=git_root)
    return {"status": "committed", "detail": f"fingerprint {fingerprint}, sig {sig_expr}",
            "message": message}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("roots", nargs="+", type=Path,
                         help="directories to scan recursively for candidate Makefiles "
                              "(e.g. package/ toolchain/ tools/)")
    parser.add_argument("--commit", action="store_true",
                         help="commit each success; default is dry-run (verify, then revert)")
    parser.add_argument("--limit", type=int, help="stop after N candidates")
    parser.add_argument("--only", help="only process candidates whose path contains this substring")
    parser.add_argument("--parallel", type=int, default=1, metavar="N",
                         help="process N candidates concurrently (default: 1, sequential). "
                              "Git commits are still serialized to avoid index-lock races.")
    parser.add_argument("--timeout", type=int, default=20, metavar="SECONDS",
                         help="per-subprocess timeout for make/download/probe calls "
                              "(default: 20). Raise this if larger packages start failing "
                              "with timeouts, especially at high --parallel where bandwidth "
                              "is shared across concurrent downloads.")
    args = parser.parse_args()

    if args.parallel < 1:
        parser.error("--parallel must be >= 1")
    if args.timeout < 1:
        parser.error("--timeout must be >= 1")

    ensure_mkhash()

    candidates = []
    for root in args.roots:
        candidates.extend(find_candidates(root.resolve()))
    candidates = sorted(set(candidates))
    if args.only:
        candidates = [c for c in candidates if args.only in str(c)]
    if args.limit:
        candidates = candidates[:args.limit]

    total = len(candidates)
    print(f" >>> {total} candidate package(s) to process\n", flush=True)

    git_lock = threading.Lock()
    print_lock = threading.Lock()
    progress = {"done": 0}

    def process_one(makefile):
        rel = makefile.relative_to(REPO_ROOT)
        result = process_package(makefile, commit=args.commit, git_lock=git_lock, timeout=args.timeout)
        result["package"] = str(rel)
        with print_lock:
            progress["done"] += 1
            print(f"--- [{progress['done']}/{total}] {rel} ---")
            print(f"  {result['status']}: {result['detail']}\n", flush=True)
        return result

    if args.parallel == 1:
        results = [process_one(makefile) for makefile in candidates]
    else:
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.parallel) as pool:
            results = list(pool.map(process_one, candidates))

    counts = {}
    for r in results:
        counts[r["status"]] = counts.get(r["status"], 0) + 1
    print(" >>> summary:")
    for status, count in sorted(counts.items()):
        print(f"      {status}: {count}")


if __name__ == "__main__":
    main()
