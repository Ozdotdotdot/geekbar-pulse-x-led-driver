// Package mpdwatch wraps the `mpc` CLI (rather than speaking the MPD
// protocol directly) -- this stays a "mini binary" the same way the rest of
// timerled does, and mpc is already how the user drives MPD by hand.
package mpdwatch

import (
	"context"
	"os/exec"
	"strconv"
	"strings"
	"time"
)

type PlayerState int

const (
	Stopped PlayerState = iota
	Paused
	Playing
)

type Status struct {
	State    PlayerState
	SongID   string // file path -- used to detect "new track"
	Elapsed  time.Duration
	Duration time.Duration
}

// CurrentStatus shells out to `mpc status` / `mpc current -f %file%` and
// parses out just what the daemon needs: play state, current track
// identity, and song position for the ring progress.
func CurrentStatus() (Status, error) {
	var st Status

	fileOut, err := exec.Command("mpc", "current", "-f", "%file%").Output()
	if err != nil {
		// mpc failing (mpd not running, no current song) just means "not
		// playing" as far as the daemon is concerned, not a fatal error.
		return Status{State: Stopped}, nil
	}
	st.SongID = strings.TrimSpace(string(fileOut))
	if st.SongID == "" {
		return Status{State: Stopped}, nil
	}

	statusOut, err := exec.Command("mpc", "status").Output()
	if err != nil {
		return Status{State: Stopped}, nil
	}
	lines := strings.Split(string(statusOut), "\n")
	if len(lines) < 2 {
		return Status{State: Stopped}, nil
	}

	statusLine := lines[1]
	switch {
	case strings.Contains(statusLine, "[playing]"):
		st.State = Playing
	case strings.Contains(statusLine, "[paused]"):
		st.State = Paused
	default:
		st.State = Stopped
	}

	// statusLine looks like: "[playing] #3/12   1:23/4:56 (28%)"
	elapsed, duration, ok := parseTimes(statusLine)
	if ok {
		st.Elapsed, st.Duration = elapsed, duration
	}

	return st, nil
}

func parseTimes(line string) (elapsed, duration time.Duration, ok bool) {
	for f := range strings.FieldsSeq(line) {
		if !strings.Contains(f, "/") || !strings.Contains(f, ":") {
			continue
		}
		parts := strings.SplitN(f, "/", 2)
		if len(parts) != 2 {
			continue
		}
		e, eok := parseClock(parts[0])
		d, dok := parseClock(parts[1])
		if eok && dok {
			return e, d, true
		}
	}
	return 0, 0, false
}

func parseClock(s string) (time.Duration, bool) {
	parts := strings.Split(s, ":")
	if len(parts) < 2 || len(parts) > 3 {
		return 0, false
	}
	var nums []int
	for _, p := range parts {
		n, err := strconv.Atoi(p)
		if err != nil {
			return 0, false
		}
		nums = append(nums, n)
	}
	var secs int
	for _, n := range nums {
		secs = secs*60 + n
	}
	return time.Duration(secs) * time.Second, true
}

// WaitForChange blocks on `mpc idle player`, returning when MPD reports a
// player-state change (play/pause/stop/song change) or ctx is cancelled.
// This is the event-driven alternative to polling that the user asked for.
func WaitForChange(ctx context.Context) error {
	cmd := exec.CommandContext(ctx, "mpc", "idle", "player")
	return cmd.Run()
}
