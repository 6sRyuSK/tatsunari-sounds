package release

import (
	"sort"

	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/model"
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
// the locally-installed versions (slug->version, "" if unknown) into the
// display model. installed may be nil.
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
		p := model.Plugin{
			Slug:      slug,
			Name:      TitleCaseSlug(slug),
			Version:   version,
			Available: map[model.AssetKey]model.Asset{},
		}
		if ce, ok := catalog[slug]; ok {
			if ce.Name != "" {
				p.Name = ce.Name
			}
			p.Category = ce.Category
			p.Reference = ce.Reference
		}
		if pa, ok := assets[slug]; ok {
			p.Available = pa.Assets
		}
		if installed != nil {
			p.Installed = installed[slug]
		}
		p.State = model.StateFor(p.Installed, p.Version)
		plugins = append(plugins, p)
	}
	sort.Slice(plugins, func(i, j int) bool { return plugins[i].Slug < plugins[j].Slug })
	return Catalog{Tag: tag, Plugins: plugins, Checksums: sums}
}
