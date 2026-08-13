// timerled drives the Geekbar LED panel as a real desk fixture: idle by
// default, expressive during MPD playback, a single-purpose pomodoro
// countdown on request, dark when the machine sleeps.
//
// `timerled daemon` runs the long-running process that owns the serial
// port. Every other subcommand is a thin client that talks to the running
// daemon over a Unix socket -- see internal/proto.
package main

import (
	"bufio"
	"context"
	"fmt"
	"net"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/Ozdotdotdot/geekbar-pulse-x-led-driver/timerled/internal/daemon"
	"github.com/Ozdotdotdot/geekbar-pulse-x-led-driver/timerled/internal/proto"
)

func main() {
	if len(os.Args) < 2 {
		usage()
		os.Exit(2)
	}

	switch os.Args[1] {
	case "daemon":
		runDaemon()
	case "pomodoro":
		if len(os.Args) != 3 {
			fmt.Fprintln(os.Stderr, "usage: timerled pomodoro <duration>  (e.g. 25m, 1h20m)")
			os.Exit(2)
		}
		sendClient("pomodoro " + os.Args[2])
	case "cancel", "status", "off", "normal":
		sendClient(os.Args[1])
	default:
		usage()
		os.Exit(2)
	}
}

func usage() {
	fmt.Fprintln(os.Stderr, `usage:
  timerled daemon                run the daemon (owns the serial port)
  timerled pomodoro <duration>   start a countdown, e.g. "timerled pomodoro 25m"
  timerled cancel                cancel an active pomodoro
  timerled status                print the current mode
  timerled off                   force all LEDs off until "normal"
  timerled normal                resume automatic regular/music state`)
}

func runDaemon() {
	portName := envOr("TIMERLED_PORT", "/dev/ttyUSB0")
	baud := 115200

	d, err := daemon.New(portName, baud, proto.SocketPath())
	if err != nil {
		fmt.Fprintf(os.Stderr, "timerled: %v\n", err)
		os.Exit(1)
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	if err := d.Run(ctx); err != nil {
		fmt.Fprintf(os.Stderr, "timerled: %v\n", err)
		os.Exit(1)
	}
}

func sendClient(line string) {
	conn, err := net.DialTimeout("unix", proto.SocketPath(), 3*time.Second)
	if err != nil {
		fmt.Fprintf(os.Stderr, "timerled: could not reach daemon (is it running? %v)\n", err)
		os.Exit(1)
	}
	defer conn.Close()

	conn.SetDeadline(time.Now().Add(30 * time.Second))
	if _, err := fmt.Fprintln(conn, line); err != nil {
		fmt.Fprintf(os.Stderr, "timerled: %v\n", err)
		os.Exit(1)
	}

	reply, err := bufio.NewReader(conn).ReadString('\n')
	if err != nil && reply == "" {
		fmt.Fprintf(os.Stderr, "timerled: %v\n", err)
		os.Exit(1)
	}
	fmt.Print(reply)
}

func envOr(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}
