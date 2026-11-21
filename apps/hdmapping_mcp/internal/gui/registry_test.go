package gui

import (
	"context"
	"sync"
	"testing"
	"time"

	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/jobs"
)

func TestReleaseWhenEveryWorkerIsCapturing(t *testing.T) {
	registry := NewRegistry()
	key, err := registry.Connect()
	if err != nil {
		t.Fatal(err)
	}
	target := jobs.TargetRef{StateID: "open", Revision: 1}
	poll := Poll{Session: key, States: []jobs.TargetRef{target}}
	if _, err = registry.Poll(poll); err != nil {
		t.Fatal(err)
	}
	session, err := registry.Find(target.StateID)
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(t.Context(), 5*time.Second)
	var wg sync.WaitGroup
	defer func() { cancel(); registry.Remove(key); wg.Wait() }()
	for range jobs.MaxLimit {
		wg.Go(func() { _, _ = session.Call(ctx, "states.capture", map[string]string{"phase": "write"}) })
		for {
			command, err := registry.Poll(poll)
			if err != nil {
				t.Fatal(err)
			}
			if command != nil {
				break
			}
			if err := ctx.Err(); err != nil {
				t.Fatal(err)
			}
		}
	}
	registry.Release(session, map[string]string{"phase": "release"})
	release, err := registry.Poll(poll)
	if err != nil || release == nil || release.Body.(map[string]string)["phase"] != "release" {
		t.Fatalf("cancellation unavailable with all workers capturing: %v %v", release, err)
	}
}
