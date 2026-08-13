// Package proto defines the tiny plaintext protocol between the timerled
// CLI and the running daemon, spoken over a Unix socket -- one command per
// line in, one text response line out.
package proto

import "os"

// SocketEnvOverride, if set, takes precedence over DefaultSocketPath --
// mainly for running multiple daemons side by side during development.
const SocketEnvOverride = "TIMERLED_SOCKET"

// SocketPath returns the socket path to use: $TIMERLED_SOCKET if set,
// otherwise $XDG_RUNTIME_DIR/timerled.sock, falling back to /tmp if neither
// is set (e.g. run outside a login session).
func SocketPath() string {
	if v := os.Getenv(SocketEnvOverride); v != "" {
		return v
	}
	if v := os.Getenv("XDG_RUNTIME_DIR"); v != "" {
		return v + "/timerled.sock"
	}
	return "/tmp/timerled.sock"
}
