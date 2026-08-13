// Package daemon is the long-running process that owns the serial port and
// drives the LED panel through Off/Pomodoro/Music/Regular, in that priority
// order, reacting to MPD, the wall clock, and system sleep/wake.
package daemon

import (
	"context"
	"log"
	"math"
	"time"

	"github.com/Ozdotdotdot/geekbar-pulse-x-led-driver/timerled/internal/mpdwatch"
	"github.com/Ozdotdotdot/geekbar-pulse-x-led-driver/timerled/internal/serialport"
	"github.com/Ozdotdotdot/geekbar-pulse-x-led-driver/timerled/internal/sleepwatch"
)

const modeUnset Mode = -1

type Daemon struct {
	panel      *Panel
	portName   string
	baud       int
	socketPath string

	forcedOff bool

	pomodoroActive bool
	pomodoroEnd    time.Time
	pomodoroTotal  time.Duration

	mpdLastSongID  string
	musicIdleSince time.Time
	musicSub       musicSub

	// justFinishedPomodoro makes the very next Music/Regular entry a soft
	// fade-up from the dark panel the finish blink left behind, instead of
	// the full clear-and-replay reveal -- the blink is already the "big
	// moment", replaying the whole trace-and-breathe on top of it is too
	// much.
	justFinishedPomodoro bool

	lastMode             Mode
	lastRenderedClockKey int
	lastRenderedHour     int

	sleeping bool

	cmds chan cmdRequest
}

func New(portName string, baud int, socketPath string) (*Daemon, error) {
	port, err := serialport.Open(portName, baud)
	if err != nil {
		return nil, err
	}
	return &Daemon{
		panel:                NewPanel(port),
		portName:             portName,
		baud:                 baud,
		socketPath:           socketPath,
		lastMode:             modeUnset,
		lastRenderedClockKey: -1,
		lastRenderedHour:     -1,
		cmds:                 make(chan cmdRequest),
	}, nil
}

// Run blocks until ctx is cancelled, driving the state machine.
func (d *Daemon) Run(ctx context.Context) error {
	ln, err := listen(d.socketPath)
	if err != nil {
		return err
	}
	defer ln.Close()
	go d.serveSocket(ln)

	sleepCh, dbusConn, err := sleepwatch.Watch()
	if err != nil {
		log.Printf("sleep/wake watch unavailable, continuing without it: %v", err)
		sleepCh = nil
	} else {
		defer dbusConn.Close()
	}

	mpdEvents := make(chan struct{}, 1)
	go d.watchMPD(ctx, mpdEvents)

	ticker := time.NewTicker(1 * time.Second)
	defer ticker.Stop()

	d.tick(time.Now())

	for {
		select {
		case <-ctx.Done():
			return nil
		case sleeping, ok := <-sleepCh:
			if !ok {
				sleepCh = nil
				continue
			}
			if sleeping {
				d.handleSuspend()
			} else {
				d.handleResume()
			}
		case <-mpdEvents:
			d.tick(time.Now())
		case <-ticker.C:
			d.tick(time.Now())
		case req := <-d.cmds:
			d.handleCommand(req)
			d.tick(time.Now())
		}
	}
}

func (d *Daemon) watchMPD(ctx context.Context, out chan<- struct{}) {
	for {
		if err := mpdwatch.WaitForChange(ctx); err != nil {
			return
		}
		select {
		case out <- struct{}{}:
		default:
		}
	}
}

func (d *Daemon) handleSuspend() {
	log.Print("suspending: blanking panel")
	d.sleeping = true
	d.panel.DisarmTwinkle()
	d.panel.Clear()
}

func (d *Daemon) handleResume() {
	log.Print("resumed: reopening serial port and redrawing")
	// The ESP32 is USB-powered off this machine and very likely lost power
	// (and reset) during suspend -- the old port handle and any assumed
	// on-device state can't be trusted.
	port, err := serialport.Open(d.portName, d.baud)
	if err != nil {
		log.Printf("failed to reopen serial port after resume: %v", err)
		return
	}
	d.panel = NewPanel(port)
	d.sleeping = false
	d.lastMode = modeUnset
	d.musicSub = musicSubNone
	d.lastRenderedClockKey = -1
	d.lastRenderedHour = -1
	d.tick(time.Now())
}

func (d *Daemon) tick(now time.Time) {
	if d.sleeping {
		return
	}
	if d.forcedOff {
		d.enterIfNeeded(ModeOff, d.enterOff)
		return
	}
	if d.pomodoroActive {
		if !now.Before(d.pomodoroEnd) {
			d.finishPomodoro()
			// falls through to Music/Regular below, same tick
		} else {
			d.enterIfNeeded(ModePomodoro, d.enterPomodoro)
			d.renderPomodoro(now)
			return
		}
	}
	d.tickMusicOrRegular(now)
}

// Music covers two visually distinct sub-states (playing, paused) under one
// Mode value, so enterIfNeeded's plain Mode-equality check can't detect a
// playing->paused transition on its own -- musicSub tracks that separately.
type musicSub int

const (
	musicSubNone musicSub = iota
	musicSubPlaying
	musicSubPaused
)

