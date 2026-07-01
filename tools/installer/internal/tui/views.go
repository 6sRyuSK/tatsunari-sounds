package tui

import (
	"fmt"
	"strings"

	"github.com/charmbracelet/lipgloss"

	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/model"
)

// View renders the current screen.
func (m Model) View() string {
	if m.quitting {
		return ""
	}
	var body string
	switch m.screen {
	case screenDiscover:
		body = m.viewDiscover()
	case screenPlugins:
		body = m.viewPlugins()
	case screenFormats:
		body = m.viewFormats()
	case screenScope:
		body = m.viewScope()
	case screenConfirm:
		body = m.viewConfirm()
	case screenProgress:
		body = m.viewProgress()
	case screenSummary:
		body = m.viewSummary()
	}
	return m.st.App.Render(m.header() + "\n\n" + body + "\n\n" + m.footer())
}

func (m Model) header() string {
	title := m.st.Title.Render("tatsunari-plugins " + m.tr.T("インストーラー", "installer"))
	sub := ""
	if m.cat.Tag != "" {
		sub = m.st.Subtitle.Render(m.tr.T("リリース ", "release ") + m.cat.Tag + "  ·  " + string(m.targetOS))
	}
	return lipgloss.JoinVertical(lipgloss.Left, title, sub)
}

func (m Model) footer() string {
	return m.st.Help.Render(m.help.View(m.keys))
}

func (m Model) viewDiscover() string {
	if m.loadErr != nil {
		return m.st.Panel.BorderForeground(lipgloss.Color("9")).Render(
			m.st.Err.Render(m.tr.T("取得に失敗しました:", "discovery failed:")) + "\n" +
				m.loadErr.Error() + "\n\n" +
				m.st.Dim.Render(m.tr.T("r で再試行 / q で終了", "press r to retry / q to quit")))
	}
	return m.spinner.View() + " " + m.tr.T("最新リリースを取得中…", "fetching the latest release…")
}

func (m Model) viewPlugins() string {
	list := m.installablePlugins()
	intro := m.st.Subtitle.Render(m.tr.T(
		"インストールするプラグインを選択（space で選択、a で更新可を全選択）",
		"choose plugins to install (space to toggle, a to select updatable)"))

	var rows []string
	for i, p := range list {
		cursor := "  "
		if i == m.cursor {
			cursor = m.st.Cursor.Render("▸ ")
		}
		check := m.st.Dim.Render("☐")
		if m.selected[p.Slug] {
			check = m.st.Check.Render("☑")
		}
		name := p.Name
		if i == m.cursor {
			name = m.st.ItemSel.Render(name)
		}
		row := fmt.Sprintf("%s%s %-26s %-10s %s %s",
			cursor, check, name, m.st.Dim.Render(p.Category), m.st.Dim.Render(p.Version), m.stateBadge(p))
		rows = append(rows, row)
	}
	return intro + "\n\n" + strings.Join(rows, "\n")
}

func (m Model) stateBadge(p model.Plugin) string {
	switch p.State {
	case model.StateUpdateAvailable:
		return m.st.badgeUpd.Render(fmt.Sprintf("↑ %s → %s", p.Installed, p.Version))
	case model.StateUpToDate:
		return m.st.badgeCur.Render(m.tr.T("最新", "up to date"))
	case model.StateInstalledUnknown:
		return m.st.badgeUnk.Render(m.tr.T("導入済(版不明)", "installed"))
	default:
		return m.st.badgeNew.Render("NEW")
	}
}

func (m Model) viewFormats() string {
	intro := m.st.Subtitle.Render(m.tr.T(
		"インストールする形式を選択",
		"choose the formats to install"))
	var rows []string
	for i, f := range m.formatOpts {
		cursor := "  "
		if i == m.formatCursor {
			cursor = m.st.Cursor.Render("▸ ")
		}
		check := m.st.Dim.Render("☐")
		if m.formatOn[f] {
			check = m.st.Check.Render("☑")
		}
		desc := ""
		if f == model.FormatAU {
			desc = m.st.Dim.Render(m.tr.T("(macOS のみ)", "(macOS only)"))
		}
		rows = append(rows, fmt.Sprintf("%s%s %-6s %s", cursor, check, string(f), desc))
	}
	return intro + "\n\n" + strings.Join(rows, "\n")
}

