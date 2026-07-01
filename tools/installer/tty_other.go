//go:build !windows

package main

import "os"

// openTTY opens the POSIX controlling terminal for reading keyboard input when
// stdin is a pipe (the `curl … | bash` case).
func openTTY() (*os.File, error) {
	return os.Open("/dev/tty")
}
