// Package sleepwatch subscribes to logind's PrepareForSleep signal on the
// system D-Bus so the daemon can blank the panel before suspend and redraw
// it on resume, without any root-owned /usr/lib/systemd/system-sleep script
// -- a normal user session can receive system-bus signals without special
// privileges.
package sleepwatch

import (
	"github.com/godbus/dbus/v5"
)

// Watch connects to the system bus and returns a channel that receives
// true right before the machine suspends and false right after it resumes.
// The caller owns the returned Conn and should Close it on shutdown.
func Watch() (<-chan bool, *dbus.Conn, error) {
	conn, err := dbus.ConnectSystemBus()
	if err != nil {
		return nil, nil, err
	}

	matchRule := "type='signal',interface='org.freedesktop.login1.Manager',member='PrepareForSleep'"
	if err := conn.BusObject().Call("org.freedesktop.DBus.AddMatch", 0, matchRule).Err; err != nil {
		conn.Close()
		return nil, nil, err
	}

	signals := make(chan *dbus.Signal, 8)
	conn.Signal(signals)

	out := make(chan bool, 8)
	go func() {
		for sig := range signals {
			if sig.Name != "org.freedesktop.login1.Manager.PrepareForSleep" {
				continue
			}
			if len(sig.Body) != 1 {
				continue
			}
			if sleeping, ok := sig.Body[0].(bool); ok {
				out <- sleeping
			}
		}
		close(out)
	}()

	return out, conn, nil
}
