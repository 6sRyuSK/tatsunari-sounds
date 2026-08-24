package main

import (
	"bytes"
	"encoding/json"
	"os"
	"testing"
)

// The shipping Windows binary is named tatsunari-sounds-installer.exe (plan
// §11.5). Windows' UAC Installer Detection heuristic forces an elevation prompt
// on UNMANIFESTED executables whose filename contains install/setup/update/
// patch, so the build must embed an application manifest requesting asInvoker.
// The manifest lives in the committed rsrc_windows_amd64.syso, generated from
// winres/winres.json (see winres/README.md).
//
// These tests gate both halves so a regression cannot reach a release: the
// source of truth still says "as invoker", and the committed resource object
// actually carries that manifest.

func TestWindowsManifestSourceRequestsAsInvoker(t *testing.T) {
	raw, err := os.ReadFile("winres/winres.json")
	if err != nil {
		t.Fatalf("winres/winres.json must exist: %v", err)
	}
	var doc struct {
		Manifest map[string]map[string]struct {
			ExecutionLevel string `json:"execution-level"`
			AutoElevate    bool   `json:"auto-elevate"`
			UIAccess       bool   `json:"ui-access"`
		} `json:"RT_MANIFEST"`
	}
	if err := json.Unmarshal(raw, &doc); err != nil {
		t.Fatalf("winres.json is not valid JSON: %v", err)
	}
	if len(doc.Manifest) == 0 {
		t.Fatal("winres.json declares no RT_MANIFEST resource")
	}
	for id, langs := range doc.Manifest {
		for lang, m := range langs {
			if m.ExecutionLevel != "as invoker" {
				t.Errorf("RT_MANIFEST[%s][%s] execution-level = %q, want %q",
					id, lang, m.ExecutionLevel, "as invoker")
			}
			if m.AutoElevate {
				t.Errorf("RT_MANIFEST[%s][%s] must not auto-elevate", id, lang)
			}
			if m.UIAccess {
				t.Errorf("RT_MANIFEST[%s][%s] must not request uiAccess", id, lang)
			}
		}
	}
}

func TestWindowsManifestResourceIsCommitted(t *testing.T) {
	// The Go linker only picks the resource up for GOOS=windows GOARCH=amd64
	// builds when this exact filename sits in the module root.
	syso, err := os.ReadFile("rsrc_windows_amd64.syso")
	if err != nil {
		t.Fatalf("rsrc_windows_amd64.syso must be committed (see winres/README.md): %v", err)
	}
	// The manifest is stored as UTF-8 XML inside the resource section.
	if !bytes.Contains(syso, []byte(`<requestedExecutionLevel level="asInvoker"`)) {
		t.Error("the committed .syso does not carry a requestedExecutionLevel=asInvoker manifest; " +
			"regenerate it from winres/winres.json (go run github.com/tc-hib/go-winres@v0.3.3 make --arch amd64)")
	}
	if bytes.Contains(syso, []byte(`level="requireAdministrator"`)) {
		t.Error("the committed .syso requests administrator; the installer must start unelevated")
	}
}
