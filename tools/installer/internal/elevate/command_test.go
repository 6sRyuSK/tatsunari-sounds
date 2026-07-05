package elevate

import (
	"strings"
	"testing"
)

func TestShQuote(t *testing.T) {
	cases := []struct {
		name string
		in   string
		want string
	}{
		{"plain", "hello", `'hello'`},
		{"empty", "", `''`},
		{"space", "/tmp/my app/self", `'/tmp/my app/self'`},
		{"single_quote", "it's", `'it'\''s'`},
		{"only_single_quote", "'", `''\'''`},
		{"double_quote", `a"b`, `'a"b'`},
		{"backslash", `a\b`, `'a\b'`},
		{"unicode", "/Users/太郎/plug", `'/Users/太郎/plug'`},
		{"dollar_and_backtick", "$(x)`y`", "'$(x)`y`'"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if got := shQuote(tc.in); got != tc.want {
				t.Errorf("shQuote(%q) = %q, want %q", tc.in, got, tc.want)
			}
		})
	}
}

func TestAsQuote(t *testing.T) {
	cases := []struct {
		name string
		in   string
		want string
	}{
		{"plain", "hello", `"hello"`},
		{"empty", "", `""`},
		{"double_quote", `a"b`, `"a\"b"`},
		{"backslash", `a\b`, `"a\\b"`},
		// Backslash must be escaped before the quote so a trailing
		// backslash-quote can never terminate the literal early.
		{"backslash_then_quote", `a\"b`, `"a\\\"b"`},
		{"trailing_backslash", `a\`, `"a\\"`},
		{"unicode", "太郎", `"太郎"`},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if got := asQuote(tc.in); got != tc.want {
				t.Errorf("asQuote(%q) = %q, want %q", tc.in, got, tc.want)
			}
		})
	}
}

// asQuote must escape backslashes before quotes; the reverse order would let
// an input like `\"` collapse into `\\"` (an escaped backslash + an unescaped
// quote) and prematurely close the AppleScript literal — a classic injection.
func TestAsQuoteOrderingIsSafe(t *testing.T) {
	got := asQuote(`\"`)
	if got != `"\\\""` {
		t.Fatalf("asQuote(%q) = %q, want %q", `\"`, got, `"\\\"`+`"`)
	}
	// The interior (between the outer quotes) must contain an even, balanced
	// set of escapes: \\ then \".
	inner := strings.TrimSuffix(strings.TrimPrefix(got, `"`), `"`)
	if inner != `\\\"` {
		t.Fatalf("inner = %q, want %q", inner, `\\\"`)
	}
}

func TestPsQuote(t *testing.T) {
	cases := []struct {
		name string
		in   string
		want string
	}{
		{"plain", "hello", `'hello'`},
		{"empty", "", `''`},
		{"single_quote", "it's", `'it''s'`},
		{"only_single_quote", "'", `''''`},
		{"double_quote", `a"b`, `'a"b'`},
		{"backslash_preserved", `C:\Program Files\x`, `'C:\Program Files\x'`},
		{"unicode", `C:\太郎`, `'C:\太郎'`},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if got := psQuote(tc.in); got != tc.want {
				t.Errorf("psQuote(%q) = %q, want %q", tc.in, got, tc.want)
			}
		})
	}
}

func TestBuildDarwinScript(t *testing.T) {
	got := buildDarwinScript("/opt/tatsunari", "/stage/plan.json", "/stage/result.json")
	want := `do shell script "'/opt/tatsunari' __apply --plan '/stage/plan.json' --result '/stage/result.json'" with administrator privileges`
	if got != want {
		t.Fatalf("buildDarwinScript mismatch:\n got: %s\nwant: %s", got, want)
	}
}

func TestBuildDarwinScriptQuotesPathsWithSpaces(t *testing.T) {
	got := buildDarwinScript("/Applications/My App/tatsunari", "/var folders/plan.json", "/var folders/result.json")
	want := `do shell script "'/Applications/My App/tatsunari' __apply --plan '/var folders/plan.json' --result '/var folders/result.json'" with administrator privileges`
	if got != want {
		t.Fatalf("buildDarwinScript with spaces mismatch:\n got: %s\nwant: %s", got, want)
	}
}

func TestBuildDarwinScriptEscapesSingleQuoteInPath(t *testing.T) {
	// A path with a single quote is first sh-escaped to '\'' by shQuote, then
	// the backslash is AppleScript-escaped to \\ by asQuote, yielding '\\''.
	// This double-escaping is exactly the original behavior and must be exact.
	got := buildDarwinScript("/Users/o'brien/tatsunari", "/stage/plan.json", "/stage/result.json")
	want := `do shell script "'/Users/o'\\''brien/tatsunari' __apply --plan '/stage/plan.json' --result '/stage/result.json'" with administrator privileges`
	if got != want {
		t.Fatalf("buildDarwinScript with single quote mismatch:\n got: %s\nwant: %s", got, want)
	}
}

func TestBuildDarwinScriptUnicodePath(t *testing.T) {
	got := buildDarwinScript("/Users/太郎/tatsunari", "/stage/計画.json", "/stage/結果.json")
	want := `do shell script "'/Users/太郎/tatsunari' __apply --plan '/stage/計画.json' --result '/stage/結果.json'" with administrator privileges`
	if got != want {
		t.Fatalf("buildDarwinScript unicode mismatch:\n got: %s\nwant: %s", got, want)
	}
}

func TestBuildWindowsCommand(t *testing.T) {
	got := buildWindowsCommand(`C:\Program Files\tatsunari.exe`, `C:\stage\plan.json`, `C:\stage\result.json`)
	want := `$ErrorActionPreference='Stop';` +
		`$p = Start-Process -FilePath 'C:\Program Files\tatsunari.exe'` +
		` -ArgumentList '__apply','--plan','C:\stage\plan.json','--result','C:\stage\result.json'` +
		` -Verb RunAs -WindowStyle Hidden -Wait -PassThru;` +
		`exit $p.ExitCode`
	if got != want {
		t.Fatalf("buildWindowsCommand mismatch:\n got: %s\nwant: %s", got, want)
	}
}

func TestBuildWindowsCommandEscapesSingleQuote(t *testing.T) {
	// An apostrophe in the path must be doubled inside the PS single-quoted string.
	got := buildWindowsCommand(`C:\Users\o'brien\tatsunari.exe`, `C:\stage\plan.json`, `C:\stage\result.json`)
	want := `$ErrorActionPreference='Stop';` +
		`$p = Start-Process -FilePath 'C:\Users\o''brien\tatsunari.exe'` +
		` -ArgumentList '__apply','--plan','C:\stage\plan.json','--result','C:\stage\result.json'` +
		` -Verb RunAs -WindowStyle Hidden -Wait -PassThru;` +
		`exit $p.ExitCode`
	if got != want {
		t.Fatalf("buildWindowsCommand with quote mismatch:\n got: %s\nwant: %s", got, want)
	}
}

func TestBuildWindowsCommandUnicodeAndEmpty(t *testing.T) {
	got := buildWindowsCommand(`C:\太郎\tatsunari.exe`, ``, ``)
	want := `$ErrorActionPreference='Stop';` +
		`$p = Start-Process -FilePath 'C:\太郎\tatsunari.exe'` +
		` -ArgumentList '__apply','--plan','','--result',''` +
		` -Verb RunAs -WindowStyle Hidden -Wait -PassThru;` +
		`exit $p.ExitCode`
	if got != want {
		t.Fatalf("buildWindowsCommand unicode/empty mismatch:\n got: %s\nwant: %s", got, want)
	}
}
