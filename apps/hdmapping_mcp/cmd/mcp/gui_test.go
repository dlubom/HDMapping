package main

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"time"

	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/gui"
	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/jobs"
	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/protocol"
	sdk "github.com/modelcontextprotocol/go-sdk/mcp"
)

// This subprocess owns the GUI end of the relay pipe. Killing it closes the
// pipe at the OS boundary just like force-killing the desktop application.
func TestGUISourceProcess(t *testing.T) {
	adapter := os.Getenv("HDMAPPING_GUI_FIXTURE_ADAPTER")
	if adapter == "" {
		t.Skip("subprocess helper")
	}
	relay := exec.Command(adapter, "--gui-bridge")
	relay.Stdin = struct{ io.Reader }{os.Stdin} // os/exec creates a private pipe owned by this process.
	relay.Stdout, relay.Stderr = os.Stdout, os.Stderr
	if err := relay.Run(); err != nil {
		os.Exit(1)
	}
	os.Exit(0)
}

type desktop struct {
	cmd    *exec.Cmd
	input  io.WriteCloser
	output *bufio.Reader
	poll   gui.Poll
}

func startDesktop(t *testing.T, ctx context.Context, adapter, dir string, target jobs.TargetRef) *desktop {
	t.Helper()
	exe, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	cmd := exec.CommandContext(ctx, exe, "-test.run=^TestGUISourceProcess$")
	cmd.Env = append(os.Environ(), "HDMAPPING_GUI_FIXTURE_ADAPTER="+adapter, "HDMAPPING_MCP_STATE_DIR="+dir, "GORACE=atexit_sleep_ms=0 halt_on_error=1")
	cmd.Stderr = os.Stderr
	input, err := cmd.StdinPipe()
	if err != nil {
		t.Fatal(err)
	}
	output, err := cmd.StdoutPipe()
	if err != nil {
		t.Fatal(err)
	}
	if err = cmd.Start(); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		_ = input.Close()
		if cmd.ProcessState == nil {
			_ = cmd.Process.Kill()
			_ = cmd.Wait()
		}
	})
	return &desktop{cmd: cmd, input: input, output: bufio.NewReader(output), poll: gui.Poll{Wire: protocol.WireVersion, Version: "1.2.3", States: []jobs.TargetRef{target}}}
}

type received struct {
	ID     string
	Method string
	Body   struct {
		Phase      string
		Directory  string
		Job        jobs.JobID `json:"job_id"`
		Target     jobs.TargetRef
		Operation  json.RawMessage
		Parameters map[string]any
	}
}

func (d *desktop) exchange(t *testing.T) *received {
	t.Helper()
	if err := protocol.WriteFrame(d.input, d.poll); err != nil {
		t.Fatal(err)
	}
	var response struct {
		Command *received
		Error   string
	}
	if err := protocol.ReadFrame(d.output, &response); err != nil || response.Error != "" {
		t.Fatalf("GUI response: %s %v", response.Error, err)
	}
	d.poll.Reply = nil
	return response.Command
}
func (d *desktop) next(t *testing.T, ctx context.Context, c *received, method, phase string) *received {
	t.Helper()
	ctx, cancel := context.WithTimeout(ctx, 5*time.Second)
	defer cancel()
	for c == nil {
		select {
		case <-ctx.Done():
			t.Fatalf("waiting for %s/%s: %v", method, phase, ctx.Err())
		default:
		}
		c = d.exchange(t)
	}
	if c.Method != method || c.Body.Phase != phase {
		t.Fatalf("unexpected GUI command: %+v", c)
	}
	return c
}
func (d *desktop) reply(t *testing.T, c *received, body any) *received {
	t.Helper()
	raw, err := json.Marshal(body)
	if err != nil {
		t.Fatal(err)
	}
	d.poll.Reply = &gui.Reply{ID: c.ID, Body: raw}
	return d.exchange(t)
}

