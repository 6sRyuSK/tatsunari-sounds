package main

import (
	"fmt"
	"io"
	"os"

	tea "github.com/charmbracelet/bubbletea"

	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/release"
	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/tui"
)

// runTUI launches the interactive Bubble Tea installer.
func runTUI(opts options) int {
	if opts.targetOS == "" {
		fmt.Fprintln(os.Stderr, "the TUI supports macOS and Windows only; on other systems use --no-tui --os <os>")
		return 2
	}
	client := release.NewClient(release.DefaultOwner, release.DefaultRepo, os.Getenv("GITHUB_TOKEN"))

	teaOpts := []tea.ProgramOption{tea.WithAltScreen()}

	// When launched via `curl … | bash` or `irm … | iex`, our stdin is the
	// script pipe, not the keyboard. Reattach the controlling terminal so the
	// TUI can read input.
	if in, closeTTY, ok := controllingTTY(); ok {
		teaOpts = append(teaOpts, tea.WithInput(in))
		defer closeTTY()
	}

	p := tea.NewProgram(tui.New(client, opts.targetOS), teaOpts...)
	if _, err := p.Run(); err != nil {
		fmt.Fprintln(os.Stderr, "tui error:", err)
		return 1
	}
	return 0
}

// controllingTTY returns the controlling terminal as an input reader when
// stdin is not itself a terminal (the pipe case). ok is false when stdin is
// already a TTY (use the default) or no controlling terminal is available.
func controllingTTY() (in io.Reader, closeFn func(), ok bool) {
	if stdinIsTTY() {
		return nil, nil, false
	}
	f, err := openTTY()
	if err != nil {
		return nil, nil, false
	}
	return f, func() { _ = f.Close() }, true
}

// stdinIsTTY reports whether os.Stdin is a character device (a terminal),
// rather than a pipe or file.
func stdinIsTTY() bool {
	fi, err := os.Stdin.Stat()
	if err != nil {
		return false
	}
	return fi.Mode()&os.ModeCharDevice != 0
}
