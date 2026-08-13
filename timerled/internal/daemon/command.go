package daemon

import "time"

type cmdVerb string

const (
	verbPomodoro cmdVerb = "pomodoro"
	verbCancel   cmdVerb = "cancel"
	verbOff      cmdVerb = "off"
	verbNormal   cmdVerb = "normal"
	verbStatus   cmdVerb = "status"
)

type cmdRequest struct {
	verb  cmdVerb
	dur   time.Duration
	reply chan string
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
	default:
		req.reply <- "unknown command"
	}
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