func checkLiveTools(t *testing.T, ctx context.Context, d *desktop, assistant *sdk.ClientSession, target jobs.TargetRef) jobs.TargetRef {
	t.Helper()
	for _, tc := range []struct{ name, method string }{{"describe_state", "states.describe"}, {"set_state_parameters", "states.execute"}} {
		args := map[string]any{"target": target}
		if tc.name == "set_state_parameters" {
			args["step"] = "step2"
			args["parameters"] = map[string]any{"iterations": 12}
		}
		type answer struct {
			result *sdk.CallToolResult
			err    error
		}
		replies := make(chan answer, 1)
		go func() {
			result, err := assistant.CallTool(ctx, &sdk.CallToolParams{Name: tc.name, Arguments: args})
			replies <- answer{result, err}
		}()
		command := d.next(t, ctx, nil, tc.method, "")
		if command.Body.Target != target {
			t.Fatalf("unexpected target %+v", command.Body)
		}
		if tc.name == "set_state_parameters" {
			if string(command.Body.Operation) != `"step2.set_parameters"` || command.Body.Parameters["iterations"] != float64(12) {
				t.Fatalf("unexpected GUI mutation: %+v", command.Body)
			}
			target.Revision++
			d.poll.States[0] = target
		}
		d.reply(t, command, map[string]any{"target": target, "parameters": map[string]any{"iterations": 12}, "metadata": map[string]any{"step": "step2"}})
		reply := <-replies
		if reply.err != nil || reply.result.IsError {
			t.Fatalf("MCP tool %s failed: %v %v", tc.name, reply.result, reply.err)
		}
		raw, err := json.Marshal(reply.result.StructuredContent)
		if err != nil {
			t.Fatal(err)
		}
		var got struct{ Target jobs.TargetRef }
		if err = json.Unmarshal(raw, &got); err != nil || got.Target != target {
			t.Fatalf("MCP tool %s returned %s (%v)", tc.name, raw, err)
		}
	}
	return target
}

