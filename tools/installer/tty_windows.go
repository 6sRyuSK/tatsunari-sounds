//go:build windows

package main

import "os"

// openTTY opens the Windows console input buffer so the TUI can read the
// keyboard when stdin is a pipe (the `irm … | iex` case).
func openTTY() (*os.File, error) {
	return os.Open("CONIN$")
}