func (d *Daemon) tickMusicOrRegular(now time.Time) {
	status, _ := mpdwatch.CurrentStatus()

	if status.State == mpdwatch.Playing {
		d.musicIdleSince = time.Time{}
		newTrack := status.SongID != d.mpdLastSongID
		d.mpdLastSongID = status.SongID
		fromOutside := d.lastMode != ModeMusic
		resumingFromPause := d.lastMode == ModeMusic && d.musicSub == musicSubPaused
		softReturn := d.justFinishedPomodoro

		switch {
		case softReturn:
			// The finish blink already had its moment -- just fade back up
			// from the dark panel it left behind instead of replaying the
			// whole reveal on top of it.
			if err := d.enterMusicPlayingSoft(); err != nil {
				log.Printf("soft return failed: %v", err)
			}
			d.lastRenderedClockKey = -1
			d.justFinishedPomodoro = false
		case fromOutside || newTrack:
			// Fresh entry into Music, or a genuinely new track -- full
			// paint-anew reveal. This clears the frame, so the clock digits
			// need to be forced to redraw too instead of waiting for the
			// next real minute change.
			if err := d.enterMusicPlaying(); err != nil {
				log.Printf("track reveal failed: %v", err)
			}
			d.lastRenderedClockKey = -1
		case resumingFromPause:
			// Same track resuming from a pause -- ease back up instead of
			// replaying the whole reveal animation.
			if err := d.enterMusicResume(); err != nil {
				log.Printf("resume failed: %v", err)
			}
		}
		d.lastMode = ModeMusic
		d.musicSub = musicSubPlaying
		d.renderMusicProgress(status)
		d.maybeHourBounce(now, constellationFullBrightness)
		d.renderClock(now, 160)
		return
	}

	if d.lastMode == ModeMusic {
		if d.musicIdleSince.IsZero() {
			d.musicIdleSince = now
		}
		if now.Sub(d.musicIdleSince) < 5*time.Minute {
			if d.musicSub != musicSubPaused {
				if err := d.enterMusicPaused(); err != nil {
					log.Printf("entering music-paused failed: %v", err)
				}
				d.lastRenderedClockKey = -1
			}
			d.lastMode = ModeMusic
			d.musicSub = musicSubPaused
			d.maybeHourBounce(now, constellationPausedBrightness)
			d.renderClock(now, 160)
			return
		}
	}

	d.musicSub = musicSubNone
	d.musicIdleSince = time.Time{}
	if d.justFinishedPomodoro {
		if err := d.enterRegularSoft(); err != nil {
			log.Printf("soft return failed: %v", err)
		}
		d.lastMode = ModeRegular
		d.lastRenderedClockKey = -1
		d.justFinishedPomodoro = false
	} else {
		d.enterIfNeeded(ModeRegular, d.enterRegular)
	}
	d.maybeHourBounce(now, constellationFullBrightness)
	d.renderClock(now, 160)
}

func (d *Daemon) enterIfNeeded(mode Mode, enter func() error) bool {
	if d.lastMode == mode {
		return false
	}
	if err := enter(); err != nil {
		log.Printf("entering %s failed: %v", mode, err)
	}
	d.lastMode = mode
	d.lastRenderedClockKey = -1
	return true
}

func (d *Daemon) enterOff() error {
	d.musicSub = musicSubNone
	d.justFinishedPomodoro = false
	d.panel.DisarmTwinkle()
	return d.panel.Clear()
}

func (d *Daemon) enterPomodoro() error {
	d.musicSub = musicSubNone
	d.justFinishedPomodoro = false
	d.panel.DisarmTwinkle()
	return d.panel.Clear()
}

func (d *Daemon) enterRegular() error {
	d.panel.DisarmTwinkle()
	if err := d.panel.Clear(); err != nil {
		return err
	}
	if err := d.panel.TraceBreatheReveal("both_constellations"); err != nil {
		return err
	}
	return d.panel.ArmTwinkle("stray_stars", starTwinkleMinBrightness, starTwinkleMaxBrightness)
}

// enterRegularSoft is the post-pomodoro version of enterRegular: the panel
// is already dark (the finish blink left it that way), so this just eases
// the constellation up from there and re-arms the twinkle, instead of
// replaying the whole trace-and-breathe reveal on top of a blink that
// already had its moment.
func (d *Daemon) enterRegularSoft() error {
	if err := d.panel.EaseGroupFromCurrent("both_constellations", constellationFullBrightness,
		constellationEaseMs); err != nil {
		return err
	}
	return d.panel.ArmTwinkle("stray_stars", starTwinkleMinBrightness, starTwinkleMaxBrightness)
}

func (d *Daemon) enterMusicPlaying() error {
	d.panel.DisarmTwinkle()
	if err := d.panel.Clear(); err != nil {
		return err
	}
	if err := d.panel.TraceBreatheReveal("both_constellations"); err != nil {
		return err
	}
	if err := d.panel.ArmTwinkle("stray_stars", starTwinkleMinBrightness, starTwinkleMaxBrightness); err != nil {
		return err
	}
	return d.panel.RingProgress(0, 220)
}