func (m Model) viewScope() string {
	intro := m.st.Subtitle.Render(m.tr.T(
		"インストール範囲を選択",
		"choose the install scope"))
	labels := map[model.Scope][2]string{
		model.ScopeSystem: {
			m.tr.T("全ユーザー（システム）", "all users (system)"),
			m.tr.T("OS がパスワード / UAC を一度要求します", "the OS asks for your password / UAC once"),
		},
		model.ScopeUser: {
			m.tr.T("現在のユーザーのみ", "just me"),
			m.tr.T("パスワード不要", "no password required"),
		},
	}
	var rows []string
	for i, s := range scopeOrder {
		cursor := "  "
		radio := m.st.Dim.Render("○")
		if i == m.scopeCursor {
			cursor = m.st.Cursor.Render("▸ ")
			radio = m.st.Accent.Render("◉")
		}
		l := labels[s]
		rows = append(rows, fmt.Sprintf("%s%s %-24s %s", cursor, radio, l[0], m.st.Dim.Render(l[1])))
	}
	note := m.st.Dim.Render(m.tr.T(
		"※ 認証を求めるのは OS であり、このアプリはパスワードを扱いません。",
		"note: the OS collects the password — this app never handles it."))
	return intro + "\n\n" + strings.Join(rows, "\n") + "\n\n" + note
}

func (m Model) viewConfirm() string {
	intro := m.st.Subtitle.Render(m.tr.T("以下の内容でインストールします", "review the install plan"))
	var rows []string
	rows = append(rows, m.st.Dim.Render(fmt.Sprintf("%-26s %-6s %-8s %s",
		m.tr.T("プラグイン", "plugin"), m.tr.T("形式", "format"), m.tr.T("動作", "action"), m.tr.T("宛先", "destination"))))
	for _, it := range m.items {
		action := m.tr.T("新規", "install")
		if it.Action == "update" {
			action = m.tr.T("更新", "update")
		}
		rows = append(rows, fmt.Sprintf("%-26s %-6s %-8s %s",
			it.Name, string(it.Format), action, m.st.Dim.Render(it.Destination)))
	}
	scopeLine := m.st.Accent.Render(m.tr.T("範囲: ", "scope: ") + m.scopeLabel())
	cta := m.st.Subtitle.Render(m.tr.T("enter で実行 / esc で戻る", "enter to install / esc to go back"))
	return intro + "\n\n" + scopeLine + "\n\n" + strings.Join(rows, "\n") + "\n\n" + cta
}

func (m Model) scopeLabel() string {
	if m.scope == model.ScopeSystem {
		return m.tr.T("システム（要認証）", "system (auth required)")
	}
	return m.tr.T("ユーザー（認証不要）", "user (no auth)")
}

func (m Model) viewProgress() string {
	head := m.tr.T("インストール中…", "installing…")
	if m.scope == model.ScopeSystem {
		head += "  " + m.st.Dim.Render(m.tr.T("(OS の認証ダイアログに応答してください)", "(respond to the OS auth dialog)"))
	}
	pct := 0.0
	if m.total > 0 {
		pct = float64(m.done) / float64(m.total)
	}
	bar := m.prog.ViewAs(pct)

	// Show the last several log lines.
	const tail = 10
	start := 0
	if len(m.logLines) > tail {
		start = len(m.logLines) - tail
	}
	var lines []string
	for _, l := range m.logLines[start:] {
		mark := m.st.Ok.Render("•")
		if l.err {
			mark = m.st.Err.Render("✗")
		}
		lines = append(lines, fmt.Sprintf("  %s %-10s %s", mark, l.phase, l.label))
	}
	return head + "\n\n" + bar + "\n\n" + strings.Join(lines, "\n")
}

func (m Model) viewSummary() string {
	var b strings.Builder
	if m.runErr != nil {
		b.WriteString(m.st.Err.Render(m.tr.T("インストールは完了しませんでした:", "install did not complete:")) + "\n")
		b.WriteString("  " + m.runErr.Error() + "\n\n")
	}
	ok := len(m.result.Installed)
	bad := len(m.result.Errors)
	b.WriteString(m.st.Ok.Render(fmt.Sprintf(m.tr.T("%d 個のバンドルをインストールしました", "installed %d bundle(s)"), ok)))
	if bad > 0 {
		b.WriteString("  " + m.st.Err.Render(fmt.Sprintf(m.tr.T("· %d 件の失敗", "· %d error(s)"), bad)))
	}
	b.WriteString("\n")
	for _, e := range m.result.Errors {
		b.WriteString("  " + m.st.Err.Render("✗ ") + e + "\n")
	}
	next := m.tr.T(
		"DAW を再起動してプラグインを再スキャンしてください。",
		"restart your DAW and rescan plugins.")
	b.WriteString("\n" + m.st.Subtitle.Render(next))
	b.WriteString("\n" + m.st.Dim.Render(m.tr.T("n で別のインストール / q で終了", "n for a new install / q to quit")))
	return b.String()
}
