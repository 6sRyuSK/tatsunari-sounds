package tui

import (
	"sort"

	"github.com/charmbracelet/bubbles/key"
	tea "github.com/charmbracelet/bubbletea"

	"github.com/6sRyuSK/tatsunari-sounds/tools/installer/internal/model"
)

// featuredOrder pins the flagship plugins to the top of the selection list, in
// this order. Everything else keeps the catalog's slug order below them. This is
// a presentation choice, so it lives in the TUI — the release-layer Reconcile
// stays a deterministic slug sort.
var featuredOrder = []string{"resonance-suppressor", "dynamic-eq", "saturator"}

// featuredRank returns a slug's position in featuredOrder, or len(featuredOrder)
// for anything not featured (sorted after the flagships).
func featuredRank(slug string) int {
	for i, s := range featuredOrder {
		if s == slug {
			return i
		}
	}
	return len(featuredOrder)
}

// keyMatches reports whether a key press matches a binding.
func keyMatches(msg tea.KeyMsg, b key.Binding) bool {
	return key.Matches(msg, b)
}

// osFormats returns the formats available on the target OS (AU only on macOS).
func (m Model) osFormats() []model.Format {
	if m.targetOS == model.OSMacOS {
		return []model.Format{model.FormatVST3, model.FormatAU}
	}
	return []model.Format{model.FormatVST3}
}

// pluginInstallable reports whether a plugin has any asset for this OS.
func (m Model) pluginInstallable(p model.Plugin) bool {
	for _, f := range m.osFormats() {
		if p.HasFormat(m.targetOS, f) {
			return true
		}
	}
	return false
}

// installablePlugins is the ordered list of plugins offered on this OS: the
// featured flagships first (featuredOrder), then the rest in catalog order.
func (m Model) installablePlugins() []model.Plugin {
	var out []model.Plugin
	for _, p := range m.cat.Plugins {
		if m.pluginInstallable(p) {
			out = append(out, p)
		}
	}
	// Stable so non-featured plugins keep the catalog's slug order.
	sort.SliceStable(out, func(i, j int) bool {
		return featuredRank(out[i].Slug) < featuredRank(out[j].Slug)
	})
	return out
}

func (m Model) anySelected() bool {
	for _, v := range m.selected {
		if v {
			return true
		}
	}
	return false
}

func (m Model) anyFormatOn() bool {
	for _, f := range m.formatOpts {
		if m.formatOn[f] {
			return true
		}
	}
	return false
}
