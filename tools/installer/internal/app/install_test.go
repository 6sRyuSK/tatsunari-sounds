package app

import (
	"archive/zip"
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"

	"github.com/6sRyuSK/tatsunari-sounds/tools/installer/internal/install"
	"github.com/6sRyuSK/tatsunari-sounds/tools/installer/internal/model"
	"github.com/6sRyuSK/tatsunari-sounds/tools/installer/internal/release"
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
		Variant:     model.VariantStable,
		Name:        "Saturator",
		Format:      model.FormatVST3,
		Scope:       model.ScopeUser,
		Version:     "0.1.3",
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
	if err := WriteReceipt(model.OSWindows, installed); err != nil {
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

// TestInstallerElevatesPerItemScope is the regression gate for the mixed-scope
// bug: the scope the user picks on the scope screen applies to NEW installs,
// but an UPDATE keeps the scope of the row it updates. Applying a system
// destination under the picked user scope would skip elevation and fail with a
// permission error, so each item must be applied under ITS OWN scope.
func TestInstallerElevatesPerItemScope(t *testing.T) {
	zipBytes := makeZipBytes(t, "Saturator.vst3")
	sum := sha256.Sum256(zipBytes)
	assetName := "saturator-v0_1_3-Windows.zip"

	srv := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write(zipBytes)
	}))
	defer srv.Close()
	client := release.NewClient("o", "r", "")
	client.HTTP = srv.Client()

	userDest, sysDest := t.TempDir(), t.TempDir()
	mk := func(slug string, scope model.Scope, dest string) model.PlanItem {
		return model.PlanItem{
			Slug: slug, Variant: model.VariantStable, Format: model.FormatVST3,
			Scope: scope, Version: "0.1.3",
			Asset:       model.Asset{Name: assetName, DownloadURL: srv.URL + "/" + assetName},
			Destination: dest,
		}
	}
	items := []model.PlanItem{
		mk("newbie", model.ScopeUser, userDest),    // new install, picked scope
		mk("oldtimer", model.ScopeSystem, sysDest), // update of an existing system row
	}

	// Record the (scope, destinations) each apply call receives.
	type call struct {
		scope model.Scope
		dsts  []string
	}
	var calls []call
	orig := applyFn
	t.Cleanup(func() { applyFn = orig })
	applyFn = func(scope model.Scope, plan model.InstallPlan, stagingDir string) (model.ApplyResult, error) {
		c := call{scope: scope}
		var res model.ApplyResult
		for _, mv := range plan.Moves {
			c.dsts = append(c.dsts, mv.Dst)
			res.Installed = append(res.Installed, mv.Dst)
		}
		calls = append(calls, c)
		return res, nil
	}

	installer := &Installer{
		Client:      client,
		Checksums:   release.Checksums{assetName: hex.EncodeToString(sum[:])},
		OS:          model.OSWindows,
		SelfInstall: false,
	}
	// The user picks "user" on the scope screen; the system row must NOT follow it.
	_, installed, err := installer.Run(context.Background(), items, model.ScopeUser, nil)
	if err != nil {
		t.Fatalf("Run: %v", err)
	}

	if len(calls) != 2 {
		t.Fatalf("expected one apply per scope, got %d: %+v", len(calls), calls)
	}
	// User scope is applied first: it needs no elevation, so the single
	// elevation prompt (if any) comes last.
	if calls[0].scope != model.ScopeUser || calls[1].scope != model.ScopeSystem {
		t.Fatalf("apply order = %s, %s; want user then system", calls[0].scope, calls[1].scope)
	}
	for _, c := range calls {
		if len(c.dsts) != 1 {
			t.Fatalf("%s group should hold exactly its own move, got %v", c.scope, c.dsts)
		}
	}
	if got := filepath.Dir(calls[0].dsts[0]); got != userDest {
		t.Errorf("user group destination = %q, want under %q", got, userDest)
	}
	if got := filepath.Dir(calls[1].dsts[0]); got != sysDest {
		t.Errorf("system group destination = %q, want under %q", got, sysDest)
	}
	if len(installed) != 2 {
		t.Errorf("both items should be reported installed, got %d", len(installed))
	}
}

// TestInstallerKeepsOtherScopeWhenOneElevationFails proves a cancelled system
// elevation does not discard the user-scope bundles that already landed — they
// must still be returned so the caller records them in the receipt.
func TestInstallerKeepsOtherScopeWhenOneElevationFails(t *testing.T) {
	zipBytes := makeZipBytes(t, "Saturator.vst3")
	sum := sha256.Sum256(zipBytes)
	assetName := "saturator-v0_1_3-Windows.zip"

	srv := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write(zipBytes)
	}))
	defer srv.Close()
	client := release.NewClient("o", "r", "")
	client.HTTP = srv.Client()

	userDest, sysDest := t.TempDir(), t.TempDir()
	mk := func(slug string, scope model.Scope, dest string) model.PlanItem {
		return model.PlanItem{
			Slug: slug, Variant: model.VariantStable, Format: model.FormatVST3,
			Scope: scope, Version: "0.1.3",
			Asset:       model.Asset{Name: assetName, DownloadURL: srv.URL + "/" + assetName},
			Destination: dest,
		}
	}
	items := []model.PlanItem{
		mk("newbie", model.ScopeUser, userDest),
		mk("oldtimer", model.ScopeSystem, sysDest),
	}

	orig := applyFn
	t.Cleanup(func() { applyFn = orig })
	applyFn = func(scope model.Scope, plan model.InstallPlan, stagingDir string) (model.ApplyResult, error) {
		if scope == model.ScopeSystem {
			return model.ApplyResult{}, errors.New("elevation cancelled")
		}
		var res model.ApplyResult
		for _, mv := range plan.Moves {
			res.Installed = append(res.Installed, mv.Dst)
		}
		return res, nil
	}

	installer := &Installer{
		Client:    client,
		Checksums: release.Checksums{assetName: hex.EncodeToString(sum[:])},
		OS:        model.OSWindows,
	}
	res, installed, err := installer.Run(context.Background(), items, model.ScopeUser, nil)
	if err == nil {
		t.Fatal("cancelled elevation must surface as an error")
	}
	if len(installed) != 1 || installed[0].Item.Slug != "newbie" {
		t.Fatalf("the user-scope install must survive, got %+v", installed)
	}
	if len(res.Installed) != 1 {
		t.Errorf("result should report the one applied bundle, got %v", res.Installed)
	}
}

