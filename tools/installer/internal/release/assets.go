package release

import (
	"regexp"
	"strings"

	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/model"
)

// pluginAssetRe parses a per-plugin zip name:
//
//	{slug}-v{maj}_{min}_{pat}-{macOS-AU|macOS-VST3|Windows}.zip
//
// The slug group is greedy but the trailing "-v\d+_\d+_\d+-<suffix>.zip" anchor
// makes the split unambiguous even for hyphenated slugs like
// "resonance-suppressor". The overall bundle assets
// ("tatsunari-plugins-v..._...-*.zip") are excluded by name below.
var pluginAssetRe = regexp.MustCompile(`^(?P<slug>.+)-v(\d+)_(\d+)_(\d+)-(macOS-AU|macOS-VST3|Windows)\.zip$`)

// bundlePrefix marks the "everything" bundle assets, which are not per-plugin.
const bundlePrefix = "tatsunari-plugins-"

// PluginAssets is the per-plugin slice of a release: its version plus the
// (os,format) -> asset map derived from the asset filenames.
type PluginAssets struct {
	Version string // dotted, e.g. "0.2.1"
	Assets  map[model.AssetKey]model.Asset
}

// suffixToKey maps the asset-name suffix to an (os, format) key.
var suffixToKey = map[string]model.AssetKey{
	"macOS-AU":   {OS: model.OSMacOS, Format: model.FormatAU},
	"macOS-VST3": {OS: model.OSMacOS, Format: model.FormatVST3},
	"Windows":    {OS: model.OSWindows, Format: model.FormatVST3},
}

// ParsePluginAssets builds the per-slug asset matrix from a release's asset
// list, ignoring manifest.json, catalog.json, checksums and the bundle zips.
func ParsePluginAssets(rel *Release) map[string]*PluginAssets {
	out := map[string]*PluginAssets{}
	for _, a := range rel.Assets {
		if strings.HasPrefix(a.Name, bundlePrefix) {
			continue // overall bundle, not a single plugin
		}
		m := pluginAssetRe.FindStringSubmatch(a.Name)
		if m == nil {
			continue
		}
		slug := m[1]
		version := m[2] + "." + m[3] + "." + m[4]
		key, ok := suffixToKey[m[5]]
		if !ok {
			continue
		}
		pa := out[slug]
		if pa == nil {
			pa = &PluginAssets{Version: version, Assets: map[model.AssetKey]model.Asset{}}
			out[slug] = pa
		}
		pa.Assets[key] = a
	}
	return out
}

// TitleCaseSlug turns "resonance-suppressor" into "Resonance Suppressor" for a
// display name when catalog.json is unavailable.
func TitleCaseSlug(slug string) string {
	words := strings.FieldsFunc(slug, func(r rune) bool { return r == '-' || r == '_' })
	for i, w := range words {
		if w == "" {
			continue
		}
		words[i] = strings.ToUpper(w[:1]) + w[1:]
	}
	return strings.Join(words, " ")
}
