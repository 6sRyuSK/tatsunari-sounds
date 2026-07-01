package tui

import "github.com/charmbracelet/lipgloss"

// styles is the 2026-flavoured theme: rounded borders, a restrained accent, and
// small status badges. Colours are adaptive so it reads on light and dark
// terminals.
type styles struct {
	App      lipgloss.Style
	Title    lipgloss.Style
	Subtitle lipgloss.Style
	Panel    lipgloss.Style
	Item     lipgloss.Style
	ItemSel  lipgloss.Style
	Cursor   lipgloss.Style
	Check    lipgloss.Style
	Dim      lipgloss.Style
	Accent   lipgloss.Style
	Err      lipgloss.Style
	Ok       lipgloss.Style
	Help     lipgloss.Style
	badgeNew lipgloss.Style
	badgeUpd lipgloss.Style
	badgeCur lipgloss.Style
	badgeUnk lipgloss.Style
	spinner  lipgloss.Style
}

func newStyles() styles {
	accent := lipgloss.AdaptiveColor{Light: "#7C3AED", Dark: "#A78BFA"} // violet
	subtle := lipgloss.AdaptiveColor{Light: "#6B7280", Dark: "#9CA3AF"}
	green := lipgloss.AdaptiveColor{Light: "#059669", Dark: "#34D399"}
	amber := lipgloss.AdaptiveColor{Light: "#B45309", Dark: "#FBBF24"}
	red := lipgloss.AdaptiveColor{Light: "#DC2626", Dark: "#F87171"}

	badge := func(fg lipgloss.TerminalColor) lipgloss.Style {
		return lipgloss.NewStyle().Foreground(fg).Bold(true)
	}

	return styles{
		App:      lipgloss.NewStyle().Padding(1, 2),
		Title:    lipgloss.NewStyle().Bold(true).Foreground(accent),
		Subtitle: lipgloss.NewStyle().Foreground(subtle),
		Panel: lipgloss.NewStyle().
			Border(lipgloss.RoundedBorder()).
			BorderForeground(accent).
			Padding(1, 2),
		Item:     lipgloss.NewStyle(),
		ItemSel:  lipgloss.NewStyle().Bold(true),
		Cursor:   lipgloss.NewStyle().Foreground(accent).Bold(true),
		Check:    lipgloss.NewStyle().Foreground(green).Bold(true),
		Dim:      lipgloss.NewStyle().Foreground(subtle),
		Accent:   lipgloss.NewStyle().Foreground(accent),
		Err:      lipgloss.NewStyle().Foreground(red),
		Ok:       lipgloss.NewStyle().Foreground(green),
		Help:     lipgloss.NewStyle().Foreground(subtle),
		badgeNew: badge(accent),
		badgeUpd: badge(amber),
		badgeCur: badge(green),
		badgeUnk: badge(subtle),
		spinner:  lipgloss.NewStyle().Foreground(accent),
	}
}
