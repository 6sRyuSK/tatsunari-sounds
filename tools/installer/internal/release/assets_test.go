package release

import (
	"testing"

	"github.com/6sRyuSK/tatsunari-sounds/tools/installer/internal/model"
)

func mkRelease(names ...string) *Release {
	r := &Release{Tag: "2026.2"}
	for _, n := range names {
		r.Assets = append(r.Assets, model.Asset{Name: n, DownloadURL: "https://example/" + n})
	}
	return r
}

func TestParsePluginAssets(t *testing.T) {
	rel := mkRelease(
		"tn-resonance-suppressor-v0_2_1-macOS-AU.zip",
		"tn-resonance-suppressor-v0_2_1-macOS-VST3.zip",
		"tn-resonance-suppressor-v0_2_1-Windows.zip",
		"nam-player-v0_1_0-macOS-VST3.zip",
		"nam-player-v0_1_0-Windows.zip",
		// noise that must be ignored:
		"tatsunari-sounds-v2026_2-macOS-AU.zip",
		"tatsunari-sounds-v2026_2-Windows.zip",
		"manifest.json",
		"catalog.json",
		"SHA256SUMS.txt",
	)
	got := ParsePluginAssets(rel)

	if len(got) != 2 {
		t.Fatalf("expected 2 plugins, got %d: %v", len(got), keys(got))
	}
	rs := got["tn-resonance-suppressor"]
	if rs == nil {
		t.Fatal("tn-resonance-suppressor missing")
	}
	if rs.Version != "0.2.1" {
		t.Errorf("version = %q, want 0.2.1", rs.Version)
	}
	for _, k := range []model.AssetKey{
		{OS: model.OSMacOS, Format: model.FormatAU},
		{OS: model.OSMacOS, Format: model.FormatVST3},
		{OS: model.OSWindows, Format: model.FormatVST3},
	} {
		if _, ok := rs.Assets[k]; !ok {
			t.Errorf("tn-resonance-suppressor missing asset %+v", k)
		}
	}
	// nam-player: no AU
	np := got["nam-player"]
	if _, ok := np.Assets[model.AssetKey{OS: model.OSMacOS, Format: model.FormatAU}]; ok {
		t.Error("nam-player should not have a macOS-AU asset")
	}
	if _, ok := np.Assets[model.AssetKey{OS: model.OSWindows, Format: model.FormatVST3}]; !ok {
		t.Error("nam-player should have a Windows VST3 asset")
	}
}

func TestTitleCaseSlug(t *testing.T) {
	cases := map[string]string{
		"tn-resonance-suppressor": "TN Resonance Suppressor",
		"tn-equalizer":            "TN Equalizer",
		"tn-vocal-tuner":          "TN Vocal Tuner",
		"nam-player":              "Nam Player",
		"single-band-eq":          "Single Band Eq",
		"saturator":               "Saturator",
	}
	for in, want := range cases {
		if got := TitleCaseSlug(in); got != want {
			t.Errorf("TitleCaseSlug(%q) = %q, want %q", in, got, want)
		}
	}
}

func keys(m map[string]*PluginAssets) []string {
	var out []string
	for k := range m {
		out = append(out, k)
	}
	return out
}