// enterMusicPlayingSoft is the post-pomodoro version of enterMusicPlaying:
// the panel is already dark, so this just eases the constellation up and
// re-arms the twinkle instead of replaying the whole reveal.
func (d *Daemon) enterMusicPlayingSoft() error {
	if err := d.panel.EaseGroupFromCurrent("both_constellations", constellationFullBrightness,
		constellationEaseMs); err != nil {
		return err
	}
	if err := d.panel.ArmTwinkle("stray_stars", starTwinkleMinBrightness, starTwinkleMaxBrightness); err != nil {
		return err
	}
	return d.panel.RingProgress(0, 220)
}

// enterMusicPaused shows the paused indicator immediately, then dims the
// held constellation and fades the stray stars out without clearing
// anything -- the constellation stays visibly lit (just dimmer) and the
// stars breathe down to off from wherever each one actually was
// mid-twinkle (not a shared starting brightness), mirroring the fade-in
// the firmware already does when the twinkle is re-armed on resume. The
// icon goes on first, before the (roughly 1s) dim/fade animations run, so
// pausing reads as instant rather than waiting on those to finish.
func (d *Daemon) enterMusicPaused() error {
	if err := d.panel.SetIcon(true); err != nil {
		return err
	}
	if err := d.panel.DisarmTwinkle(); err != nil {
		return err
	}
	if err := d.panel.EaseGroupFromCurrent("stray_stars", 0, starFadeMs); err != nil {
		return err
	}
	return d.panel.EaseGroup("both_constellations", constellationFullBrightness,
		constellationPausedBrightness, constellationEaseMs)
}

// enterMusicResume is the mirror of enterMusicPaused: eases the
// constellation back up and re-arms the twinkle, without a full clear and
// replay of the reveal animation, for resuming the same track.
func (d *Daemon) enterMusicResume() error {
	if err := d.panel.SetIcon(false); err != nil {
		return err
	}
	if err := d.panel.EaseGroup("both_constellations", constellationPausedBrightness,
		constellationFullBrightness, constellationEaseMs); err != nil {
		return err
	}
	return d.panel.ArmTwinkle("stray_stars", starTwinkleMinBrightness, starTwinkleMaxBrightness)
}

func (d *Daemon) finishPomodoro() {
	d.pomodoroActive = false
	if err := d.panel.FinishBlink(); err != nil {
		log.Printf("finish blink failed: %v", err)
	}
	d.justFinishedPomodoro = true
}

func (d *Daemon) renderPomodoro(now time.Time) {
	remaining := d.pomodoroEnd.Sub(now)
	if remaining < 0 {
		remaining = 0
	}
	frac := remaining.Seconds() / d.pomodoroTotal.Seconds()
	filled := int(math.Ceil(frac * 14))
	if err := d.panel.RingProgress(filled, 220); err != nil {
		log.Printf("pomodoro ring update failed: %v", err)
	}

	const brightness = 220
	if remaining >= time.Hour {
		h := int(remaining.Hours())
		m := int(remaining.Minutes()) % 60
		hTens, hOnes := splitDigits(h)
		mTens, mOnes := splitDigits(m)
		if err := d.panel.SetClockDigits(hTens, hOnes, mTens, mOnes, brightness); err != nil {
			log.Printf("pomodoro digit update failed: %v", err)
		}
	} else {
		m := int(remaining.Minutes())
		s := int(remaining.Seconds()) % 60
		mTens, mOnes := splitDigits(m)
		sTens, sOnes := splitDigits(s)
		if err := d.panel.SetClockDigits(mTens, mOnes, sTens, sOnes, brightness); err != nil {
			log.Printf("pomodoro digit update failed: %v", err)
		}
	}
}

func (d *Daemon) renderMusicProgress(status mpdwatch.Status) {
	if status.Duration <= 0 {
		return
	}
	frac := status.Elapsed.Seconds() / status.Duration.Seconds()
	filled := int(math.Round(frac * 14))
	if err := d.panel.RingProgress(filled, 220); err != nil {
		log.Printf("music ring update failed: %v", err)
	}
}

func (d *Daemon) renderClock(now time.Time, brightness uint8) {
	hour, minute := now.Hour(), now.Minute()
	key := hour*60 + minute
	if key == d.lastRenderedClockKey {
		return
	}
	hTens, hOnes := splitDigits(hour)
	mTens, mOnes := splitDigits(minute)
	if err := d.panel.SetClockDigits(hTens, hOnes, mTens, mOnes, brightness); err != nil {
		log.Printf("clock update failed: %v", err)
		return
	}
	d.lastRenderedClockKey = key
}

func (d *Daemon) maybeHourBounce(now time.Time, peakBrightness uint8) {
	hour := now.Hour()
	if d.lastRenderedHour != -1 && hour != d.lastRenderedHour {
		if err := d.panel.HourBounce("both_constellations", peakBrightness); err != nil {
			log.Printf("hour bounce failed: %v", err)
		}
	}
	d.lastRenderedHour = hour
}

func splitDigits(n int) (tens, ones int) {
	return n / 10, n % 10
}
