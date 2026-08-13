// Package serialport talks to the ESP32 firmware's newline-terminated
// command protocol: write a line, wait for the "OK" acknowledgement it
// prints after handling each command. Mirrors the cmd() helper in
// scripts/demo.py.
package serialport

import (
	"bufio"
	"fmt"
	"strings"
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
	time.Sleep(2 * time.Second)
	sp.ResetInputBuffer()
	return &Port{port: sp, reader: bufio.NewReader(sp)}, nil
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
