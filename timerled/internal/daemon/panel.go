// panel.go wraps the serial port with the higher-level operations the state
// machine needs, each one just formatting a command string and sending it.
package daemon

import (
	"fmt"
	"strings"
	"time"

	"github.com/Ozdotdotdot/geekbar-pulse-x-led-driver/timerled/internal/leds"
	"github.com/Ozdotdotdot/geekbar-pulse-x-led-driver/timerled/internal/serialport"
)

const sendTimeout = 20 * time.Second

// Brightness levels the constellation is held at while playing vs paused,
// and how long the ease between them takes.
const (
	constellationFullBrightness   = 230
	constellationPausedBrightness = 70
	constellationEaseMs           = 900
)

// The stray-star twinkle's brightness range, and how long it takes to fade
// out on pause / back in on resume (the fade-in is handled on-device by the
// firmware's twinkle envelope when K re-arms it; the fade-out is driven
// from here via "E" since there's nothing else keeping the stars animating
// once the twinkle is disarmed).
const (
	starTwinkleMinBrightness = 1
	starTwinkleMaxBrightness = 45
	starFadeMs               = 900
)

type Panel struct {
	port *serialport.Port
}

func NewPanel(port *serialport.Port) *Panel {
	return &Panel{port: port}
}

func (p *Panel) send(line string) error {
	return p.port.Send(line, sendTimeout)
}

// Close releases the underlying serial port.
func (p *Panel) Close() error {
	return p.port.Close()
}

func (p *Panel) Clear() error { return p.send("C") }

// TraceBreatheReveal is the "B" trace-and-breathe reveal used both for
// entering Regular and for a fresh track in Music mode.
func (p *Panel) TraceBreatheReveal(group string, peakBrightness uint8) error {
	return p.send(fmt.Sprintf("B 7 12 450 450 %d %s", peakBrightness, group))
}

// HourBounce is the 5x bounce-and-settle that replays on the hour, ending
// held at peakBrightness -- callers pass the brightness the group should
// settle back to (full brightness in Regular/Music-playing, the dimmed
// pause level in Music-paused, so the bounce doesn't undo the pause dim).
func (p *Panel) HourBounce(group string, peakBrightness uint8) error {
	return p.send(fmt.Sprintf("Y 5 35 6 9 %d %s", peakBrightness, group))
}

func (p *Panel) ArmTwinkle(group string, minB, maxB uint8) error {
	return p.send(fmt.Sprintf("K %d %d %s", minB, maxB, group))
}

func (p *Panel) DisarmTwinkle() error {
	return p.send("K off")
}

// EaseGroup smoothly ramps every LED in a held group from fromB to toB over
// ms, on-device, without clearing anything else -- used to dim/undim a
// constellation on pause/resume instead of an instant step.
func (p *Panel) EaseGroup(group string, fromB, toB uint8, ms int) error {
	return p.send(fmt.Sprintf("E %d %d %d %s", fromB, toB, ms, group))
}

// EaseGroupFromCurrent is like EaseGroup, but each LED fades from its own
// actual current brightness (read on-device out of frame[]) rather than a
// shared starting value -- needed for fading out a group that was
// mid-animation (e.g. each star at a different point in its own twinkle
// cycle), so nothing snaps to a uniform brightness before the fade starts.
func (p *Panel) EaseGroupFromCurrent(group string, toB uint8, ms int) error {
	return p.send(fmt.Sprintf("V %d %d %s", toB, ms, group))
}

// RingSweep is a one-shot "S" radar/second-hand sweep around the ring --
// used for the dashboard's fire-and-forget "sweep" trigger.
func (p *Panel) RingSweep(ms int, brightness uint8, decayPercent int, laps int) error {
	return p.send(fmt.Sprintf("S %d %d %d %d", ms, brightness, decayPercent, laps))
}

// RingProgress sets 0-14 ring LEDs filled at the given brightness.
func (p *Panel) RingProgress(filledCount int, brightness uint8) error {
	if filledCount < 0 {
		filledCount = 0
	}
	if filledCount > 14 {
		filledCount = 14
	}
	return p.send(fmt.Sprintf("J %d %d", filledCount, brightness))
}

func (p *Panel) FinishBlink() error {
	return p.send("H 2 250 200 all")
}

// SetIcon turns the X icon fully on or off via the non-destructive U
// command, leaving everything else in frame[] untouched.
func (p *Panel) SetIcon(on bool) error {
	b := uint8(0)
	if on {
		b = constellationPausedBrightness
	}
	var sb strings.Builder
	sb.WriteString("U")
	for _, idx := range leds.XIcon {
		fmt.Fprintf(&sb, " %d %d", idx, b)
	}
	return p.send(sb.String())
}

// SetClockDigits renders HH:MM or MM:SS (tens/ones already split by the
// caller) into all four digit slots via "U", without clearing anything else
// resident in frame[]. Pass -1 for a slot to blank it.
func (p *Panel) SetClockDigits(topA, topB, bottomA, bottomB int, brightness uint8) error {
	var sb strings.Builder
	sb.WriteString("U")
	appendDigit := func(slot [7]uint16, digit int) {
		indices, brightnesses := leds.DigitPixels(slot, digit, brightness)
		for i, idx := range indices {
			fmt.Fprintf(&sb, " %d %d", idx, brightnesses[i])
		}
	}
	appendDigit(leds.DigitTopA, topA)
	appendDigit(leds.DigitTopB, topB)
	appendDigit(leds.DigitBottomA, bottomA)
	appendDigit(leds.DigitBottomB, bottomB)
	return p.send(sb.String())
}
