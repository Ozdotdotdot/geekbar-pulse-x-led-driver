// Package serialport talks to the ESP32 firmware's newline-terminated
// command protocol: write a line, wait for the "OK" acknowledgement it
// prints after handling each command. Mirrors the cmd() helper in
// scripts/demo.py.
package serialport

import (
	"bufio"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"

	"go.bug.st/serial"
)

type Port struct {
	port   serial.Port
	reader *bufio.Reader
}

// Open opens the serial port and waits out the ESP32's post-connect reset,
// same as demo.py's 2s sleep + reset_input_buffer.
func Open(name string, baud int) (*Port, error) {
	mode := &serial.Mode{BaudRate: baud}
	sp, err := serial.Open(name, mode)
	if err != nil {
		return nil, fmt.Errorf("open %s: %w", name, err)
	}
	markCloseOnExec(name)
	time.Sleep(2 * time.Second)
	sp.ResetInputBuffer()
	return &Port{port: sp, reader: bufio.NewReader(sp)}, nil
}

// markCloseOnExec finds the fd go.bug.st/serial just opened for name (it
// doesn't set O_CLOEXEC itself) and marks it close-on-exec. Without this,
// every "mpc ..." subprocess the daemon forks inherits the raw serial fd,
// including the long-lived "mpc idle player" watcher -- which then pins the
// old fd open across a suspend/resume cycle, so the daemon's post-resume
// reopen fails with "device busy" even though it closed its own handle.
func markCloseOnExec(name string) {
	target, err := filepath.EvalSymlinks(name)
	if err != nil {
		target = name
	}
	entries, err := os.ReadDir("/proc/self/fd")
	if err != nil {
		return
	}
	for _, e := range entries {
		fd, err := strconv.Atoi(e.Name())
		if err != nil {
			continue
		}
		link, err := os.Readlink(filepath.Join("/proc/self/fd", e.Name()))
		if err != nil {
			continue
		}
		if link == target || link == name {
			syscall.CloseOnExec(fd)
		}
	}
}

func (p *Port) Close() error {
	return p.port.Close()
}

// Send writes a command line and blocks until "OK" is seen or timeout
// elapses. Non-fatal on timeout -- callers decide whether a missed ack
// matters (it doesn't for most animation commands: worst case the daemon's
// next tick just retries a fresher command).
func (p *Port) Send(line string, timeout time.Duration) error {
	if _, err := p.port.Write([]byte(line + "\n")); err != nil {
		return fmt.Errorf("write %q: %w", line, err)
	}
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		p.port.SetReadTimeout(100 * time.Millisecond)
		resp, err := p.reader.ReadString('\n')
		if err == nil && strings.Contains(resp, "OK") {
			return nil
		}
	}
	return fmt.Errorf("no OK for %q within %s", line, timeout)
}
