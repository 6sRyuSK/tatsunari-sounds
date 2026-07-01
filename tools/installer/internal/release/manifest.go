package release

import (
	"context"
	"encoding/json"
	"fmt"

	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/model"
)

// ManifestName is the release asset mapping slug -> dotted version.
const ManifestName = "manifest.json"

// FetchManifest downloads and parses manifest.json ({ "<slug>": "<version>" }).
func (c *Client) FetchManifest(ctx context.Context, rel *Release) (map[string]string, error) {
	a, ok := rel.Asset(ManifestName)
	if !ok {
		return nil, fmt.Errorf("release %s has no %s", rel.Tag, ManifestName)
	}
	body, err := c.GetBody(ctx, a.DownloadURL)
	if err != nil {
		return nil, err
	}
	defer body.Close()

	var m map[string]string
	if err := json.NewDecoder(body).Decode(&m); err != nil {
		return nil, fmt.Errorf("decode %s: %w", ManifestName, err)
	}
	return m, nil
}

// versionToToken converts a dotted version ("0.2.1") to the underscore token
// used in asset filenames ("v0_2_1"). Kept here because manifest versions are
// the canonical dotted form used to build asset names.
func versionToToken(v string) (string, error) {
	sv, err := model.ParseSemVer(v)
	if err != nil {
		return "", err
	}
	return fmt.Sprintf("v%d_%d_%d", sv.Major, sv.Minor, sv.Patch), nil
}
