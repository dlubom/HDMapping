"""Exercise the public MCP tools against three real GUI owners under Xvfb."""
import json
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile
import time


def check(binaries, directory):
    env = os.environ | {"HDMAPPING_MCP_STATE_DIR": str(directory / "registry")}
    # The coordinator normally outlives its callers. Own its process in this test.
    coordinator = subprocess.Popen(
        [binaries / "hdmapping_coordinator", "serve", env["HDMAPPING_MCP_STATE_DIR"]],
        env=env, start_new_session=True,
    )
    adapter = subprocess.Popen(
        [binaries / "hdmapping_mcp"], env=env,
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
    )
    windows = []
    sequence = 0

    def request(method, params):
        nonlocal sequence
        sequence += 1
        print(json.dumps({"jsonrpc": "2.0", "id": sequence, "method": method, "params": params}),
              file=adapter.stdin, flush=True)
        if not select.select([adapter.stdout], [], [], 30)[0]:
            raise TimeoutError(method)
        response = json.loads(adapter.stdout.readline())
        assert response["id"] == sequence and "error" not in response, response
        return response["result"]

    def call(name, arguments):
        return request("tools/call", {"name": name, "arguments": arguments})

    try:
        request("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                               "clientInfo": {"name": "gui-contract", "version": "1"}})
        print('{"jsonrpc":"2.0","method":"notifications/initialized"}', file=adapter.stdin, flush=True)
        archive = directory / "recording"
        subprocess.run([binaries / "processing_publication_test", "--make-input", archive, "7"], check=True)
        session = archive / "session.mjs"
        cases = [
            ("step1", "lidar_odometry_step_1", [], {"nr_iter": 37}),
            ("step2", "multi_view_tls_registration_step_2", [session], {"ndt": {"number_of_iterations": 37}}),
            ("step3", "multi_session_registration_step_3", [], {"ndt_iterations": 37}),
            ("step2", "multi_view_tls_registration_step_2", [session], {"icp": {"number_of_iterations": 39}}),
        ]
        ids = set()
        for step, program, args, parameters in cases:
            child = subprocess.Popen([binaries / program, *args], env=env, cwd=directory, stdout=subprocess.DEVNULL)
            windows.append(child)
            deadline = time.monotonic() + 45
            while time.monotonic() < deadline:
                assert child.poll() is None, (program, child.returncode)
                states = call("list_open_states", {})["structuredContent"]["states"]
                opening = next((s for s in states if s["state_id"] not in ids), None)
                if opening:
                    break
                time.sleep(0.1)
            else:
                raise TimeoutError(program)
            ids.add(opening["state_id"])
            update = {"step": step, "target": opening, "parameters": parameters}
            changed = call("set_state_parameters", update)["structuredContent"]["target"]
            assert changed["state_id"] == opening["state_id"] and changed["revision"] > opening["revision"]
            assert call("set_state_parameters", update)["isError"]
            # Both requests travel through later GUI frame dispatches.
            for _ in range(2):
                model = call("describe_state", {"target": changed})["structuredContent"]["parameters"]
                for key, expected in parameters.items():
                    if isinstance(expected, dict):
                        assert all(model[key][k] == v for k, v in expected.items()), model[key]
                    else:
                        assert model[key] == expected, model[key]
            print(program, opening, changed, parameters, flush=True)
        # Exercise the Step 2 owner, durable snapshot, worker and auto-apply
        # through the same MCP calls as an external client.
        control = [
            {"name": str(i), "scan_index": 0, "local_m": local,
             "target_m": [local[0] + 8, local[1] - 2, local[2] + 3],
             "sigma_m": [0.01, 0.02, 0.03]}
            for i, local in enumerate(([0, 0, 0], [1, 0, 0], [0, 1, 0]))
        ]
        added = call("cp.import", {"target": changed, "parameters": {"points": control}})
        state = added["structuredContent"]["state"]
        assert len(state["metadata"]["control_points"]) == 3
        alignment = {"request_key": "gui-align-cp", "target": state["target"], "parameters": {"source": "cp"}}
        submitted = call("step2.align_reference", alignment)["structuredContent"]["job"]
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            result = call("get_job", {"job_id": submitted["job_id"]})["structuredContent"]
            assert result["status"] not in {"failed", "cancelled", "interrupted"}, result
            if result["status"] == "completed" and result["gui_apply_status"] == "applied":
                break
            time.sleep(0.1)
        else:
            raise TimeoutError(result)
        while time.monotonic() < deadline:
            current = next((s for s in call("list_open_states", {})["structuredContent"]["states"]
                            if s["state_id"] == changed["state_id"] and s["revision"] > state["target"]["revision"]), None)
            if current:
                break
            time.sleep(0.1)
        else:
            raise TimeoutError("auto-applied revision is not visible")
        metadata = call("describe_state", {"target": current})["structuredContent"]["metadata"]
        published = Path(metadata["georeference_result_path"])
        assert len(json.loads(published.read_text())["control_points"]) == 3
        assert not json.loads(session.read_text()).get("control_points")
        print("Step 2 CP captured from the live GUI and auto-applied", submitted["job_id"], flush=True)

        scope = {"scan_indices": [0]}
        anchor = [[0, -1, 0, 4], [1, 0, 0, 12], [0, 0, 1, -8], [0, 0, 0, 1]]
        edited = call("step2.edit_segment", {"target": current, "scope": scope,
                                           "parameters": {"anchor_pose": anchor}})["structuredContent"]["state"]["target"]
        assert edited["revision"] == current["revision"] + 1
        assert call("step2.restore_poses", {"target": current, "scope": scope})["isError"]
        assert call("step2.restore_poses", {"target": edited, "scope": {"scan_indices": []}})["isError"]
        assert call("describe_state", {"target": edited})["structuredContent"]["target"] == edited
        translate = [[1, 0, 0, 2], [0, 1, 0, -3], [0, 0, 1, 1], [0, 0, 0, 1]]
        transform = {"target": edited, "scope": scope, "parameters": {"transform": translate},
                     "request_key": "gui-transform-scope"}
        job = call("step2.transform_scope", transform)["structuredContent"]["job"]
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            result = call("get_job", {"job_id": job["job_id"]})["structuredContent"]
            assert result["status"] not in {"failed", "cancelled", "interrupted"}, result
            if result["status"] == "completed" and result["gui_apply_status"] != "pending":
                assert result["gui_apply_status"] == "applied", result
                break
            time.sleep(0.1)
        else:
            raise TimeoutError("auto-apply Step 2 segment result")
        assert call("step2.transform_scope", transform)["structuredContent"]["job"]["job_id"] == job["job_id"]
        manifest = Path(result["result"]["snapshot"]["manifest_path"])
        published = manifest.parent / json.loads(manifest.read_text())["entry"]
        poses = json.loads(published.read_text())["Session Settings"]["poses_file_name"]
        first_pose = (published.parent / poses).read_text().splitlines()[2:5]
        assert [list(map(float, row.split())) for row in first_pose] == [
            [0, -1, 0, 6], [1, 0, 0, 9], [0, 0, 1, -7],
        ], first_pose
        applied = dict(edited, revision=edited["revision"] + 1)
        assert call("describe_state", {"target": applied})["structuredContent"]["target"] == applied
        print("Step 2 committed pose edit captured and auto-applied", job["job_id"], flush=True)
        def io_job(tool, key, apply="not_requested", **arguments):
            request = {"request_key": key, **arguments}
            reply = call(tool, request)
            assert not reply.get("isError"), reply
            job = reply["structuredContent"]["job_id"]
            assert call(tool, request)["structuredContent"]["job_id"] == job
            deadline = time.monotonic() + 45
            while time.monotonic() < deadline:
                record = call("get_job", {"job_id": job})["structuredContent"]
                assert record["status"] not in {"failed", "cancelled", "interrupted"}, record
                if record["status"] == "completed" and record["gui_apply_status"] != "pending":
                    assert record["gui_apply_status"] == apply, record
                    document = Path(record["result"]["snapshot"]["manifest_path"])
                    return record, document
                time.sleep(0.1)
            raise TimeoutError(tool)

        params = {"format": "mjs", "paths": [str(session)]}
        restore, _ = io_job("session.open", "headless-session-open", parameters=params)
        io_job("session.open", "live-session-open", "applied", target=applied, parameters=params)
        after_open = dict(applied, revision=applied["revision"] + 1)
        assert call("describe_state", {"target": after_open})["structuredContent"]["target"] == after_open
        _, saved = io_job("session.save", "live-session-save", target=after_open)
        subprocess.run([binaries / "processing_publication_test", "--read", saved.parent.parent], check=True)
        _, exported = io_job("export_result", "headless-session-export", snapshot=restore["result"]["snapshot"],
                             scope={"scan_indices": [0]}, parameters={"format": "laz", "skip_zero_timestamps": False})
        manifest = json.loads(exported.read_text())
        assert manifest["parameters"]["scan_indices"] == [0]
        assert (exported.parent / manifest["entry"]).read_bytes()[:4] == b"LASF"
        print("Step 2 MCP session open, auto-apply, native save and scoped export", flush=True)
        assert len(ids) == 4
    finally:
        adapter.stdin.close()
        for child in windows:
            child.terminate()
        for child in [*windows, adapter]:
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
        if coordinator.poll() is None:
            coordinator.send_signal(signal.SIGTERM)
        coordinator.wait(timeout=10)


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="hdmapping-gui-") as temporary:
        check(Path(sys.argv[1]).resolve(), Path(temporary))
