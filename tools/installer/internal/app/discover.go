// Package app wires the release-discovery and install packages into the
// orchestration shared by the TUI and the headless (--no-tui) path.
package app

import (
	"context"

	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/release"
)

// Discover fetches the latest release and everything derived from it, then
// reconciles against locally-installed versions (installed may be nil).
//
// manifest, catalog and checksums are fetched concurrently; catalog is
// best-effort (a missing catalog.json degrades to title-cased slugs).
func Discover(ctx context.Context, c *release.Client, installed map[string]string) (release.Catalog, error) {
	rel, err := c.FetchLatestRelease(ctx)
	if err != nil {
		return release.Catalog{}, err
	}

	type manifestRes struct {
		m   map[string]string
		err error
	}
	type sumsRes struct {
		s   release.Checksums
		err error
	}
	type catRes struct {
		c   map[string]release.CatalogEntry
		err error
	}
	mCh := make(chan manifestRes, 1)
	sCh := make(chan sumsRes, 1)
	cCh := make(chan catRes, 1)

	go func() { m, err := c.FetchManifest(ctx, rel); mCh <- manifestRes{m, err} }()
	go func() { s, err := c.FetchChecksums(ctx, rel); sCh <- sumsRes{s, err} }()
	go func() { cat, err := c.FetchCatalog(ctx, rel); cCh <- catRes{cat, err} }()

	mr := <-mCh
	if mr.err != nil {
		return release.Catalog{}, mr.err
	}
	sr := <-sCh
	if sr.err != nil {
		return release.Catalog{}, sr.err
	}
	cr := <-cCh
	// catalog is best-effort: ignore cr.err, use whatever (possibly nil) map.

	assets := release.ParsePluginAssets(rel)
	return release.Reconcile(rel.Tag, mr.m, assets, cr.c, installed, sr.s), nil
}
