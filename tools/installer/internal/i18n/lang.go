// Package i18n provides minimal bilingual (Japanese/English) UI strings. Rather
// than a global key table, callers pass both variants at the call site via T(),
// keeping the translation next to its context. Language is chosen from the OS
// locale once at startup.
package i18n

import (
	"os"
	"strings"
)

// Lang is the selected UI language.
type Lang int

const (
	EN Lang = iota
	JA
)

// Translator renders bilingual strings for the chosen language.
type Translator struct{ Lang Lang }

// New detects the language from the environment and returns a Translator.
//
// Precedence: TATSUNARI_LANG (explicit override, set by the bootstrap script
// from the Windows UI culture) > LC_ALL/LC_MESSAGES/LANG (POSIX). Anything
// starting with "ja" selects Japanese; otherwise English.
func New() Translator {
	return Translator{Lang: Detect()}
}

// Detect resolves the UI language from the environment.
func Detect() Lang {
	for _, key := range []string{"TATSUNARI_LANG", "LC_ALL", "LC_MESSAGES", "LANG"} {
		if v := strings.ToLower(strings.TrimSpace(os.Getenv(key))); v != "" {
			if strings.HasPrefix(v, "ja") {
				return JA
			}
			// A non-empty, non-ja locale is a positive English signal.
			return EN
		}
	}
	return EN
}

// T returns the Japanese or English string for the current language.
func (t Translator) T(ja, en string) string {
	if t.Lang == JA {
		return ja
	}
	return en
}
