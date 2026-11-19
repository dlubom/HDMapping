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
