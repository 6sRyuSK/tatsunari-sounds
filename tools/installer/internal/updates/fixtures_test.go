package updates_test

import (
	"os"
	"path/filepath"
	"runtime"
	"testing"

	"github.com/6sRyuSK/tatsunari-sounds/tools/installer/internal/updates"
)

func fixtureDir(t *testing.T) string {
	t.Helper()
	_, file, _, ok := runtime.Caller(0)
	if !ok {
		t.Fatal("runtime.Caller failed")
	}
	return filepath.Join(filepath.Dir(file), "..", "..", "testdata", "updates", "v1")
}

func readFixture(t *testing.T, name string) []byte {
	t.Helper()
	data, err := os.ReadFile(filepath.Join(fixtureDir(t), name))
	if err != nil {
		t.Fatal(err)
	}
	return data
}

func withHosts(t *testing.T) {
	t.Helper()
	prev := updates.AllowedHostsSnapshot()
	updates.SetAllowedHosts([]string{"cdn.example.test", "updates.example.test"})
	t.Cleanup(func() { updates.SetAllowedHosts(prev) })
}

func TestParseLatestMinimalAndFull(t *testing.T) {
	withHosts(t)
	min, err := updates.ParseLatest(readFixture(t, "latest_minimal.json"))
	if err != nil {
		t.Fatal(err)
	}
	if len(min.Doc.Plugins) != 1 || min.Doc.Plugins[0].Latest != "1.0.0" {
		t.Fatalf("minimal: %+v", min.Doc.Plugins)
	}
	full, err := updates.ParseLatest(readFixture(t, "latest_full.json"))
	if err != nil {
		t.Fatal(err)
	}
	if len(full.Doc.Plugins) != 1 {
		t.Fatalf("full plugins=%d", len(full.Doc.Plugins))
	}
	if len(full.Doc.Plugins[0].Highlights) != 3 {
		t.Errorf("highlights=%v", full.Doc.Plugins[0].Highlights)
	}
}

func TestParseCatalogMinimalFull(t *testing.T) {
	withHosts(t)
	min, err := updates.ParseCatalog(readFixture(t, "catalog_minimal.json"))
	if err != nil {
		t.Fatal(err)
	}
	if len(min.Doc.Plugins) != 1 || len(min.Issues) != 0 {
		t.Fatalf("minimal plugins=%d issues=%v", len(min.Doc.Plugins), min.Issues)
	}
	full, err := updates.ParseCatalog(readFixture(t, "catalog_full.json"))
	if err != nil {
		t.Fatal(err)
	}
	if full.Doc.Client == nil || len(full.Doc.Client.Assets) != 3 {
		t.Fatalf("client assets: %+v", full.Doc.Client)
	}
	if len(full.Doc.Plugins) != 2 {
		t.Fatalf("want stable+dev, got %d", len(full.Doc.Plugins))
	}
	var dev *updates.Plugin
	for i := range full.Doc.Plugins {
		if full.Doc.Plugins[i].Variant == "dev" {
			dev = &full.Doc.Plugins[i]
		}
	}
	if dev == nil || len(dev.Versions) != 5 {
		t.Fatalf("dev versions: %+v", dev)
	}
}

func TestUnknownFormatDegradesAsset(t *testing.T) {
	withHosts(t)
	res, err := updates.ParseCatalog(readFixture(t, "catalog_unknown_format.json"))
	if err != nil {
		t.Fatal(err)
	}
	if len(res.Doc.Plugins) != 1 {
		t.Fatalf("plugin should remain usable, got %d", len(res.Doc.Plugins))
	}
	assets := res.Doc.Plugins[0].Versions[0].Assets
	if len(assets) != 1 || assets[0].Format != "vst3" {
		t.Fatalf("expected only vst3 kept, got %+v", assets)
	}
	if len(res.Issues) == 0 {
		t.Fatal("expected issue for unknown format")
	}
}

func TestYankedVersionsParse(t *testing.T) {
	withHosts(t)
	res, err := updates.ParseCatalog(readFixture(t, "catalog_yanked.json"))
	if err != nil {
		t.Fatal(err)
	}
	p := res.Doc.Plugins[0]
	latest, ok := updates.LatestUsable(p, updates.ResolveOptions{Channel: updates.ChannelStable})
	if !ok || latest.Version.Version != "1.0.0" {
		t.Fatalf("yanked should be skipped, got %+v ok=%v", latest, ok)
	}
}

func TestTraversalAndAbsoluteRejected(t *testing.T) {
	withHosts(t)
	for _, name := range []string{"catalog_traversal.json", "catalog_absolute_path.json"} {
		res, err := updates.ParseCatalog(readFixture(t, name))
		if err != nil {
			t.Fatalf("%s envelope err: %v", name, err)
		}
		if len(res.Doc.Plugins) != 0 {
			t.Fatalf("%s: expected plugin dropped when only asset is evil, got %+v", name, res.Doc.Plugins)
		}
		if len(res.Issues) == 0 {
			t.Fatalf("%s: expected issues", name)
		}
	}
}

