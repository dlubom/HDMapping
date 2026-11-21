import hashlib
import json
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading


def await_notice(child, expected):
    result = queue.Queue()
    def listen():
        for line in child.stdout:
            if line.strip() == expected:
                result.put(expected)
                return
        result.put("child exited before " + expected)

    reader = threading.Thread(target=listen, daemon=True)
    reader.start()
    assert result.get(timeout=20) == expected


def fingerprints(root):
    return {
        path.relative_to(root): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in root.rglob("*")
        if path.is_file()
    }


def main(executable, root):
    def run(*args, check=True):
        result = subprocess.run(
            [executable, *args], text=True, encoding="utf-8", errors="replace",
            capture_output=True, timeout=25,
        )
        if check and result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        return result

    def start(*args):
        return subprocess.Popen(
            [executable, *args], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace",
        )

    def read(destination):
        lines = run("--read", destination).stdout.splitlines()
        return json.loads(next(line[len("READOUT "):] for line in lines if line.startswith("READOUT ")))

    archive = root / "archive"
    updated = root / "updated"
    run("--make-input", archive, "7")
    run("--make-input", updated, "13")
    source_hashes = fingerprints(archive)
    updated_hashes = fingerprints(updated)
    destination = root / "published"
    session = archive / "session.mjs"
    run("--publish", session, destination, "original")
    first = read(destination)
    assert first["dependencies"] == 2 and first["first_pose_x"] == 7
    assert first["scans"] == first["points"] == first["trajectory_nodes"] == 1
    original_files = fingerprints(destination)

    child = start("--pause-after-poses", updated / "session.mjs", destination, "interrupted", "original")
    try:
        await_notice(child, "POSES_WRITTEN")
        partial = root / ".hdmapping-result-stage-interrupted.preparing"
        assert (partial / "poses.mrp").is_file()
        assert not (partial / "snapshot.json").exists()
    finally:
        child.kill()
        child.communicate(timeout=20)
    assert fingerprints(destination) == original_files
    assert read(destination) == first
    run("--cleanup", root, "interrupted", destination)
    assert not partial.exists()

    child = start("--pause-before-head", updated / "session.mjs", destination, "pending", "original")
    try:
        await_notice(child, "READY_TO_COMMIT")
        abandoned = destination / "result-pending"
        assert (abandoned / "snapshot.json").is_file()
        assert read(destination) == first
    finally:
        child.kill()
        child.communicate(timeout=20)
    wrong = run("--cleanup", root, "pending", root / "unrelated", check=False)
    assert wrong.returncode != 0 and "different destination" in wrong.stderr
    assert abandoned.exists()
    run("--cleanup", root, "pending", destination)
    assert not abandoned.exists()
    assert fingerprints(destination) == original_files

    children = [
        start("--race-publish", updated / "session.mjs", destination, generation, "original")
        for generation in ("concurrent-a", "concurrent-b")
    ]
    try:
        for child in children:
            await_notice(child, "READY")
        for child in children:
            child.stdin.write("start\n")
            child.stdin.flush()
        outputs = [child.communicate(timeout=20)[0] for child in children]
    finally:
        for child in children:
            if child.poll() is None:
                child.kill()
                child.communicate(timeout=20)
    assert sorted(child.returncode for child in children) == [0, 1], outputs
    assert any("publication_conflict" in output for output in outputs), outputs
    winner = read(destination)
    assert winner["artifact_id"] in ("concurrent-a", "concurrent-b")
    assert winner["first_pose_x"] == 13
    run("--cleanup", root, "original", destination)
    assert fingerprints(destination / "result-original") == {
        path.relative_to("result-original"): sha
        for path, sha in original_files.items() if path.parts[0] == "result-original"
    }

    export = root / "complete"
    run("--export", destination, export)
    assert fingerprints(archive) == source_hashes
    assert fingerprints(updated) == updated_hashes
    assert not list(root.glob(".hdmapping-result-*"))
    archive.rename(root / "archive disconnected")
    updated.rename(root / "updated disconnected")
    assert run("--read", destination, check=False).returncode == 1
    relocated = root / "relocated export"
    export.rename(relocated)
    complete = read(relocated)
    assert complete["dependencies"] == 0
    assert {key: value for key, value in complete.items() if key not in ("entry", "artifact_id", "dependencies")} == {
        key: value for key, value in winner.items() if key not in ("entry", "artifact_id", "dependencies")
    }
    print("Process checks: interrupted before snapshot and commit manifests, one replacement winner, relocated native MJS read after source detachment")


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="hdmapping-publication-process-") as temporary:
        main(sys.argv[1], Path(temporary))
