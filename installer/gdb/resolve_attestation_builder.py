"""Build the exact fixed ABI-v3 attestation for a stopped Resolve inferior.

This module is imported by GDB's embedded Python. It only reads /proc and
mapped files; the returned record is copied into a disposable anonymous
mapping by the GDB probe.
"""

from __future__ import annotations

import hashlib
import os
import secrets
import stat
import struct


PREFLIGHT_MAGIC = 0x4156315052465633
PREFLIGHT_VERSION = 3
PREFLIGHT_SIZE = 192
PREFLIGHT_CHECKS = 47
PREFLIGHT_FLAGS = 0xFF


def _proc_start_ticks(pid: int) -> int:
    with open("/proc/%d/stat" % pid, "r", encoding="ascii") as stream:
        data = stream.read()
    close = data.rfind(")")
    if close < 0:
        raise RuntimeError("malformed /proc/PID/stat command field")
    fields = data[close + 1 :].split()
    if len(fields) <= 19:
        raise RuntimeError("short /proc/PID/stat record")
    return int(fields[19])


def _stable_stat(left: os.stat_result, right: os.stat_result) -> bool:
    return all(
        getattr(left, name) == getattr(right, name)
        for name in (
            "st_dev",
            "st_ino",
            "st_size",
            "st_mtime_ns",
            "st_ctime_ns",
        )
    )


def _hash_descriptor(fd: int, before: os.stat_result) -> tuple[bytes, os.stat_result]:
    digest = hashlib.sha256()
    os.lseek(fd, 0, os.SEEK_SET)
    while True:
        chunk = os.read(fd, 1024 * 1024)
        if not chunk:
            break
        digest.update(chunk)
    after = os.fstat(fd)
    if not _stable_stat(before, after):
        raise RuntimeError("mapped file changed while hashing")
    return digest.digest(), after


def _loaded_file_digest(pid: int) -> bytes:
    entries = {}
    with open("/proc/%d/maps" % pid, "rb", buffering=0) as stream:
        for line in stream:
            fields = line.rstrip(b"\n").split(None, 5)
            if len(fields) < 6:
                continue
            permissions = fields[1]
            device = fields[3].split(b":", 1)
            inode = int(fields[4], 10)
            raw_path = fields[5]
            if (
                permissions[2:3] != b"x"
                or inode == 0
                or not raw_path.startswith(b"/")
            ):
                continue
            if raw_path.endswith(b" (deleted)"):
                raise RuntimeError("deleted executable mapping cannot be attested")

            major, minor = int(device[0], 16), int(device[1], 16)
            previous = entries.get(raw_path)
            if previous is not None:
                if previous[:3] != (major, minor, inode):
                    raise RuntimeError("conflicting mapped identity for one path")
                continue
            if len(entries) >= 256:
                raise RuntimeError("mapped executable image count exceeds 256")

            candidates = [raw_path, raw_path.replace(b"\\012", b"\n")]
            fd = None
            before = None
            for candidate in candidates:
                try:
                    candidate_fd = os.open(
                        candidate, os.O_RDONLY | os.O_CLOEXEC
                    )
                    candidate_stat = os.fstat(candidate_fd)
                except OSError:
                    continue
                if (
                    candidate_stat.st_ino == inode
                    and stat.S_ISREG(candidate_stat.st_mode)
                ):
                    fd, before = candidate_fd, candidate_stat
                    break
                os.close(candidate_fd)

            if fd is None:
                raise RuntimeError("cannot open mapped executable image by inode")
            try:
                if os.pread(fd, 4, 0) != b"\x7fELF":
                    continue
                file_hash, final_stat = _hash_descriptor(fd, before)
                entries[raw_path] = (
                    major,
                    minor,
                    inode,
                    file_hash,
                    fd,
                    final_stat,
                )
                fd = None
            finally:
                if fd is not None:
                    os.close(fd)

    if not entries:
        raise RuntimeError("no mapped executable ELF images found")

    aggregate = hashlib.sha256()
    try:
        for raw_path in sorted(entries):
            major, minor, inode, file_hash, fd, final_stat = entries[raw_path]
            aggregate.update(len(raw_path).to_bytes(4, "big"))
            aggregate.update(raw_path)
            aggregate.update(major.to_bytes(4, "big"))
            aggregate.update(minor.to_bytes(4, "big"))
            aggregate.update(inode.to_bytes(8, "big"))
            aggregate.update(file_hash)
            if not _stable_stat(final_stat, os.fstat(fd)):
                raise RuntimeError(
                    "mapped file identity changed before digest completion"
                )
    finally:
        for entry in entries.values():
            os.close(entry[4])
    return aggregate.digest()


def build_preflight_attestation(
    pid: int,
    generic_map: int,
    av1_callback: int,
    operator_new: int,
    sized_delete: int,
    scalable_malloc: int,
    safe_free: int,
) -> bytes:
    """Return one packed PreflightAttestation for the current stopped inferior."""
    if pid <= 0:
        raise ValueError("invalid inferior PID")
    fields = (
        generic_map,
        av1_callback,
        operator_new,
        sized_delete,
        scalable_malloc,
        safe_free,
    )
    if any(value <= 0 or value >= (1 << 64) for value in fields):
        raise ValueError("attested target pointer is out of range")

    with open("/proc/%d/exe" % pid, "rb") as stream:
        executable_digest = hashlib.sha256(stream.read()).digest()
    image_set_digest = _loaded_file_digest(pid)
    thread_count = len(os.listdir("/proc/%d/task" % pid))
    nonce = secrets.token_bytes(16)

    header = struct.pack(
        "<QIIIIIIQ",
        PREFLIGHT_MAGIC,
        PREFLIGHT_VERSION,
        PREFLIGHT_SIZE,
        PREFLIGHT_CHECKS,
        PREFLIGHT_CHECKS,
        0,
        0,
        PREFLIGHT_FLAGS,
    )
    session = struct.pack(
        "<QQII16sQQ32s32sQQQQ",
        pid,
        _proc_start_ticks(pid),
        thread_count,
        0,
        nonce,
        generic_map,
        av1_callback,
        executable_digest,
        image_set_digest,
        operator_new,
        sized_delete,
        scalable_malloc,
        safe_free,
    )
    record = header + session
    if len(record) != PREFLIGHT_SIZE:
        raise RuntimeError("internal ABI-v3 record size mismatch")
    return record
