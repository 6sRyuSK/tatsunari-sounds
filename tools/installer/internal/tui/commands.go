package tui

import (
	"context"

	tea "github.com/charmbracelet/bubbletea"

	"github.com/6sRyuSK/tatsunari-sounds/tools/installer/internal/app"
	"github.com/6sRyuSK/tatsunari-sounds/tools/installer/internal/install"
	"github.com/6sRyuSK/tatsunari-sounds/tools/installer/internal/model"
	"github.com/6sRyuSK/tatsunari-sounds/tools/installer/internal/release"
)

// discoveredMsg carries the result of release discovery.
type discoveredMsg struct {
	cat release.Catalog
	err error
}

// progressMsg is one install step update streamed from the worker goroutine.
type progressMsg app.ProgressEvent

// installDoneMsg is the terminal message of an install run.
type installDoneMsg struct {
	result model.ApplyResult
	err    error
}

// discoverCmd fetches the latest release and reconciles with local receipts
// (user + system projected to slug→version for the legacy reconcile path).
func discoverCmd(c *release.Client, osID model.OS) tea.Cmd {
	return func() tea.Msg {
		var installed map[string]string
		if rec, err := install.LoadAllReceipts(osID); err == nil {
			installed = rec.InstalledVersions()
		} else if rec, err := install.LoadReceipt(); err == nil {
			installed = rec.InstalledVersions()
		}
		cat, err := app.Discover(context.Background(), c, installed)
		return discoveredMsg{cat: cat, err: err}
	}
}

// startInstall launches the install worker, streaming progress into ch and a
// final installDoneMsg. The elevation prompt (if system scope) happens inside
// the worker, off the UI goroutine. PlanItem.Version/Variant/Scope drive the
// receipt; versionOf maps are no longer required.
func startInstall(ch chan tea.Msg, installer *app.Installer, items []model.PlanItem, scope model.Scope) tea.Cmd {
	return func() tea.Msg {
		go func() {
			res, installed, err := installer.Run(context.Background(), items, scope, func(ev app.ProgressEvent) {
				ch <- progressMsg(ev)
			})
			// Recorded even when err != nil: Run can partially apply (one scope
			// succeeded, the other's elevation was cancelled), and an
			// unrecorded install is invisible to the next run.
			if len(installed) > 0 {
				_ = app.WriteReceipt(installer.OS, installed)
			}
			ch <- installDoneMsg{result: res, err: err}
			close(ch)
		}()
		return waitForMsg(ch)()
	}
}

// waitForMsg reads the next streamed message.
func waitForMsg(ch chan tea.Msg) tea.Cmd {
	return func() tea.Msg { return <-ch }
}
