package tui

import (
	"github.com/charmbracelet/bubbles/key"
	tea "github.com/charmbracelet/bubbletea"

	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/model"
)

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

// installablePlugins is the ordered list of plugins offered on this OS.
func (m Model) installablePlugins() []model.Plugin {
	var out []model.Plugin
	for _, p := range m.cat.Plugins {
		if m.pluginInstallable(p) {
			out = append(out, p)
		}
	}
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