func TestLiveGUIHandoff(t *testing.T) {
	ctx, cancel := context.WithTimeout(t.Context(), 60*time.Second)
	defer cancel()
	adapter, coordinator, _ := buildTestProduct(t, ctx, "1.2.3", "GUI-")
	dir, err := protocol.ValidateDirectory(filepath.Join(t.TempDir(), "żywy stan"))
	if err != nil {
		t.Fatal(err)
	}
	client, err := protocol.NewClient(dir, "1.2.3", coordinator)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { stopCoordinator(t, client) })
	assistant := startAdapter(t, ctx, adapter, dir)
	for _, scenario := range []string{"window_closed", "source_killed", "applied", "conflict", "reconnected", "read_result", "cancel_transfer"} {
		t.Run(scenario, func(t *testing.T) {
			target := jobs.TargetRef{StateID: scenario, Revision: 7}
			d := startDesktop(t, ctx, adapter, dir, target)
			if c := d.exchange(t); c != nil {
				t.Fatalf("unexpected command %+v", c)
			}
			if scenario == "window_closed" {
				// A GUI opened without an assistant leaves local preview independent of the coordinator.
				if err := client.Call(ctx, "ping", nil, nil); err == nil {
					t.Fatal("idle GUI started a coordinator")
				}
			}
			type listing struct {
				result *sdk.CallToolResult
				err    error
			}
			replies := make(chan listing, 1)
			go func() {
				result, err := assistant.CallTool(ctx, &sdk.CallToolParams{Name: "list_open_states", Arguments: map[string]any{}})
				replies <- listing{result, err}
			}()
			// The desktop's background transport keeps polling as the first MCP
			// query starts the coordinator and waits for initial discovery.
			var result *sdk.CallToolResult
			for result == nil {
				select {
				case reply := <-replies:
					if reply.err != nil || reply.result.IsError {
						t.Fatalf("list_open_states: %v %+v", reply.err, reply.result)
					}
					result = reply.result
				default:
					if c := d.exchange(t); c != nil {
						t.Fatalf("unexpected command %+v", c)
					}
				}
			}

			data, err := json.Marshal(result.StructuredContent)
			if err != nil {
				t.Fatal(err)
			}
			var listed struct{ States []jobs.TargetRef }
			if err = json.Unmarshal(data, &listed); err != nil || len(listed.States) != 1 || listed.States[0] != target {
				t.Fatalf("live list %s: %v", data, err)
			}
			if scenario == "applied" {
				target = checkLiveTools(t, ctx, d, assistant, target)
			}
			// A returned job ID requires an acknowledged pin even when other workers are busy.
			type admission struct {
				job jobs.JobRecord
				err error
			}
			admitted := make(chan admission, 1)
			request := jobs.SubmitRequest{RequestKey: scenario, Operation: jobs.OperationRequest{Operation: "durable", Target: target, Scope: json.RawMessage("{}"), Parameters: json.RawMessage("{}")}, Source: jobs.SourceMCP, Access: jobs.AccessMutate, CaptureSource: target.StateID}
			if scenario == "read_result" {
				request.Access = jobs.AccessRead
			}
			go func() {
				var j jobs.JobRecord
				var err error
				if scenario == "applied" {
					j, err = submitFromDesktop(ctx, adapter, dir, request)
				} else {
					err = client.Call(ctx, "jobs.submit", request, &j)
				}
				admitted <- admission{j, err}
			}()
			reserve := d.next(t, ctx, nil, "states.capture", "reserve")
			var operation jobs.OperationRequest
			if err := json.Unmarshal(reserve.Body.Operation, &operation); err != nil || operation.Operation != request.Operation.Operation || operation.Target != target {
				t.Fatalf("reservation lost operation: %s (%v)", reserve.Body.Operation, err)
			}
			select {
			case a := <-admitted:
				t.Fatalf("admitted before reservation: %+v", a)
			default:
			}
			c := d.reply(t, reserve, struct{}{})
			a := <-admitted
			if a.err != nil || a.job.ID != reserve.Body.Job || a.job.Status != jobs.Preparing {
				t.Fatalf("admission %+v", a)
			}
			if scenario == "window_closed" {
				d.poll.States = nil
				next := d.exchange(t)
				if c == nil {
					c = next
				} else if next != nil {
					t.Fatalf("unexpected command during close %+v", next)
				}
				var visible struct{ States []jobs.TargetRef }
				call(t, ctx, client, "states.list", nil, &visible)
				if len(visible.States) != 0 {
					t.Fatalf("closed opening is still listed: %+v", visible)
				}
			}
			write := d.next(t, ctx, c, "states.capture", "write")
			var same jobs.JobRecord
			request.Source = a.job.Request.Source
			call(t, ctx, client, "jobs.submit", request, &same)
			if same.ID != a.job.ID {
				t.Fatal("duplicate GUI job")
			}
			if scenario == "source_killed" {
				if err = d.cmd.Process.Kill(); err != nil {
					t.Fatal(err)
				}
				_ = d.cmd.Wait()
				j := waitJob(t, ctx, client, a.job.ID, "", jobs.Interrupted)
				if j.Result != nil || j.Snapshot != nil || j.GUIApplyStatus != "not_requested" {
					t.Fatalf("source loss published %+v", j)
				}
				return
			}
			if scenario == "cancel_transfer" {
				call(t, ctx, client, "jobs.cancel", a.job.ID, nil)
				release := d.next(t, ctx, nil, "states.capture", "release")
				d.poll.Reply = &gui.Reply{ID: write.ID, Error: "capture cancelled"}
				d.exchange(t)
				d.reply(t, release, struct{}{})
				waitJob(t, ctx, client, a.job.ID, "", jobs.Cancelled)
				return
			}
			if scenario == "conflict" {
				d.poll.States[0].Revision++
			}
			if err = os.Mkdir(write.Body.Directory, 0700); err != nil {
				t.Fatal(err)
			}
			path := filepath.Join(write.Body.Directory, "snapshot.json")
			if err = os.WriteFile(path, []byte(`{"fixture_complete":true}`), 0600); err != nil {
				t.Fatal(err)
			}
			d.reply(t, write, jobs.SnapshotRef{ManifestPath: path, Target: target})
			if scenario == "window_closed" || scenario == "reconnected" {
				if err = d.input.Close(); err != nil {
					t.Fatal(err)
				}
				if err = d.cmd.Wait(); err != nil {
					t.Fatal(err)
				}
				if scenario == "reconnected" {
					d = startDesktop(t, ctx, adapter, dir, target)
					if c = d.exchange(t); c != nil {
						t.Fatalf("unexpected command after reconnect %+v", c)
					}
				}
			}
			waitJob(t, ctx, client, a.job.ID, "durable", jobs.Running)
			// The fixture has published a durable result. Cancellation lets it exit;
			// publication wins this race, as required by P02/P03.
			call(t, ctx, client, "jobs.cancel", a.job.ID, nil)
			want := "closed"
			switch scenario {
			case "window_closed":
			case "read_result":
				want = "not_requested"
				release := d.next(t, ctx, nil, "states.capture", "release")
				d.reply(t, release, struct{}{})
			default:
				apply := d.next(t, ctx, nil, "states.apply_result", "")
				if apply.Body.Target != target || apply.Body.Job != a.job.ID {
					t.Fatalf("wrong target: %+v", apply)
				}
				want = "applied"
				if scenario == "conflict" {
					want = "stale_revision"
					d.poll.Reply = &gui.Reply{ID: apply.ID, Error: "stale_revision: changed"}
					c = d.exchange(t)
				} else {
					c = d.reply(t, apply, struct{}{})
				}
				if scenario != "reconnected" {
					release := d.next(t, ctx, c, "states.capture", "release")
					d.reply(t, release, struct{}{})
				}
			}
			deadline, stop := context.WithTimeout(ctx, 5*time.Second)
			defer stop()
			for {
				var j jobs.JobRecord
				call(t, deadline, client, "jobs.get", a.job.ID, &j)
				if j.GUIApplyStatus == want {
					if j.Status != jobs.Completed || j.Result == nil || j.Snapshot == nil || j.Snapshot.ManifestPath != path {
						t.Fatalf("lost result or snapshot: %+v", j)
					}
					if _, err = os.Stat(j.Result.ManifestPath); err != nil {
						t.Fatal(err)
					}
					break
				}
			}
			if scenario != "window_closed" {
				if err = d.input.Close(); err != nil && !errors.Is(err, os.ErrClosed) {
					t.Fatal(err)
				}
				if err = d.cmd.Wait(); err != nil {
					t.Fatal(err)
				}
			}
		})
	}
}

func submitFromDesktop(ctx context.Context, adapter, dir string, request jobs.SubmitRequest) (jobs.JobRecord, error) {
	var j jobs.JobRecord
	var input bytes.Buffer
	if err := protocol.WriteFrame(&input, map[string]any{
		"wire": protocol.WireVersion, "version": "1.2.3", "method": "jobs.submit", "body": request,
	}); err != nil {
		return j, err
	}
	cmd := exec.CommandContext(ctx, adapter, "--gui-rpc")
	cmd.Stdin, cmd.Stderr = &input, os.Stderr
	cmd.Env = append(os.Environ(), "HDMAPPING_MCP_STATE_DIR="+dir, "GORACE=atexit_sleep_ms=0 halt_on_error=1")
	data, err := cmd.Output()
	if err != nil {
		return j, err
	}
	var reply struct {
		Body  jobs.JobRecord
		Error string
	}
	if err = protocol.ReadFrame(bufio.NewReader(bytes.NewReader(data)), &reply); err != nil {
		return j, err
	}
	if reply.Error != "" {
		return j, errors.New(reply.Error)
	}
	if reply.Body.Request.Source != jobs.SourceGUI {
		return j, errors.New("desktop job lost its source")
	}
	return reply.Body, nil
}
