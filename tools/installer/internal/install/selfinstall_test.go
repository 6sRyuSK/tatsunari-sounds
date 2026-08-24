package install

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/6sRyuSK/tatsunari-sounds/tools/installer/internal/model"
)

func TestFindInstallerBinaryOrder(t *testing.T) {
	home := t.TempDir()
	t.Setenv("HOME", home)
	t.Setenv("APPDATA", filepath.Join(home, "AppData", "Roaming"))
	t.Setenv("LOCALAPPDATA", filepath.Join(home, "AppData", "Local"))

	userPath, err := InstallerBinaryPath(model.OSMacOS, model.ScopeUser)
	if err != nil {
		t.Fatal(err)
	}
	if _, _, ok := FindInstallerBinary(model.OSMacOS); ok {
		t.Fatal("expected miss when nothing installed")
	}

	if err := os.MkdirAll(filepath.Dir(userPath), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(userPath, []byte("user"), 0o755); err != nil {
		t.Fatal(err)
	}
	got, scope, ok := FindInstallerBinary(model.OSMacOS)
	if !ok || scope != model.ScopeUser || got != userPath {
		t.Fatalf("user: got=%q scope=%s ok=%v", got, scope, ok)
	}

	// System path is absolute (/Library/...) and not writable in CI; verify the
	// resolved path shape and that user still wins when both exist by faking
	// the search via a Windows layout under LOCALAPPDATA / ProgramFiles.
	t.Setenv("ProgramFiles", filepath.Join(home, "Program Files"))
	winUser, err := InstallerBinaryPath(model.OSWindows, model.ScopeUser)
	if err != nil {
		t.Fatal(err)
	}
	winSys, err := InstallerBinaryPath(model.OSWindows, model.ScopeSystem)
	if err != nil {
		t.Fatal(err)
	}
	for _, p := range []string{winSys, winUser} {
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(p, []byte("x"), 0o755); err != nil {
			t.Fatal(err)
		}
	}
	got, scope, ok = FindInstallerBinary(model.OSWindows)
	if !ok || scope != model.ScopeUser || got != winUser {
		t.Fatalf("windows user should win: got=%q scope=%s ok=%v want=%q", got, scope, ok, winUser)
	}
}

func TestSelfInstallMoveInAllowlist(t *testing.T) {
	t.Setenv("HOME", "/Users/tester")
	staging := t.TempDir()
	src := filepath.Join(staging, "tatsunari-sounds-installer")
	if err := os.WriteFile(src, []byte("bin"), 0o755); err != nil {
		t.Fatal(err)
	}
	mv, err := SelfInstallMove(model.OSMacOS, model.ScopeUser, src)
	if err != nil {
		t.Fatal(err)
	}
	plan := model.InstallPlan{Moves: []model.Move{mv}, ResultPath: filepath.Join(staging, "result.json")}
	roots := InstallRoots(model.OSMacOS)
	if err := ValidatePlan(plan, roots, staging); err != nil {
		t.Fatalf("self-install move must be allowlisted: %v", err)
	}
}

// §11.5 pins the shipping filename. The plugins' in-editor update badge locates
// the installer by this exact name (update/src/InstallerLocator.cpp), the
// bootstrap scripts write it, and the release assets are named after it — they
// all have to move together, so make the name itself a gated constant.
func TestInstallerBinaryName(t *testing.T) {
	if got := InstallerBinaryName(model.OSWindows); got != "tatsunari-sounds-installer.exe" {
		t.Errorf("windows installer name = %q", got)
	}
	if got := InstallerBinaryName(model.OSMacOS); got != "tatsunari-sounds-installer" {
		t.Errorf("macOS installer name = %q", got)
	}
}
