package db

import (
	"context"
	"fmt"
	"log/slog"
	"sync"
	"time"
)

// Scrubbing the WAL after an account deletion (security audit SA5-3).
//
// secure_delete=ON zeroes the deleted rows, but SQLite writes those zeroed
// pages through the WAL, and app.db-wal still holds the pages as they were
// before the deletion until a checkpoint resets it. A TRUNCATE checkpoint
// copies every frame into app.db and cuts app.db-wal to zero bytes.
//
// A checkpoint cannot finish while any reader holds a snapshot that needs
// the WAL, and while it waits it holds off new writers. So it never runs on
// the request path or the writer connection: ScheduleCheckpoint starts (or
// re-arms) one background goroutine that tries on the maintenance
// connection, whose busy timeout is short (maintBusyMS), and retries with
// backoff until it succeeds or the schedule runs out. If it runs out, the
// next scheduled, startup or nightly checkpoint finishes the job.

// maintBusyMS is the maintenance connection's busy timeout: the longest one
// checkpoint attempt can hold off writers.
const maintBusyMS = "50"

// scrubBackoff is the wait before each retry (about 2 minutes in all).
var scrubBackoff = []time.Duration{
	100 * time.Millisecond, 200 * time.Millisecond, 400 * time.Millisecond, 800 * time.Millisecond,
	1600 * time.Millisecond, 3200 * time.Millisecond, 6400 * time.Millisecond, 12800 * time.Millisecond,
	30 * time.Second, 60 * time.Second,
}

type scrubState struct {
	mu      sync.Mutex
	running bool
	again   bool // a deletion arrived while running: start over when done
	closed  bool
	stop    chan struct{}
	wg      sync.WaitGroup
}

// ScheduleCheckpoint arranges a TRUNCATE checkpoint in the background and
// returns at once. Calls while one is pending are coalesced.
func (d *DB) ScheduleCheckpoint() {
	s := &d.scrub
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed {
		return
	}
	if s.running {
		s.again = true
		return
	}
	if s.stop == nil {
		s.stop = make(chan struct{})
	}
	s.running = true
	s.wg.Add(1)
	go d.scrubLoop(s.stop)
}

func (d *DB) scrubLoop(stop <-chan struct{}) {
	s := &d.scrub
	defer s.wg.Done()
	for {
		d.checkpointWithRetry(stop)
		s.mu.Lock()
		if !s.again || s.closed {
			s.running = false
			s.mu.Unlock()
			return
		}
		s.again = false
		s.mu.Unlock()
	}
}

func (d *DB) checkpointWithRetry(stop <-chan struct{}) {
	for attempt := 0; ; attempt++ {
		err := d.CheckpointTruncate(context.Background())
		if err == nil {
			return
		}
		if attempt == len(scrubBackoff) {
			// No user data in this message: the error is SQLite's or ours.
			slog.Warn("database: WAL checkpoint after an account deletion did not complete; the next checkpoint will",
				"attempts", attempt+1, "err", err)
			return
		}
		select {
		case <-time.After(scrubBackoff[attempt]):
		case <-stop:
			return
		}
	}
}

// stopScrub cancels a pending background checkpoint and waits for it.
func (d *DB) stopScrub() {
	s := &d.scrub
	s.mu.Lock()
	if !s.closed {
		s.closed = true
		if s.stop != nil {
			close(s.stop)
		}
	}
	s.mu.Unlock()
	s.wg.Wait()
}

// CheckpointTruncate runs one PRAGMA wal_checkpoint(TRUNCATE) on the
// maintenance connection: every WAL frame is copied into app.db and
// app.db-wal is cut to zero bytes. It returns an error if a reader or
// writer kept it from finishing within the short busy timeout.
func (d *DB) CheckpointTruncate(ctx context.Context) error {
	var busy, logFrames, checkpointed int
	if err := d.maint.QueryRowContext(ctx, "PRAGMA wal_checkpoint(TRUNCATE)").Scan(&busy, &logFrames, &checkpointed); err != nil {
		return err
	}
	if busy != 0 {
		return fmt.Errorf("wal checkpoint incomplete: %d of %d frames checkpointed", checkpointed, logFrames)
	}
	return nil
}
