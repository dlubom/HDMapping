// Package gui routes private coordinator commands to the process that owns an opening.
package gui

import (
	"bytes"
	"context"
	"crypto/rand"
	"encoding/json"
	"errors"
	"fmt"
	"slices"
	"strings"
	"sync"
	"time"

	"github.com/MapsHD/HDMapping/apps/hdmapping_mcp/internal/jobs"
)

type Poll struct {
	Wire    int              `json:"wire"`
	Version string           `json:"version"`
	Session string           `json:"session,omitempty"`
	States  []jobs.TargetRef `json:"states"`
	Reply   *Reply           `json:"reply,omitempty"`
}
type Reply struct {
	ID    string          `json:"id"`
	Body  json.RawMessage `json:"body,omitempty"`
	Error string          `json:"error,omitempty"`
}
type Command struct {
	ID     string `json:"id"`
	Method string `json:"method"`
	Body   any    `json:"body"`
}
type Envelope struct {
	Wire    int      `json:"wire"`
	Version string   `json:"version"`
	Command *Command `json:"command,omitempty"`
	Error   string   `json:"error,omitempty"`
}

type pending struct {
	command Command
	ctx     context.Context
	result  chan Reply
}
type Session struct {
	queue    chan *pending
	releases chan *pending
	done     chan struct{}
	active   map[string]*pending
	seen     time.Time
	states   []jobs.TargetRef
}
type Registry struct {
	mu            sync.Mutex
	sessions      map[string]*Session
	discoverUntil time.Time
}

func NewRegistry() *Registry {
	return &Registry{sessions: make(map[string]*Session), discoverUntil: time.Now().Add(500 * time.Millisecond)}
}
func (r *Registry) Connect() (string, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if len(r.sessions) >= 128 {
		return "", errors.New("GUI session limit reached")
	}
	key := rand.Text()
	r.sessions[key] = &Session{queue: make(chan *pending, 16), releases: make(chan *pending, jobs.MaxPending), active: make(map[string]*pending), done: make(chan struct{}), seen: time.Now()}
	return key, nil
}
func (r *Registry) Remove(key string) { r.mu.Lock(); defer r.mu.Unlock(); r.remove(key) }
func (r *Registry) remove(key string) {
	if s := r.sessions[key]; s != nil {
		delete(r.sessions, key)
		close(s.done)
	}
}

