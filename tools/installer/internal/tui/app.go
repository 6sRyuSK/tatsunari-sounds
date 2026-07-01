// Package tui implements the interactive Bubble Tea installer: discover ->
// select plugins -> select formats -> select scope -> confirm -> progress ->
// summary. It is driven by the shared orchestration in internal/app.
package tui

import (
	"github.com/charmbracelet/bubbles/help"
	"github.com/charmbracelet/bubbles/progress"
	"github.com/charmbracelet/bubbles/spinner"
	tea "github.com/charmbracelet/bubbletea"

	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/app"
	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/i18n"
	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/model"
	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/release"
)

type screen int

const (
	screenDiscover screen = iota
	screenPlugins
	screenFormats
	screenScope
	screenConfirm
	screenProgress
	screenSummary
)

// formatOrder / scopeOrder fix the on-screen ordering of toggles.
var scopeOrder = []model.Scope{model.ScopeSystem, model.ScopeUser}

// progLine is one row in the progress log.
type progLine struct {
	label string
	phase string
	err   bool
}

// Model is the root Bubble Tea model.
type Model struct {
	tr       i18n.Translator
	st       styles
	keys     keyMap
	client   *release.Client
	targetOS model.OS

	width, height int

	screen  screen
	spinner spinner.Model
	prog    progress.Model
	help    help.Model

	// discovery
	cat     release.Catalog
	loadErr error

	// plugin selection
	cursor   int
	selected map[string]bool

	// format selection (only VST3/AU that exist on this OS are offered)
	formatOpts   []model.Format
	formatOn     map[model.Format]bool
	formatCursor int

	// scope
	scope       model.Scope
	scopeCursor int

	// plan + progress
	items     []model.PlanItem
	versionOf map[string]string
	installCh chan tea.Msg
	logLines  []progLine
	done      int
	total     int
	result    model.ApplyResult
	runErr    error

	quitting bool
}

// New builds the initial model.
func New(client *release.Client, targetOS model.OS) Model {
	sp := spinner.New()
	sp.Spinner = spinner.Dot

	m := Model{
		tr:       i18n.New(),
		st:       newStyles(),
		keys:     defaultKeys(),
		client:   client,
		targetOS: targetOS,
		screen:   screenDiscover,
		spinner:  sp,
		prog:     progress.New(progress.WithDefaultGradient()),
		help:     help.New(),
		selected: map[string]bool{},
		scope:    model.ScopeSystem,
		formatOn: map[model.Format]bool{},
	}
	sp.Style = m.st.spinner
	m.spinner = sp
	return m
}

// Init starts discovery and the spinner.
func (m Model) Init() tea.Cmd {
	return tea.Batch(m.spinner.Tick, discoverCmd(m.client))
}

// Update is the central event handler.
func (m Model) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	switch msg := msg.(type) {
	case tea.WindowSizeMsg:
		m.width, m.height = msg.Width, msg.Height
		m.help.Width = msg.Width
		return m, nil

	case tea.KeyMsg:
		return m.handleKey(msg)

	case spinner.TickMsg:
		if m.screen == screenDiscover {
			var cmd tea.Cmd
			m.spinner, cmd = m.spinner.Update(msg)
			return m, cmd
		}
		return m, nil

	case discoveredMsg:
		m.loadErr = msg.err
		if msg.err == nil {
			m.cat = msg.cat
			m.versionOf = map[string]string{}
			for _, p := range m.cat.Plugins {
				m.versionOf[p.Slug] = p.Version
			}
			m.initSelections()
			m.screen = screenPlugins
		}
		return m, nil

	case progressMsg:
		m.applyProgress(app.ProgressEvent(msg))
		return m, waitForMsg(m.installCh)

	case installDoneMsg:
		m.result = msg.result
		m.runErr = msg.err
		m.screen = screenSummary
		return m, nil

	case progress.FrameMsg:
		pm, cmd := m.prog.Update(msg)
		m.prog = pm.(progress.Model)
		return m, cmd
	}
	return m, nil
}

func (m Model) handleKey(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	// Quit is always available except mid-install (avoid leaving a half-run).
	if keyMatches(msg, m.keys.Quit) && m.screen != screenProgress {
		m.quitting = true
		return m, tea.Quit
	}

	switch m.screen {
	case screenDiscover:
		if m.loadErr != nil && keyMatches(msg, m.keys.Retry) {
			m.loadErr = nil
			m.screen = screenDiscover
			return m, tea.Batch(m.spinner.Tick, discoverCmd(m.client))
		}
	case screenPlugins:
		return m.updatePlugins(msg)
	case screenFormats:
		return m.updateFormats(msg)
	case screenScope:
		return m.updateScope(msg)
	case screenConfirm:
		return m.updateConfirm(msg)
	case screenSummary:
		if keyMatches(msg, m.keys.Restart) {
			return m.restart()
		}
	}
	return m, nil
}

