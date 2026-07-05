package tui

import (
	"testing"

	tea "github.com/charmbracelet/bubbletea"

	"github.com/6sRyuSK/tatsunari-sounds/tools/installer/internal/app"
	"github.com/6sRyuSK/tatsunari-sounds/tools/installer/internal/model"
)

// atPlugins returns a model that has completed discovery and is on the plugin
// selection screen.
func atPlugins(t *testing.T) Model {
	t.Helper()
	t.Setenv("HOME", "/Users/tester")
	m := New(nil, model.OSMacOS)
	m = step(t, m, discoveredMsg{cat: fakeCatalog()})
	if m.screen != screenPlugins {
		t.Fatalf("setup: screen = %v, want plugins", m.screen)
	}
	return m
}

// atConfirm walks the flow to the confirm screen with resonance-suppressor
// selected (pre-selected because it has an update) and both formats on.
func atConfirm(t *testing.T) Model {
	t.Helper()
	m := atPlugins(t)
	m = step(t, m, keyPress("enter")) // -> formats
	m = step(t, m, keyPress("enter")) // -> scope
	m = step(t, m, keyPress("enter")) // -> confirm
	if m.screen != screenConfirm {
		t.Fatalf("setup: screen = %v, want confirm", m.screen)
	}
	return m
}

func TestQuitFromEachScreen(t *testing.T) {
	for _, key := range []string{"q", "ctrl+c"} {
		m := atPlugins(t)
		var msg tea.KeyMsg
		if key == "ctrl+c" {
			msg = tea.KeyMsg{Type: tea.KeyCtrlC}
		} else {
			msg = keyPress(key)
		}
		next, cmd := m.Update(msg)
		nm := next.(Model)
		if !nm.quitting {
			t.Errorf("%s: expected quitting=true", key)
		}
		if cmd == nil {
			t.Errorf("%s: expected a quit command", key)
		}
	}
}

func TestQuitBlockedDuringProgress(t *testing.T) {
	m := atPlugins(t)
	m.screen = screenProgress
	next, _ := m.Update(keyPress("q"))
	nm := next.(Model)
	if nm.quitting {
		t.Error("quit must be ignored during progress")
	}
	if nm.screen != screenProgress {
		t.Errorf("screen = %v, want progress", nm.screen)
	}
}

func TestWindowSizeMsg(t *testing.T) {
	m := atPlugins(t)
	next, _ := m.Update(tea.WindowSizeMsg{Width: 100, Height: 40})
	nm := next.(Model)
	if nm.width != 100 || nm.height != 40 {
		t.Errorf("size = %dx%d, want 100x40", nm.width, nm.height)
	}
}

func TestPluginCursorAndToggle(t *testing.T) {
	m := atPlugins(t)
	list := m.installablePlugins()
	if len(list) < 2 {
		t.Fatalf("need >=2 installable plugins, got %d", len(list))
	}

	// Up at the top is a no-op.
	m = step(t, m, keyPress("up"))
	if m.cursor != 0 {
		t.Errorf("cursor = %d after up at top, want 0", m.cursor)
	}
	// Down moves.
	m = step(t, m, keyPress("down"))
	if m.cursor != 1 {
		t.Errorf("cursor = %d after down, want 1", m.cursor)
	}
	// Down cannot exceed the last row.
	for i := 0; i < 10; i++ {
		m = step(t, m, keyPress("down"))
	}
	if m.cursor != len(list)-1 {
		t.Errorf("cursor = %d, want clamped to %d", m.cursor, len(list)-1)
	}
	// Toggle flips the plugin under the cursor.
	slug := list[m.cursor].Slug
	before := m.selected[slug]
	m = step(t, m, keyPress("space"))
	if m.selected[slug] == before {
		t.Errorf("toggle did not flip %q", slug)
	}
}

func TestPluginNextBlockedWhenNothingSelected(t *testing.T) {
	m := atPlugins(t)
	// Clear the pre-selection.
	for k := range m.selected {
		m.selected[k] = false
	}
	m = step(t, m, keyPress("enter"))
	if m.screen != screenPlugins {
		t.Errorf("with nothing selected, Next must stay on plugins; got %v", m.screen)
	}
}

func TestFormatsToggleBackAndNextGuard(t *testing.T) {
	m := atPlugins(t)
	m = step(t, m, keyPress("enter")) // -> formats
	if m.screen != screenFormats {
		t.Fatalf("screen = %v, want formats", m.screen)
	}

	// Back returns to plugins.
	back := step(t, m, keyPress("esc"))
	if back.screen != screenPlugins {
		t.Errorf("esc on formats should go back to plugins, got %v", back.screen)
	}

	// Cursor down then toggle everything off, Next must be blocked.
	m = step(t, m, keyPress("down"))
	if m.formatCursor != 1 {
		t.Errorf("formatCursor = %d, want 1", m.formatCursor)
	}
	for _, f := range m.formatOpts {
		m.formatOn[f] = false
	}
	m = step(t, m, keyPress("enter"))
	if m.screen != screenFormats {
		t.Errorf("Next with no format on must stay on formats; got %v", m.screen)
	}

	// Toggle the current one on, Next proceeds to scope.
	m = step(t, m, keyPress("space"))
	m = step(t, m, keyPress("enter"))
	if m.screen != screenScope {
		t.Errorf("Next with a format on should go to scope; got %v", m.screen)
	}
}

