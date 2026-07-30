package release

import (
	"sort"

	"github.com/6sRyuSK/tatsunari-sounds/tools/installer/internal/model"
)

// Catalog is the discovery bundle: everything fetched about the latest release,
// reconciled with local install state into a sorted []model.Plugin.
type Catalog struct {
	Tag       string
	Plugins   []model.Plugin
	Checksums Checksums
}

// Reconcile joins the manifest (authoritative slug->version), the parsed asset
// matrix (downloadable per os/format), optional catalog metadata (names) and
// the locally-installed versions keyed by model.EntryKey (slug|variant|scope)
// into the display model. installed may be nil.
//
// When the same slug is installed under multiple scopes or variants, one Plugin
// row is emitted per compound key so update targets stay unambiguous (plan §5.5).
func Reconcile(
	tag string,
	manifest map[string]string,
	assets map[string]*PluginAssets,
	catalog map[string]CatalogEntry,
	installed map[string]string,
	sums Checksums,
) Catalog {
	plugins := make([]model.Plugin, 0, len(manifest))
	for slug, version := range manifest {
		base := model.Plugin{
			Slug:      slug,
			Variant:   model.VariantStable,
			Name:      TitleCaseSlug(slug),
			Version:   version,
			Available: map[model.AssetKey]model.Asset{},
		}
		if ce, ok := catalog[slug]; ok {
			if ce.Name != "" {
				base.Name = ce.Name
			}
			base.Category = ce.Category
			base.Reference = ce.Reference
		}
		if pa, ok := assets[slug]; ok {
			base.Available = pa.Assets
		}

		matches := installedMatches(installed, slug)
		if len(matches) == 0 {
			p := base
			p.State = model.StateFor("", version)
			plugins = append(plugins, p)
			continue
		}
		for _, m := range matches {
			p := base
			p.Variant = m.Variant
			if p.Variant == "" {
				p.Variant = model.VariantStable
			}
			p.Scope = m.Scope
			p.Installed = m.Version
			p.State = model.StateFor(p.Installed, version)
			if p.Variant == model.VariantDev && p.Name != "" && !hasDevSuffix(p.Name) {
				p.Name = p.Name + " (Dev)"
			}
			plugins = append(plugins, p)
		}
	}
	sort.Slice(plugins, func(i, j int) bool {
		if plugins[i].Slug != plugins[j].Slug {
			return plugins[i].Slug < plugins[j].Slug
		}
		if plugins[i].Variant != plugins[j].Variant {
			return plugins[i].Variant < plugins[j].Variant
		}
		return plugins[i].Scope < plugins[j].Scope
	})
	return Catalog{Tag: tag, Plugins: plugins, Checksums: sums}
}

type installedMatch struct {
	Variant model.Variant
	Scope   model.Scope
	Version string
}

func installedMatches(installed map[string]string, slug string) []installedMatch {
	if installed == nil {
		return nil
	}
	var out []installedMatch
	for key, ver := range installed {
		s, variant, scope, err := model.ParseEntryKey(key)
		if err != nil || s != slug {
			continue
		}
		out = append(out, installedMatch{Variant: variant, Scope: scope, Version: ver})
	}
	sort.Slice(out, func(i, j int) bool {
		if out[i].Variant != out[j].Variant {
			return out[i].Variant < out[j].Variant
		}
		return out[i].Scope < out[j].Scope
	})
	return out
}

func hasDevSuffix(name string) bool {
	return len(name) >= 6 && name[len(name)-6:] == " (Dev)"
}