// ---- plugin selection ----

func (m *Model) initSelections() {
	m.selected = map[string]bool{}
	for _, p := range m.cat.Plugins {
		if !m.pluginInstallable(p) {
			continue
		}
		// Pre-select plugins that have an update available.
		if p.State == model.StateUpdateAvailable {
			m.selected[p.Slug] = true
		}
	}
	m.cursor = 0
	// Default formats: everything the OS supports.
	m.formatOpts = m.osFormats()
	m.formatOn = map[model.Format]bool{}
	for _, f := range m.formatOpts {
		m.formatOn[f] = true
	}
}

func (m Model) updatePlugins(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	list := m.installablePlugins()
	switch {
	case keyMatches(msg, m.keys.Up):
		if m.cursor > 0 {
			m.cursor--
		}
	case keyMatches(msg, m.keys.Down):
		if m.cursor < len(list)-1 {
			m.cursor++
		}
	case keyMatches(msg, m.keys.Toggle):
		if m.cursor < len(list) {
			slug := list[m.cursor].Slug
			m.selected[slug] = !m.selected[slug]
		}
	case keyMatches(msg, m.keys.All):
		for _, p := range list {
			if p.State == model.StateUpdateAvailable {
				m.selected[p.Slug] = true
			}
		}
	case keyMatches(msg, m.keys.Next):
		if m.anySelected() {
			m.screen = screenFormats
			m.formatCursor = 0
		}
	}
	return m, nil
}

// ---- format selection ----

func (m Model) updateFormats(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	switch {
	case keyMatches(msg, m.keys.Up):
		if m.formatCursor > 0 {
			m.formatCursor--
		}
	case keyMatches(msg, m.keys.Down):
		if m.formatCursor < len(m.formatOpts)-1 {
			m.formatCursor++
		}
	case keyMatches(msg, m.keys.Toggle):
		f := m.formatOpts[m.formatCursor]
		m.formatOn[f] = !m.formatOn[f]
	case keyMatches(msg, m.keys.Back):
		m.screen = screenPlugins
	case keyMatches(msg, m.keys.Next):
		if m.anyFormatOn() {
			m.screen = screenScope
			m.scopeCursor = 0
			for i, s := range scopeOrder {
				if s == m.scope {
					m.scopeCursor = i
				}
			}
		}
	}
	return m, nil
}

// ---- scope selection ----

func (m Model) updateScope(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	switch {
	case keyMatches(msg, m.keys.Up):
		if m.scopeCursor > 0 {
			m.scopeCursor--
		}
	case keyMatches(msg, m.keys.Down):
		if m.scopeCursor < len(scopeOrder)-1 {
			m.scopeCursor++
		}
	case keyMatches(msg, m.keys.Back):
		m.screen = screenFormats
	case keyMatches(msg, m.keys.Next):
		m.scope = scopeOrder[m.scopeCursor]
		if err := m.buildPlan(); err == nil {
			m.screen = screenConfirm
		}
	}
	return m, nil
}

// ---- confirm ----

func (m Model) updateConfirm(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	switch {
	case keyMatches(msg, m.keys.Back):
		m.screen = screenScope
	case keyMatches(msg, m.keys.Next):
		return m.beginInstall()
	}
	return m, nil
}

func (m Model) beginInstall() (tea.Model, tea.Cmd) {
	m.screen = screenProgress
	m.logLines = nil
	m.done = 0
	m.total = len(m.items)
	ch := make(chan tea.Msg, 32)
	m.installCh = ch
	installer := &app.Installer{Client: m.client, Checksums: m.cat.Checksums, OS: m.targetOS}
	return m, startInstall(ch, installer, m.items, m.scope, m.versionOf)
}

func (m *Model) applyProgress(ev app.ProgressEvent) {
	label := ev.Item.Slug + " (" + string(ev.Item.Format) + ")"
	m.logLines = append(m.logLines, progLine{label: label, phase: ev.Phase, err: ev.Err != nil})
	if ev.Phase == app.PhaseDone || ev.Phase == app.PhaseError {
		m.done++
	}
}

func (m Model) restart() (tea.Model, tea.Cmd) {
	m.initSelections()
	m.logLines = nil
	m.result = model.ApplyResult{}
	m.runErr = nil
	m.screen = screenPlugins
	return m, nil
}

// buildPlan resolves the current selection into plan items.
func (m *Model) buildPlan() error {
	sel := app.Selection{OS: m.targetOS, Scope: m.scope}
	for _, p := range m.installablePlugins() {
		if m.selected[p.Slug] {
			sel.Slugs = append(sel.Slugs, p.Slug)
		}
	}
	for _, f := range m.formatOpts {
		if m.formatOn[f] {
			sel.Formats = append(sel.Formats, f)
		}
	}
	items, err := app.BuildPlanItems(m.cat, sel)
	if err != nil {
		return err
	}
	m.items = items
	return nil
}