func TestScopeNavigation(t *testing.T) {
	m := atPlugins(t)
	m = step(t, m, keyPress("enter")) // formats
	m = step(t, m, keyPress("enter")) // scope
	if m.screen != screenScope {
		t.Fatalf("screen = %v, want scope", m.screen)
	}

	// Default cursor is on the current scope (system = index 0).
	if m.scopeCursor != 0 {
		t.Errorf("scopeCursor = %d, want 0", m.scopeCursor)
	}
	// Down selects user (index 1); up clamps back to 0.
	m = step(t, m, keyPress("down"))
	if m.scopeCursor != 1 {
		t.Errorf("scopeCursor = %d, want 1", m.scopeCursor)
	}
	m = step(t, m, keyPress("down")) // clamp at last
	if m.scopeCursor != len(scopeOrder)-1 {
		t.Errorf("scopeCursor = %d, want clamped", m.scopeCursor)
	}

	// Back returns to formats.
	back := step(t, m, keyPress("esc"))
	if back.screen != screenFormats {
		t.Errorf("esc on scope should go to formats; got %v", back.screen)
	}

	// Enter commits the chosen scope and builds the plan.
	m = step(t, m, keyPress("enter"))
	if m.screen != screenConfirm {
		t.Fatalf("screen = %v, want confirm", m.screen)
	}
	if m.scope != model.ScopeUser {
		t.Errorf("scope = %q, want user", m.scope)
	}
}

func TestConfirmBackAndBeginInstall(t *testing.T) {
	m := atConfirm(t)

	// Back returns to scope.
	back := step(t, m, keyPress("esc"))
	if back.screen != screenScope {
		t.Errorf("esc on confirm should go to scope; got %v", back.screen)
	}

	// Enter begins the install: transition to progress with totals primed.
	m = step(t, m, keyPress("enter"))
	if m.screen != screenProgress {
		t.Fatalf("screen = %v, want progress", m.screen)
	}
	if m.total != len(m.items) {
		t.Errorf("total = %d, want %d", m.total, len(m.items))
	}
	if m.installCh == nil {
		t.Error("beginInstall should set the install channel")
	}
}

func TestProgressAndDoneTransition(t *testing.T) {
	m := atConfirm(t)
	m = step(t, m, keyPress("enter")) // -> progress
	total := m.total

	// A done event advances the completed counter and logs a line.
	item := m.items[0]
	m = step(t, m, progressMsg(app.ProgressEvent{Item: item, Phase: app.PhaseDone}))
	if m.done != 1 {
		t.Errorf("done = %d, want 1", m.done)
	}
	if len(m.logLines) != 1 {
		t.Errorf("logLines = %d, want 1", len(m.logLines))
	}

	// An error event also counts as completed and is flagged.
	m = step(t, m, progressMsg(app.ProgressEvent{Item: item, Phase: app.PhaseError, Err: errFake{}}))
	if m.done != 2 {
		t.Errorf("done = %d, want 2", m.done)
	}
	if !m.logLines[len(m.logLines)-1].err {
		t.Error("error event should set err flag on log line")
	}
	_ = total

	// installDoneMsg moves to the summary screen and records the result.
	m = step(t, m, installDoneMsg{result: model.ApplyResult{Installed: []string{"/x.vst3"}}})
	if m.screen != screenSummary {
		t.Fatalf("screen = %v, want summary", m.screen)
	}
	if len(m.result.Installed) != 1 {
		t.Errorf("result not recorded: %+v", m.result)
	}
}

func TestRestartFromSummary(t *testing.T) {
	m := atConfirm(t)
	m = step(t, m, keyPress("enter")) // progress
	m = step(t, m, installDoneMsg{result: model.ApplyResult{}, err: errFake{}})
	if m.screen != screenSummary {
		t.Fatalf("screen = %v, want summary", m.screen)
	}
	// 'n' starts a new install: back to plugins with cleared state.
	m = step(t, m, keyPress("n"))
	if m.screen != screenPlugins {
		t.Errorf("restart should return to plugins; got %v", m.screen)
	}
	if m.logLines != nil || m.runErr != nil {
		t.Error("restart should clear progress state")
	}
}

func TestRetryAfterDiscoverError(t *testing.T) {
	t.Setenv("HOME", "/Users/tester")
	m := New(nil, model.OSMacOS)
	m = step(t, m, discoveredMsg{err: errFake{}})
	if m.screen != screenDiscover || m.loadErr == nil {
		t.Fatalf("expected discover error state; screen=%v err=%v", m.screen, m.loadErr)
	}
	// 'r' retries: clears the error and re-issues discovery.
	next, cmd := m.Update(keyPress("r"))
	nm := next.(Model)
	if nm.loadErr != nil {
		t.Error("retry should clear loadErr")
	}
	if cmd == nil {
		t.Error("retry should issue a discovery command")
	}
}

func TestViewsRenderAcrossScreens(t *testing.T) {
	m := atPlugins(t)
	for _, sc := range []screen{screenDiscover, screenPlugins, screenFormats, screenScope, screenConfirm, screenProgress, screenSummary} {
		m.screen = sc
		if m.View() == "" {
			t.Errorf("View for screen %v is empty", sc)
		}
	}
}

func TestWindowsOffersOnlyVST3(t *testing.T) {
	m := New(nil, model.OSWindows)
	if got := m.osFormats(); len(got) != 1 || got[0] != model.FormatVST3 {
		t.Errorf("Windows osFormats = %v, want [VST3]", got)
	}
}
