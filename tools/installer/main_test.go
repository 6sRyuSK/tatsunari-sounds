package main

import (
	"testing"

	"github.com/6sRyuSK/tatsunari-sounds/tools/installer/internal/model"
	"github.com/6sRyuSK/tatsunari-sounds/tools/installer/internal/release"
)

func TestParseFlagsDefaults(t *testing.T) {
	opts, err := parseFlags(nil)
	if err != nil {
		t.Fatal(err)
	}
	if opts.scope != model.ScopeSystem {
		t.Errorf("default scope = %q, want system", opts.scope)
	}
	if opts.noTUI || opts.dryRun || opts.jsonOut || opts.showVersion || opts.showHelp {
		t.Errorf("default bool flags should be false: %+v", opts)
	}
}

func TestParseFlagsCombination(t *testing.T) {
	opts, err := parseFlags([]string{
		"--no-tui", "--dry-run", "--json",
		"--plugins", "a,b", "--formats", "vst3,au",
		"--scope", "user", "--os", "macOS",
	})
	if err != nil {
		t.Fatal(err)
	}
	if !opts.noTUI || !opts.dryRun || !opts.jsonOut {
		t.Errorf("bool flags not set: %+v", opts)
	}
	if opts.plugins != "a,b" || opts.formats != "vst3,au" {
		t.Errorf("plugins/formats = %q / %q", opts.plugins, opts.formats)
	}
	if opts.scope != model.ScopeUser {
		t.Errorf("scope = %q, want user", opts.scope)
	}
	if opts.targetOS != model.OSMacOS {
		t.Errorf("os = %q, want macOS", opts.targetOS)
	}
}

func TestParseFlagsEqualsForm(t *testing.T) {
	opts, err := parseFlags([]string{"--plugins=all", "--formats=vst3", "--scope=system", "--os=Windows"})
	if err != nil {
		t.Fatal(err)
	}
	if opts.plugins != "all" || opts.formats != "vst3" {
		t.Errorf("plugins/formats = %q / %q", opts.plugins, opts.formats)
	}
	if opts.scope != model.ScopeSystem || opts.targetOS != model.OSWindows {
		t.Errorf("scope/os = %q / %q", opts.scope, opts.targetOS)
	}
}

func TestParseFlagsShortFlags(t *testing.T) {
	for _, a := range []string{"-v", "--version"} {
		opts, err := parseFlags([]string{a})
		if err != nil || !opts.showVersion {
			t.Errorf("%s: showVersion=%v err=%v", a, opts.showVersion, err)
		}
	}
	for _, a := range []string{"-h", "--help"} {
		opts, err := parseFlags([]string{a})
		if err != nil || !opts.showHelp {
			t.Errorf("%s: showHelp=%v err=%v", a, opts.showHelp, err)
		}
	}
}

func TestParseFlagsErrors(t *testing.T) {
	cases := [][]string{
		{"--unknown"},
		{"--plugins"},        // missing value
		{"--formats"},        // missing value
		{"--scope"},          // missing value
		{"--os"},             // missing value
		{"--scope", "bogus"}, // bad scope
		{"--os", "linux"},    // bad os
		{"--scope=weird"},    // bad scope (equals form)
		{"--os=solaris"},     // bad os (equals form)
	}
	for _, args := range cases {
		if _, err := parseFlags(args); err == nil {
			t.Errorf("parseFlags(%v) expected error", args)
		}
	}
}

func TestParseScope(t *testing.T) {
	for in, want := range map[string]model.Scope{"system": model.ScopeSystem, "SYSTEM": model.ScopeSystem, "user": model.ScopeUser, "User": model.ScopeUser} {
		got, err := parseScope(in)
		if err != nil || got != want {
			t.Errorf("parseScope(%q) = %q, %v; want %q", in, got, err, want)
		}
	}
	if _, err := parseScope("root"); err == nil {
		t.Error("parseScope(root) should error")
	}
}

