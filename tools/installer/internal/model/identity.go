package model

import (
	"fmt"
	"strings"
)

// EntryKey builds the stable compound install identity (slug|variant|scope).
// Empty variant defaults to stable. Empty scope is allowed for "not yet scoped"
// catalog rows (scope chosen later on the scope screen).
func EntryKey(slug string, variant Variant, scope Scope) string {
	if variant == "" {
		variant = VariantStable
	}
	return slug + "|" + string(variant) + "|" + string(scope)
}

// ParseEntryKey splits a compound install identity.
func ParseEntryKey(key string) (slug string, variant Variant, scope Scope, err error) {
	parts := strings.Split(key, "|")
	if len(parts) != 3 {
		return "", "", "", fmt.Errorf("invalid entry key %q", key)
	}
	return parts[0], Variant(parts[1]), Scope(parts[2]), nil
}

// VariantForChannel maps a release channel to the coinstallable variant.
// beta shares the stable identity; only dev gets a distinct variant.
func VariantForChannel(channel string) Variant {
	if channel == "dev" {
		return VariantDev
	}
	return VariantStable
}
