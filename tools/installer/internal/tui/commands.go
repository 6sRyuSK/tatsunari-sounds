package tui

import (
	"context"

	tea "github.com/charmbracelet/bubbletea"

	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/app"
	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/install"
	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/model"
	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/release"
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
	result    model.ApplyResult
	installed []app.InstalledItem
	err       error
}

// discoverCmd fetches the latest release and reconciles with the local receipt.
func discoverCmd(c *release.Client) tea.Cmd {
	return func() tea.Msg {
		var installed map[string]string
		if rec, err := install.LoadReceipt(); err == nil {
			installed = rec.InstalledVersions()
		}
		cat, err := app.Discover(context.Background(), c, installed)
		return discoveredMsg{cat: cat, err: err}
	}
}

// startInstall launches the install worker, streaming progress into ch and a
// final installDoneMsg. The elevation prompt (if system scope) happens inside
// the worker, off the UI goroutine.
func startInstall(ch chan tea.Msg, installer *app.Installer, items []model.PlanItem, scope model.Scope, versionOf map[string]string) tea.Cmd {
	return func() tea.Msg {
		go func() {
			res, installed, err := installer.Run(context.Background(), items, scope, func(ev app.ProgressEvent) {
				ch <- progressMsg(ev)
			})
			if err == nil && len(installed) > 0 {
				_ = app.WriteReceipt(installed, versionOf)
			}
			ch <- installDoneMsg{result: res, installed: installed, err: err}
			close(ch)
		}()
		return waitForMsg(ch)()
	}
}

// waitForMsg reads the next streamed message.
func waitForMsg(ch chan tea.Msg) tea.Cmd {
	return func() tea.Msg { return <-ch }
}