func TestParseOS(t *testing.T) {
	for in, want := range map[string]model.OS{"macos": model.OSMacOS, "mac": model.OSMacOS, "darwin": model.OSMacOS, "windows": model.OSWindows, "win": model.OSWindows} {
		got, err := parseOS(in)
		if err != nil || got != want {
			t.Errorf("parseOS(%q) = %q, %v; want %q", in, got, err, want)
		}
	}
	if _, err := parseOS("bsd"); err == nil {
		t.Error("parseOS(bsd) should error")
	}
}

func TestParseFormats(t *testing.T) {
	got, err := parseFormats("vst3,au", model.OSMacOS)
	if err != nil || len(got) != 2 || got[0] != model.FormatVST3 || got[1] != model.FormatAU {
		t.Fatalf("parseFormats macOS = %v, %v", got, err)
	}
	if _, err := parseFormats("au", model.OSWindows); err == nil {
		t.Error("AU on Windows should error")
	}
	if _, err := parseFormats("aax", model.OSMacOS); err == nil {
		t.Error("unknown format should error")
	}
	if got, err := parseFormats("  ", model.OSMacOS); err != nil || got != nil {
		t.Errorf("blank formats = %v, %v; want nil,nil", got, err)
	}
}

func TestSplitCSV(t *testing.T) {
	got := splitCSV(" a , b ,, c ")
	want := []string{"a", "b", "c"}
	if len(got) != len(want) {
		t.Fatalf("splitCSV = %v, want %v", got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Errorf("splitCSV[%d] = %q, want %q", i, got[i], want[i])
		}
	}
	if splitCSV("") != nil {
		t.Error("splitCSV(\"\") should be nil")
	}
}

func catalogFixture() release.Catalog {
	mk := func(slug string, keys ...model.AssetKey) model.Plugin {
		avail := map[model.AssetKey]model.Asset{}
		for _, k := range keys {
			avail[k] = model.Asset{Name: slug}
		}
		return model.Plugin{Slug: slug, Version: "1.0.0", Available: avail}
	}
	return release.Catalog{
		Tag: "2026.1",
		Plugins: []model.Plugin{
			mk("mac-only", model.AssetKey{OS: model.OSMacOS, Format: model.FormatVST3}),
			mk("win-only", model.AssetKey{OS: model.OSWindows, Format: model.FormatVST3}),
			mk("both", model.AssetKey{OS: model.OSMacOS, Format: model.FormatVST3}, model.AssetKey{OS: model.OSWindows, Format: model.FormatVST3}),
		},
	}
}

func TestResolveSlugs(t *testing.T) {
	cat := catalogFixture()

	// empty spec -> nil
	if resolveSlugs("", cat, model.OSMacOS) != nil {
		t.Error("empty spec should resolve to nil")
	}

	// "all" filters to plugins with a macOS asset
	got := resolveSlugs("all", cat, model.OSMacOS)
	want := map[string]bool{"mac-only": true, "both": true}
	if len(got) != len(want) {
		t.Fatalf("all(macOS) = %v, want %v", got, want)
	}
	for _, s := range got {
		if !want[s] {
			t.Errorf("unexpected slug %q for macOS", s)
		}
	}

	// "all" on Windows filters differently
	gotWin := resolveSlugs("all", cat, model.OSWindows)
	wantWin := map[string]bool{"win-only": true, "both": true}
	if len(gotWin) != len(wantWin) {
		t.Fatalf("all(Windows) = %v, want %v", gotWin, wantWin)
	}

	// explicit CSV passes through verbatim (no filtering)
	explicit := resolveSlugs("x, y", cat, model.OSMacOS)
	if len(explicit) != 2 || explicit[0] != "x" || explicit[1] != "y" {
		t.Errorf("explicit CSV = %v", explicit)
	}
}

func TestRunVersionHelpAndError(t *testing.T) {
	if code := run([]string{"--version"}); code != 0 {
		t.Errorf("run(--version) = %d, want 0", code)
	}
	if code := run([]string{"--help"}); code != 0 {
		t.Errorf("run(--help) = %d, want 0", code)
	}
	if code := run([]string{"--bogus"}); code != 2 {
		t.Errorf("run(--bogus) = %d, want 2", code)
	}
}