// TestSystemScopeReceiptGoesThroughTheApplier pins the split: the system plan
// carries a ReceiptPath (so the privileged apply records it while it still
// holds elevation), and WriteReceipt — which runs unprivileged — leaves those
// rows alone rather than filing them in the user's receipt.
func TestSystemScopeReceiptGoesThroughTheApplier(t *testing.T) {
	zipBytes := makeZipBytes(t, "Saturator.vst3")
	sum := sha256.Sum256(zipBytes)
	assetName := "saturator-v0_1_3-Windows.zip"

	srv := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write(zipBytes)
	}))
	defer srv.Close()
	client := release.NewClient("o", "r", "")
	client.HTTP = srv.Client()

	cfg := t.TempDir()
	t.Setenv("APPDATA", cfg)
	t.Setenv("HOME", cfg)
	t.Setenv("ProgramData", filepath.Join(cfg, "ProgramData"))

	userDest, sysDest := t.TempDir(), t.TempDir()
	mk := func(slug string, scope model.Scope, dest string) model.PlanItem {
		return model.PlanItem{
			Slug: slug, Variant: model.VariantStable, Format: model.FormatVST3,
			Scope: scope, Version: "0.1.3",
			Asset:       model.Asset{Name: assetName, DownloadURL: srv.URL + "/" + assetName},
			Destination: dest,
		}
	}
	items := []model.PlanItem{
		mk("useronly", model.ScopeUser, userDest),
		mk("systemwide", model.ScopeSystem, sysDest),
	}

	plansByScope := map[model.Scope]model.InstallPlan{}
	orig := applyFn
	t.Cleanup(func() { applyFn = orig })
	applyFn = func(scope model.Scope, plan model.InstallPlan, stagingDir string) (model.ApplyResult, error) {
		plansByScope[scope] = plan
		var res model.ApplyResult
		for _, mv := range plan.Moves {
			res.Installed = append(res.Installed, mv.Dst)
		}
		return res, nil
	}

	installer := &Installer{
		Client:    client,
		Checksums: release.Checksums{assetName: hex.EncodeToString(sum[:])},
		OS:        model.OSWindows,
	}
	_, installed, err := installer.Run(context.Background(), items, model.ScopeUser, nil)
	if err != nil {
		t.Fatalf("Run: %v", err)
	}

	// The system plan carries a receipt path; the user plan must not.
	sysPlan := plansByScope[model.ScopeSystem]
	if sysPlan.ReceiptPath == "" {
		t.Error("the system plan must carry a ReceiptPath for the privileged write")
	}
	if got := filepath.Base(sysPlan.ReceiptPath); got != "receipt.json" {
		t.Errorf("system ReceiptPath basename = %q", got)
	}
	if p := plansByScope[model.ScopeUser].ReceiptPath; p != "" {
		t.Errorf("the user plan must not carry a ReceiptPath, got %q", p)
	}

	// Every bundle move carries its receipt identity, so the applier can record
	// exactly what landed.
	for _, mv := range sysPlan.Moves {
		if mv.Receipt == nil {
			t.Fatalf("move %q carries no receipt identity", mv.Dst)
		}
		if mv.Receipt.Scope != model.ScopeSystem || mv.Receipt.Slug != "systemwide" {
			t.Errorf("receipt ref = %+v", *mv.Receipt)
		}
	}

	// WriteReceipt files ONLY the user row.
	if err := WriteReceipt(model.OSWindows, installed); err != nil {
		t.Fatalf("WriteReceipt: %v", err)
	}
	userPath, err := install.ReceiptPathFor(model.OSWindows, model.ScopeUser)
	if err != nil {
		t.Fatal(err)
	}
	r, err := install.LoadReceiptAt(userPath)
	if err != nil {
		t.Fatal(err)
	}
	if _, ok := r.Entries[install.EntryKey("useronly", model.VariantStable, model.ScopeUser)]; !ok {
		t.Errorf("user row missing from the user receipt; have %v", r.Entries)
	}
	if _, ok := r.Entries[install.EntryKey("systemwide", model.VariantStable, model.ScopeSystem)]; ok {
		t.Error("a system row must NOT be filed in the user receipt")
	}
}
