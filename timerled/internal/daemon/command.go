package daemon

import (
	"encoding/json"
	"time"
)

type cmdVerb string

const (
	verbPomodoro   cmdVerb = "pomodoro"
	verbCancel     cmdVerb = "cancel"
	verbOff        cmdVerb = "off"
	verbNormal     cmdVerb = "normal"
	verbStatus     cmdVerb = "status"
	verbBrightness cmdVerb = "brightness"
	verbTrigger    cmdVerb = "trigger"
	verbSnapshot   cmdVerb = "snapshot"
)

// brightnessTargets are the sliders the dashboard exposes -- deliberately a
// small curated set (not every group the firmware can address) so a bad
// value can't clobber state the automatic Regular/Music rendering depends
// on (e.g. the paused-dim level, or the ring/digits during a pomodoro).
const (
	targetStars         = "stars"
	targetConstellation = "constellation"
	targetClock         = "clock"
)

// triggerNames are the fire-and-forget one-shot animations the dashboard can
// fire. Each just plays on top of whatever's currently held/twinkling; they
// don't change d.lastMode, so the next regular tick keeps rendering as if
// nothing happened.
const (
	triggerBounce = "bounce"
	triggerReveal = "reveal"
	triggerSweep  = "sweep"
	triggerBlink  = "blink"
)

type cmdRequest struct {
	verb    cmdVerb
	dur     time.Duration
	target  string
	value   uint8
	trigger string
	reply   chan string
}

func (d *Daemon) handleCommand(req cmdRequest) {
	switch req.verb {
	case verbPomodoro:
		d.pomodoroActive = true
		d.pomodoroTotal = req.dur
		d.pomodoroEnd = time.Now().Add(req.dur)
		req.reply <- "pomodoro started for " + req.dur.String()
	case verbCancel:
		if d.pomodoroActive {
			d.pomodoroActive = false
			req.reply <- "pomodoro cancelled"
		} else {
			req.reply <- "no active pomodoro"
		}
	case verbOff:
		d.forcedOff = true
		d.pomodoroActive = false
		req.reply <- "off"
	case verbNormal:
		d.forcedOff = false
		d.pomodoroActive = false
		req.reply <- "normal"
	case verbStatus:
		req.reply <- d.statusString()
	case verbBrightness:
		req.reply <- d.setBrightness(req.target, req.value)
	case verbTrigger:
		req.reply <- d.fireTrigger(req.trigger)
	case verbSnapshot:
		req.reply <- d.snapshotJSON()
	default:
		req.reply <- "unknown command"
	}
}

// setBrightness updates the persistent brightness setting for one of the
// curated targets and, if that target is currently visible on the panel
// (not blanked by off/pomodoro, and not the dimmed-for-pause constellation
// level), pushes an immediate on-device fade so the dashboard slider feels
// live instead of waiting for the next mode transition.
func (d *Daemon) setBrightness(target string, value uint8) string {
	visible := !d.forcedOff && !d.pomodoroActive && !d.sleeping
	switch target {
	case targetStars:
		d.starMaxBrightness = value
		if visible && d.musicSub != musicSubPaused {
			if err := d.panel.ArmTwinkle("stray_stars", d.starMinBrightness, d.starMaxBrightness); err != nil {
				return "error: " + err.Error()
			}
		}
	case targetConstellation:
		d.constellationBrightness = value
		if visible && d.musicSub != musicSubPaused {
			if err := d.panel.EaseGroupFromCurrent("both_constellations", value, constellationEaseMs); err != nil {
				return "error: " + err.Error()
			}
		}
	case targetClock:
		d.clockBrightness = value
		if visible {
			d.lastRenderedClockKey = -1
		}
	default:
		return "error: unknown brightness target " + target
	}
	return "ok"
}

// fireTrigger plays a one-shot animation on top of whatever's currently
// held, without touching d.lastMode -- the next regular tick keeps
// rendering as if nothing happened. Silently skipped while the panel is
// blanked (off/pomodoro/asleep), where there's nothing to animate on top of.
func (d *Daemon) fireTrigger(name string) string {
	if d.forcedOff || d.pomodoroActive || d.sleeping {
		return "error: panel not showing regular/music decorations right now"
	}
	var err error
	switch name {
	case triggerBounce:
		err = d.panel.HourBounce("both_constellations", d.constellationBrightness)
	case triggerReveal:
		err = d.panel.TraceBreatheReveal("both_constellations", d.constellationBrightness)
	case triggerSweep:
		err = d.panel.RingSweep(40, 200, 60, 2)
	case triggerBlink:
		err = d.panel.FinishBlink()
	default:
		return "error: unknown trigger " + name
	}
	if err != nil {
		return "error: " + err.Error()
	}
	return "ok"
}

type statusSnapshot struct {
	Mode              string `json:"mode"`
	PomodoroRemaining string `json:"pomodoroRemaining,omitempty"`
	Stars             uint8  `json:"stars"`
	Constellation     uint8  `json:"constellation"`
	Clock             uint8  `json:"clock"`
}

func (d *Daemon) snapshotJSON() string {
	s := statusSnapshot{
		Stars:         d.starMaxBrightness,
		Constellation: d.constellationBrightness,
		Clock:         d.clockBrightness,
	}
	switch {
	case d.forcedOff:
		s.Mode = "off"
	case d.pomodoroActive:
		s.Mode = "pomodoro"
		remaining := time.Until(d.pomodoroEnd).Round(time.Second)
		if remaining < 0 {
			remaining = 0
		}
		s.PomodoroRemaining = remaining.String()
	default:
		s.Mode = d.lastMode.String()
	}
	b, err := json.Marshal(s)
	if err != nil {
		return `{"error":"` + err.Error() + `"}`
	}
	return string(b)
}

func (d *Daemon) statusString() string {
	if d.forcedOff {
		return "mode=off"
	}
	if d.pomodoroActive {
		remaining := time.Until(d.pomodoroEnd).Round(time.Second)
		if remaining < 0 {
			remaining = 0
		}
		return "mode=pomodoro remaining=" + remaining.String()
	}
	return "mode=" + d.lastMode.String()
}
