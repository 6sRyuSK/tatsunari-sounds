package elevate

import "strings"

// This file holds the platform-independent, pure command-construction logic for
// system-scope elevation. The platform files (elevate_darwin.go /
// elevate_windows.go) are thin syscall wrappers that build their invocation
// with these functions and hand it to exec. Keeping the string assembly and
// argument quoting here lets it be unit-tested on any platform (including Linux
// CI, where the platform files cannot be compiled).

// applyArgs are the fixed helper arguments, threaded through the OS-specific
// quoting below. The privileged helper is always invoked as
// `<self> __apply --plan <planPath> --result <resultPath>`.

// buildDarwinScript assembles the AppleScript passed to `osascript -e`. The
// inner shell command is single-quoted per argument (POSIX sh), then embedded
// as an AppleScript double-quoted string literal, and run "with administrator
// privileges" so macOS shows the native auth dialog exactly once.
func buildDarwinScript(self, planPath, resultPath string) string {
	inner := shQuote(self) + " __apply --plan " + shQuote(planPath) + " --result " + shQuote(resultPath)
	return "do shell script " + asQuote(inner) + " with administrator privileges"
}

// buildWindowsCommand assembles the PowerShell command passed to
// `powershell.exe -Command`. It re-launches the helper with `-Verb RunAs`
// (triggering a single UAC prompt), hidden and waited-on, and propagates the
// child's exit code so a cancelled UAC surfaces as an error.
func buildWindowsCommand(self, planPath, resultPath string) string {
	argList := strings.Join([]string{
		psQuote("__apply"),
		psQuote("--plan"), psQuote(planPath),
		psQuote("--result"), psQuote(resultPath),
	}, ",")

	return "$ErrorActionPreference='Stop';" +
		"$p = Start-Process -FilePath " + psQuote(self) +
		" -ArgumentList " + argList +
		" -Verb RunAs -WindowStyle Hidden -Wait -PassThru;" +
		"exit $p.ExitCode"
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

// psQuote wraps s in a PowerShell single-quoted string, doubling embedded quotes.
func psQuote(s string) string {
	return "'" + strings.ReplaceAll(s, "'", "''") + "'"
}
