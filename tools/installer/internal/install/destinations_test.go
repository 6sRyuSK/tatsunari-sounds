package install

import (
	"path/filepath"
	"strings"
	"testing"

	"github.com/6sRyuSK/tatsunari-sounds/tools/installer/internal/model"
)

func TestDestinationMacOS(t *testing.T) {
	t.Setenv("HOME", "/Users/tester")

	cases := []struct {
		format model.Format
		scope  model.Scope
		want   string
	}{
		{model.FormatVST3, model.ScopeSystem, "/Library/Audio/Plug-Ins/VST3"},
		{model.FormatAU, model.ScopeSystem, "/Library/Audio/Plug-Ins/Components"},
		{model.FormatVST3, model.ScopeUser, "/Users/tester/Library/Audio/Plug-Ins/VST3"},
		{model.FormatAU, model.ScopeUser, "/Users/tester/Library/Audio/Plug-Ins/Components"},
	}
	for _, c := range cases {
		got, err := Destination(model.OSMacOS, c.format, c.scope)
		if err != nil {
			t.Fatalf("Destination(mac,%s,%s): %v", c.format, c.scope, err)
		}
		if filepath.ToSlash(got) != c.want {
			t.Errorf("Destination(mac,%s,%s) = %q, want %q", c.format, c.scope, got, c.want)
		}
	}
}

func TestDestinationWindows(t *testing.T) {
	t.Setenv("CommonProgramFiles", `C:\Program Files\Common Files`)
	t.Setenv("LOCALAPPDATA", `C:\Users\tester\AppData\Local`)

	// Normalize both separators: when this test runs on Linux CI, filepath.Join
	// mixes the backslashes from the env vars with '/' joins, so compare on a
	// separator-agnostic form.
	norm := func(p string) string { return strings.ReplaceAll(filepath.ToSlash(p), `\`, "/") }

	sys, err := Destination(model.OSWindows, model.FormatVST3, model.ScopeSystem)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.HasSuffix(norm(sys), "Common Files/VST3/tatsunari-sounds") {
		t.Errorf("windows system dest = %q", sys)
	}
	usr, err := Destination(model.OSWindows, model.FormatVST3, model.ScopeUser)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.HasSuffix(norm(usr), "AppData/Local/Programs/Common/VST3") {
		t.Errorf("windows user dest = %q", usr)
	}
	if _, err := Destination(model.OSWindows, model.FormatAU, model.ScopeSystem); err == nil {
		t.Error("windows AU should be rejected")
	}
}

func TestInstallRoots(t *testing.T) {
	t.Setenv("HOME", "/Users/tester")
	t.Setenv("APPDATA", "/Users/tester/AppData/Roaming")
	t.Setenv("ProgramData", `C:\ProgramData`)
	t.Setenv("ProgramFiles", `C:\Program Files`)
	t.Setenv("CommonProgramFiles", `C:\Program Files\Common Files`)
	t.Setenv("LOCALAPPDATA", `C:\Users\tester\AppData\Local`)

	roots := InstallRoots(model.OSMacOS)
	// plugin×2 + installer×2 + receipt×2
	if len(roots) != 6 {
		t.Fatalf("macOS should have 6 install roots, got %d: %v", len(roots), roots)
	}
	win := InstallRoots(model.OSWindows)
	if len(win) != 6 {
		t.Fatalf("windows should have 6 install roots, got %d: %v", len(win), win)
	}
}

func TestDestinationCLAP(t *testing.T) {
	t.Setenv("HOME", "/Users/tester")
	got, err := Destination(model.OSMacOS, model.FormatCLAP, model.ScopeUser)
	if err != nil {
		t.Fatal(err)
	}
	if filepath.ToSlash(got) != "/Users/tester/Library/Audio/Plug-Ins/CLAP" {
		t.Errorf("clap dest = %q", got)
	}
}

func TestResolvePathRejectsTraversal(t *testing.T) {
	if _, err := ResolvePath("/Library/Audio/Plug-Ins", "../etc", "x.vst3"); err == nil {
		t.Fatal("expected traversal rejection")
	}
}
