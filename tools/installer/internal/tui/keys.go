package tui

import "github.com/charmbracelet/bubbles/key"

// keyMap collects every binding, wired into bubbles/help for the footer.
type keyMap struct {
	Up      key.Binding
	Down    key.Binding
	Toggle  key.Binding
	All     key.Binding
	Next    key.Binding
	Back    key.Binding
	Retry   key.Binding
	Restart key.Binding
	Help    key.Binding
	Quit    key.Binding
}

func defaultKeys() keyMap {
	return keyMap{
		Up:      key.NewBinding(key.WithKeys("up", "k"), key.WithHelp("↑/k", "up")),
		Down:    key.NewBinding(key.WithKeys("down", "j"), key.WithHelp("↓/j", "down")),
		Toggle:  key.NewBinding(key.WithKeys(" "), key.WithHelp("space", "toggle")),
		All:     key.NewBinding(key.WithKeys("a"), key.WithHelp("a", "select updatable")),
		Next:    key.NewBinding(key.WithKeys("enter"), key.WithHelp("enter", "next")),
		Back:    key.NewBinding(key.WithKeys("esc", "b"), key.WithHelp("esc", "back")),
		Retry:   key.NewBinding(key.WithKeys("r"), key.WithHelp("r", "retry")),
		Restart: key.NewBinding(key.WithKeys("n"), key.WithHelp("n", "new install")),
		Help:    key.NewBinding(key.WithKeys("?"), key.WithHelp("?", "help")),
		Quit:    key.NewBinding(key.WithKeys("q", "ctrl+c"), key.WithHelp("q", "quit")),
	}
}

// ShortHelp / FullHelp implement help.KeyMap.
func (k keyMap) ShortHelp() []key.Binding {
	return []key.Binding{k.Up, k.Down, k.Toggle, k.Next, k.Back, k.Quit}
}

func (k keyMap) FullHelp() [][]key.Binding {
	return [][]key.Binding{
		{k.Up, k.Down, k.Toggle, k.All},
		{k.Next, k.Back, k.Retry, k.Restart},
		{k.Help, k.Quit},
	}
}
