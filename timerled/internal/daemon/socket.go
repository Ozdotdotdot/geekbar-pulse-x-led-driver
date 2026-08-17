package daemon

import (
	"bufio"
	"fmt"
	"log"
	"net"
	"os"
	"strconv"
	"strings"
	"time"
)

// serveSocket accepts client connections and turns each request line into
// a cmdRequest on d.cmds, writing the reply back before closing. One line
// per connection, matching how the CLI subcommands are meant to be used
// (short-lived, not an interactive session).
func (d *Daemon) serveSocket(ln net.Listener) {
	for {
		conn, err := ln.Accept()
		if err != nil {
			return
		}
		go d.handleConn(conn)
	}
}

func (d *Daemon) handleConn(conn net.Conn) {
	defer conn.Close()
	conn.SetReadDeadline(time.Now().Add(5 * time.Second))

	line, err := bufio.NewReader(conn).ReadString('\n')
	if err != nil && line == "" {
		return
	}
	line = strings.TrimSpace(line)

	req, err := parseRequest(line)
	if err != nil {
		fmt.Fprintln(conn, "error: "+err.Error())
		return
	}

	// The daemon's main loop can be mid blocking animation (a reveal or
	// bounce can run several seconds) when this request arrives, so the
	// wait for a reply needs real headroom -- a tight deadline here would
	// make an already-successful command look like it failed to the CLI
	// (write past the deadline -> EOF on the client), even though the
	// daemon processed it fine.
	conn.SetWriteDeadline(time.Now().Add(30 * time.Second))
	d.cmds <- req
	reply := <-req.reply
	fmt.Fprintln(conn, reply)
}

func parseRequest(line string) (cmdRequest, error) {
	fields := strings.Fields(line)
	if len(fields) == 0 {
		return cmdRequest{}, fmt.Errorf("empty command")
	}
	reply := make(chan string, 1)
	switch cmdVerb(fields[0]) {
	case verbPomodoro:
		if len(fields) != 2 {
			return cmdRequest{}, fmt.Errorf("usage: pomodoro <duration>")
		}
		dur, err := time.ParseDuration(fields[1])
		if err != nil {
			return cmdRequest{}, fmt.Errorf("bad duration %q: %w", fields[1], err)
		}
		return cmdRequest{verb: verbPomodoro, dur: dur, reply: reply}, nil
	case verbCancel, verbOff, verbNormal, verbStatus, verbSnapshot:
		return cmdRequest{verb: cmdVerb(fields[0]), reply: reply}, nil
	case verbBrightness:
		if len(fields) != 3 {
			return cmdRequest{}, fmt.Errorf("usage: brightness <stars|constellation|clock> <0-255>")
		}
		val, err := strconv.Atoi(fields[2])
		if err != nil || val < 0 || val > 255 {
			return cmdRequest{}, fmt.Errorf("bad brightness value %q (want 0-255)", fields[2])
		}
		return cmdRequest{verb: verbBrightness, target: fields[1], value: uint8(val), reply: reply}, nil
	case verbTrigger:
		if len(fields) != 2 {
			return cmdRequest{}, fmt.Errorf("usage: trigger <bounce|reveal|sweep|blink>")
		}
		return cmdRequest{verb: verbTrigger, trigger: fields[1], reply: reply}, nil
	default:
		return cmdRequest{}, fmt.Errorf("unknown command %q", fields[0])
	}
}

// listen removes any stale socket file left behind by a crashed previous
// run before binding -- otherwise net.Listen fails with "address already
// in use" even though nothing is listening anymore.
func listen(socketPath string) (net.Listener, error) {
	if _, err := os.Stat(socketPath); err == nil {
		if _, dialErr := net.Dial("unix", socketPath); dialErr != nil {
			log.Printf("removing stale socket %s", socketPath)
			os.Remove(socketPath)
		}
	}
	return net.Listen("unix", socketPath)
}
