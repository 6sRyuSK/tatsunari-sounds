//go:build darwin

package elevate

import (
	"os/exec"
	"strings"
)

// runElevated asks osascript to run the helper "with administrator privileges",
// which shows the native macOS auth dialog (Touch ID / password) exactly once.
// The inner shell command is single-quoted per argument; the whole command is
// then embedded as an AppleScript string literal.
func runElevated(self, planPath, resultPath string) error {
	inner := shQuote(self) + " __apply --plan " + shQuote(planPath) + " --result " + shQuote(resultPath)
	script := "do shell script " + asQuote(inner) + " with administrator privileges"
	return exec.Command("/usr/bin/osascript", "-e", script).Run()
}

// shQuote wraps s in POSIX single quotes, escaping embedded single quotes.
func shQuote(s string) string {
	return "'" + strings.ReplaceAll(s, "'", `'\''`) + "'"
}

// asQuote renders s as an AppleScript double-quoted string literal.
func asQuote(s string) string {
	s = strings.ReplaceAll(s, `\`, `\\`)
	s = strings.ReplaceAll(s, `"`, `\"`)
	return `"` + s + `"`
}