// Expire detects a killed relay. The GUI connection thread polls independently
// of the application's render thread and capture workers, including during close.
func (r *Registry) Expire(ctx context.Context) {
	ticker := time.NewTicker(time.Second)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			r.mu.Lock()
			for key := range r.sessions {
				r.remove(key)
			}
			r.mu.Unlock()
			return
		case now := <-ticker.C:
			r.mu.Lock()
			for key, s := range r.sessions {
				if now.Sub(s.seen) > 10*time.Second {
					r.remove(key)
				}
			}
			r.mu.Unlock()
		}
	}
}
func (r *Registry) Poll(p Poll) (*Command, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if len(p.Session) < 16 || len(p.Session) > 128 || len(p.States) > 32 {
		return nil, errors.New("invalid GUI session or state count")
	}
	s := r.sessions[p.Session]
	if s == nil {
		return nil, jobs.ErrSourceLost
	}
	seen := make(map[string]bool, len(p.States))
	for _, t := range p.States {
		if strings.TrimSpace(t.StateID) == "" || len(t.StateID) > 256 || seen[t.StateID] {
			return nil, errors.New("invalid GUI state identity")
		}
		seen[t.StateID] = true
		for _, other := range r.sessions {
			if other != s && other.has(t.StateID) {
				return nil, errors.New("GUI state identity already registered")
			}
		}
	}
	s.states = slices.Clone(p.States)
	s.seen = time.Now()
	if p.Reply != nil {
		c := s.active[p.Reply.ID]
		if c == nil {
			return nil, errors.New("unexpected GUI command reply")
		}
		if p.Reply.Error == "" && !bytes.HasPrefix(bytes.TrimSpace(p.Reply.Body), []byte("{")) {
			return nil, errors.New("GUI success reply requires an object")
		}
		c.result <- *p.Reply
		delete(s.active, p.Reply.ID)
	}
	// Keep a separate control slot when every worker has an outstanding capture.
	if len(s.active) > jobs.MaxLimit {
		return nil, nil
	}
	select {
	case c := <-s.releases:
		s.active[c.command.ID] = c
		return &c.command, nil
	default:
	}
	if len(s.active) == jobs.MaxLimit {
		return nil, nil
	}
	for {
		select {
		case c := <-s.queue:
			if c.ctx.Err() == nil {
				s.active[c.command.ID] = c
				return &c.command, nil
			}
		default:
			return nil, nil
		}
	}
}
func (s *Session) Done() <-chan struct{} { return s.done }
func (s *Session) has(id string) bool {
	return slices.ContainsFunc(s.states, func(t jobs.TargetRef) bool { return t.StateID == id })
}
func (r *Registry) Find(id string) (*Session, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	for _, s := range r.sessions {
		if s.has(id) {
			return s, nil
		}
	}
	return nil, jobs.ErrSourceLost
}
func (r *Registry) List(ctx context.Context) ([]jobs.TargetRef, error) {
	// A first MCP query can start the coordinator. Allow already-open GUIs
	// several 100 ms poll cycles to advertise before returning its first inventory.
	if delay := time.Until(r.discoverUntil); delay > 0 {
		timer := time.NewTimer(delay)
		defer timer.Stop()
		select {
		case <-timer.C:
		case <-ctx.Done():
			return nil, ctx.Err()
		}
	}

	r.mu.Lock()
	defer r.mu.Unlock()
	out := []jobs.TargetRef{}
	for _, s := range r.sessions {
		out = append(out, s.states...)
	}
	slices.SortFunc(out, func(a, b jobs.TargetRef) int { return strings.Compare(a.StateID, b.StateID) })
	return out, nil
}

type CommandError struct{ method, message string }

func (e *CommandError) Error() string { return fmt.Sprintf("GUI %s: %s", e.method, e.message) }
func (e *CommandError) Code() string  { code, _, _ := strings.Cut(e.message, ":"); return code }
func (s *Session) Call(ctx context.Context, method string, body any) (json.RawMessage, error) {
	c := &pending{command: Command{ID: rand.Text(), Method: method, Body: body}, ctx: ctx, result: make(chan Reply, 1)}
	select {
	case <-s.done:
		return nil, jobs.ErrSourceLost
	case <-ctx.Done():
		return nil, ctx.Err()
	case s.queue <- c:
	default:
		return nil, errors.New("GUI command queue full")
	}
	var reply Reply
	select {
	case <-s.done:
		// The last poll can durably acknowledge a capture and close the relay
		// before its waiting dispatcher gets a timeslice.
		select {
		case reply = <-c.result:
		default:
			return nil, jobs.ErrSourceLost
		}
	case <-ctx.Done():
		return nil, ctx.Err()
	case reply = <-c.result:
	}
	if reply.Error != "" {
		return nil, &CommandError{method, reply.Error}
	}
	return reply.Body, nil
}

// Release survives a cancelled submit or a slow capture already running on
// the GUI thread. The original session owns cleanup until delivery or exit.
func (r *Registry) Release(s *Session, body any) {
	c := &pending{command: Command{ID: rand.Text(), Method: "states.capture", Body: body}, result: make(chan Reply, 1)}
	select {
	case <-s.done:
	case s.releases <- c:
	default:
		// A process unable to drain even its reserved cleanup queue must drop
		// the session and clear the GUI locks on its next poll.
		r.mu.Lock()
		defer r.mu.Unlock()
		for key, session := range r.sessions {
			if session == s {
				r.remove(key)
				break
			}
		}
	}
}
