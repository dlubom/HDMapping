package supervisor

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"

	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/jobs"
)

func TestCameraCaptureSurvivesSourceMoveAndRejectsChangedBytes(t *testing.T) {
	root := t.TempDir()
	source := filepath.Join(root, "kamera żółć")
	if err := os.Mkdir(source, 0700); err != nil {
		t.Fatal(err)
	}
	name := "cam0_1000000000.jpg"
	const original = "camera input"
	if err := os.WriteFile(filepath.Join(source, name), []byte(original), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(source, "ignore.txt"), []byte("other metadata"), 0600); err != nil {
		t.Fatal(err)
	}
	parameters, err := json.Marshal(map[string][]string{"camera_dirs": {source}})
	if err != nil {
		t.Fatal(err)
	}
	job := jobs.JobRecord{ID: "0123456789abcdef0123456789abcdef", Request: jobs.SubmitRequest{
		Operation: jobs.OperationRequest{Operation: jobs.DetectLoopCandidates, Parameters: parameters},
	}}
	s := Supervisor{Directory: root}
	if err := s.stageCameras(t.Context(), job); err != nil {
		t.Fatal(err)
	}
	if err := os.Rename(source, source+".moved"); err != nil {
		t.Fatal(err)
	}
	operation, err := s.useStagedCameras(t.Context(), job)
	if err != nil {
		t.Fatal(err)
	}
	_, cameras, err := cameraPaths(operation)
	if err != nil || len(cameras) != 1 {
		t.Fatalf("unexpected staged folders: %v (%v)", cameras, err)
	}
	frame := filepath.Join(cameras[0], name)
	data, err := os.ReadFile(frame)
	if err != nil || string(data) != original {
		t.Fatalf("staged frame = %q (%v)", data, err)
	}
	if err := os.WriteFile(frame, []byte("changed data"), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := s.useStagedCameras(t.Context(), job); err == nil {
		t.Fatal("accepted a changed staged frame")
	}
}
