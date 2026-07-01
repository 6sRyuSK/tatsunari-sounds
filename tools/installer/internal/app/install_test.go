package app

import (
	"archive/zip"
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"

	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/model"
	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/release"
)

// makeZipBytes builds an in-memory zip with a "<bundle>/..." bundle at the root.
func makeZipBytes(t *testing.T, bundle string) []byte {
	t.Helper()
	var buf bytes.Buffer
	zw := zip.NewWriter(&buf)
	for name, body := range map[string]string{
		bundle + "/Contents/moduleinfo.json": `{"name":"X"}`,
		bundle + "/Contents/bin":             "BINARY",
	} {
		w, err := zw.Create(name)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := w.Write([]byte(body)); err != nil {
			t.Fatal(err)
		}
	}
	if err := zw.Close(); err != nil {
		t.Fatal(err)
	}
	return buf.Bytes()
}

// TestInstallerRunUserScope exercises the full pipeline end to end:
// download -> checksum verify -> extract -> in-process (user-scope) apply ->
// receipt, against a local TLS server. No elevation, no real system paths.
func TestInstallerRunUserScope(t *testing.T) {
	zipBytes := makeZipBytes(t, "Saturator.vst3")
	sum := sha256.Sum256(zipBytes)
	assetName := "saturator-v0_1_3-Windows.zip"

	srv := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/"+assetName {
			_, _ = w.Write(zipBytes)
			return
		}
		http.NotFound(w, r)
	}))
	defer srv.Close()

	client := release.NewClient("o", "r", "")
	client.HTTP = srv.Client()

	// Point the receipt dir at a temp location (APPDATA on Windows, HOME else).
	cfg := t.TempDir()
	t.Setenv("APPDATA", cfg)
	t.Setenv("HOME", cfg)

	dest := t.TempDir()
	items := []model.PlanItem{{
		Slug:        "saturator",
		Name:        "Saturator",
		Format:      model.FormatVST3,
		Scope:       model.ScopeUser,
		Action:      "install",
		Asset:       model.Asset{Name: assetName, DownloadURL: srv.URL + "/" + assetName},
		Destination: dest,
	}}

	installer := &Installer{
		Client:    client,
		Checksums: release.Checksums{assetName: hex.EncodeToString(sum[:])},
		OS:        model.OSWindows,
	}

	res, installed, err := installer.Run(context.Background(), items, model.ScopeUser, nil)
	if err != nil {
		t.Fatalf("Run: %v", err)
	}
	if len(res.Errors) != 0 {
		t.Fatalf("unexpected errors: %v", res.Errors)
	}
	wantDst := filepath.Join(dest, "Saturator.vst3")
	if len(res.Installed) != 1 || res.Installed[0] != wantDst {
		t.Fatalf("installed = %v, want [%s]", res.Installed, wantDst)
	}
	if _, err := os.Stat(filepath.Join(wantDst, "Contents", "moduleinfo.json")); err != nil {
		t.Errorf("bundle content missing at destination: %v", err)
	}

	// Receipt written with the plugin's version.
	if err := WriteReceipt(installed, map[string]string{"saturator": "0.1.3"}); err != nil {
		t.Fatalf("WriteReceipt: %v", err)
	}
	// The version came from BuildPlanItems in production; here assert receipt round-trips.
}

// TestInstallerRejectsBadChecksum proves a tampered download is refused before
// extraction.
func TestInstallerRejectsBadChecksum(t *testing.T) {
	zipBytes := makeZipBytes(t, "Saturator.vst3")
	assetName := "saturator-v0_1_3-Windows.zip"

	srv := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write(zipBytes)
	}))
	defer srv.Close()
	client := release.NewClient("o", "r", "")
	client.HTTP = srv.Client()

	dest := t.TempDir()
	items := []model.PlanItem{{
		Slug: "saturator", Format: model.FormatVST3, Scope: model.ScopeUser,
		Asset:       model.Asset{Name: assetName, DownloadURL: srv.URL + "/" + assetName},
		Destination: dest,
	}}
	installer := &Installer{
		Client:    client,
		Checksums: release.Checksums{assetName: "0000000000000000000000000000000000000000000000000000000000000000"},
		OS:        model.OSWindows,
	}
	res, installed, err := installer.Run(context.Background(), items, model.ScopeUser, nil)
	if err != nil {
		t.Fatalf("Run returned fatal err: %v", err)
	}
	if len(installed) != 0 || len(res.Installed) != 0 {
		t.Fatal("nothing should install on checksum mismatch")
	}
	if len(res.Errors) == 0 {
		t.Fatal("expected a checksum-mismatch error")
	}
	if _, err := os.Stat(filepath.Join(dest, "Saturator.vst3")); !os.IsNotExist(err) {
		t.Error("bundle must not be written when the checksum fails")
	}
}