func TestForbiddenHookAndReferenceRejected(t *testing.T) {
	withHosts(t)
	for _, name := range []string{"catalog_forbidden_hook.json", "catalog_with_reference.json"} {
		res, err := updates.ParseCatalog(readFixture(t, name))
		if err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		if len(res.Doc.Plugins) != 0 {
			t.Fatalf("%s: plugin with forbidden field must be dropped", name)
		}
	}
}

func TestBadSHADegrades(t *testing.T) {
	withHosts(t)
	res, err := updates.ParseCatalog(readFixture(t, "catalog_bad_sha.json"))
	if err != nil {
		t.Fatal(err)
	}
	if len(res.Doc.Plugins) != 0 {
		t.Fatal("plugin with only bad-sha asset should be dropped")
	}
}

func TestFutureClientRowDisabledNotBlocking(t *testing.T) {
	withHosts(t)
	res, err := updates.ParseCatalog(readFixture(t, "catalog_future_client.json"))
	if err != nil {
		t.Fatal(err)
	}
	if len(res.Doc.Plugins) != 2 {
		t.Fatalf("both plugins should parse, got %d", len(res.Doc.Plugins))
	}
	var blocked, okCount int
	for _, p := range res.Doc.Plugins {
		list := updates.ListVersions(p, updates.ResolveOptions{
			Channel: updates.ChannelStable, ClientVersion: "0.2.0",
		})
		for _, vc := range list {
			if vc.Disabled {
				blocked++
			} else {
				okCount++
			}
		}
	}
	if blocked == 0 || okCount == 0 {
		t.Fatalf("expected mixed disabled/ok rows, blocked=%d ok=%d", blocked, okCount)
	}
}

func TestValidateSubpathAdversarial(t *testing.T) {
	cases := []string{
		"../x", "/etc", `C:\Windows`, `\\server\share`, "VST3/../../etc", "CON", "aux.txt",
	}
	for _, c := range cases {
		if err := updates.ValidateSubpath(c); err == nil {
			t.Errorf("ValidateSubpath(%q) should fail", c)
		}
	}
	if err := updates.ValidateSubpath("VST3"); err != nil {
		t.Fatal(err)
	}
	if err := updates.ValidateSubpath("VST3/tatsunari-sounds"); err != nil {
		t.Fatal(err)
	}
}

func TestRejectDuplicateJSONKeys(t *testing.T) {
	withHosts(t)
	dup := []byte(`{"schema":1,"schema":1,"generated":"2026-07-30T00:00:00Z","plugins":[]}`)
	if _, err := updates.ParseLatest(dup); err == nil {
		t.Fatal("duplicate top-level keys must be rejected")
	}
	nested := []byte(`{
  "schema": 1,
  "generated": "2026-07-30T00:00:00Z",
  "channels": [{"id":"stable","name":{"en":"Stable"}}],
  "plugins": [{
    "slug": "x",
    "slug": "y",
    "variant": "stable",
    "name": {"en": "X"},
    "category": "EQ",
    "vendor": "T",
    "pluginIds": {"clapId": "jp.tatsunari-sounds.x"},
    "latest": "1.0.0",
    "versions": [{
      "version": "1.0.0",
      "channel": "stable",
      "releasedAt": "2026-07-01T00:00:00Z",
      "stateCompatVersion": "1.0.0",
      "assets": [{
        "format": "vst3", "os": "macos", "arch": "universal",
        "url": "https://cdn.example.test/a.zip", "size": 1,
        "sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "subpath": "VST3", "bundleName": "X.vst3"
      }]
    }]
  }]
}`)
	if _, err := updates.ParseCatalog(nested); err == nil {
		t.Fatal("duplicate nested keys must be rejected")
	}
}

func TestHostAllowlistFailClosed(t *testing.T) {
	updates.SetAllowedHosts(nil)
	t.Cleanup(func() { updates.SetAllowedHosts(nil) })
	if err := updates.CheckHTTPSURL("https://cdn.example.test/x"); err == nil {
		t.Fatal("empty allowlist must reject")
	}
}

func TestDowngradeAndStateCompat(t *testing.T) {
	down, err := updates.IsDowngrade("1.2.0", "1.1.0")
	if err != nil || !down {
		t.Fatalf("downgrade: %v %v", down, err)
	}
	dec, err := updates.StateCompatDecreases("2.0.0", "1.0.0")
	if err != nil || !dec {
		t.Fatalf("stateCompat: %v %v", dec, err)
	}
}
