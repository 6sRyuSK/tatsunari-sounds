package tui

import (
	"testing"

	tea "github.com/charmbracelet/bubbletea"

	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/model"
	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/release"
)

func fakeCatalog() release.Catalog {
	manifest := map[string]string{"resonance-suppressor": "0.2.1", "nam-player": "0.1.0"}
	assets := release.ParsePluginAssets(&release.Release{Tag: "2026.2", Assets: []model.Asset{
		{Name: "resonance-suppressor-v0_2_1-macOS-VST3.zip", DownloadURL: "https://x/rs"},
		{Name: "resonance-suppressor-v0_2_1-macOS-AU.zip", DownloadURL: "https://x/rsau"},
		{Name: "nam-player-v0_1_0-macOS-VST3.zip", DownloadURL: "https://x/nam"},
	}})
	installed := map[string]string{"resonance-suppressor": "0.2.0"} // update available
	return release.Reconcile("2026.2", manifest, assets, nil, installed, nil)
}

func keyPress(s string) tea.KeyMsg {
	switch s {
	case "enter":
		return tea.KeyMsg{Type: tea.KeyEnter}
	case "space":
		return tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune{' '}}
	default:
		return tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune(s)}
	}
}

func step(t *testing.T, m tea.Model, msg tea.Msg) Model {
	t.Helper()
	next, _ := m.Update(msg)
	return next.(Model)
}

func TestFlowDiscoverToConfirm(t *testing.T) {
	t.Setenv("HOME", "/Users/tester")

	m := New(nil, model.OSMacOS)
	// Discovery completes.
	m = step(t, m, discoveredMsg{cat: fakeCatalog()})
	if m.screen != screenPlugins {
		t.Fatalf("after discovery, screen = %v, want plugins", m.screen)
	}
	// resonance-suppressor has an update -> pre-selected; nam-player not.
	if !m.selected["resonance-suppressor"] {
		t.Error("update-available plugin should be pre-selected")
	}
	if m.selected["nam-player"] {
		t.Error("fresh plugin should not be pre-selected")
	}

	// enter -> formats (a selection already exists)
	m = step(t, m, keyPress("enter"))
	if m.screen != screenFormats {
		t.Fatalf("screen = %v, want formats", m.screen)
	}
	if len(m.formatOpts) != 2 {
		t.Errorf("macOS should offer VST3+AU, got %v", m.formatOpts)
	}

	// enter -> scope
	m = step(t, m, keyPress("enter"))
	if m.screen != screenScope {
		t.Fatalf("screen = %v, want scope", m.screen)
	}

	// enter -> confirm, plan built
	m = step(t, m, keyPress("enter"))
	if m.screen != screenConfirm {
		t.Fatalf("screen = %v, want confirm", m.screen)
	}
	if len(m.items) == 0 {
		t.Fatal("confirm screen should have plan items")
	}
	for _, it := range m.items {
		if it.Slug != "resonance-suppressor" {
			t.Errorf("unexpected item slug %q", it.Slug)
		}
		if it.Action != "update" {
			t.Errorf("action = %q, want update", it.Action)
		}
	}
	// View should render without panicking.
	if m.View() == "" {
		t.Error("confirm view is empty")
	}
}

func TestSelectAllUpdatable(t *testing.T) {
	t.Setenv("HOME", "/Users/tester")
	m := New(nil, model.OSMacOS)
	m = step(t, m, discoveredMsg{cat: fakeCatalog()})

	// Deselect everything by toggling the pre-selected one off, then 'a'.
	m.selected["resonance-suppressor"] = false
	m = step(t, m, keyPress("a"))
	if !m.selected["resonance-suppressor"] {
		t.Error("'a' should select all updatable plugins")
	}
}

func TestDiscoverErrorStaysOnScreen(t *testing.T) {
	m := New(nil, model.OSWindows)
	m = step(t, m, discoveredMsg{err: errFake{}})
	if m.screen != screenDiscover {
		t.Errorf("on discovery error, should stay on discover; got %v", m.screen)
	}
	if m.View() == "" {
		t.Error("error view should render")
	}
}

type errFake struct{}

func (errFake) Error() string { return "boom" }
