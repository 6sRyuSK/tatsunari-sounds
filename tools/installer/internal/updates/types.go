package updates

// Localized is a locale -> string map. "en" is the required fallback.
type Localized map[string]string

// LatestDocument is the badge-only /tatsunarisounds/updates/v1/latest.json envelope.
type LatestDocument struct {
	Schema    int            `json:"schema"`
	Generated string         `json:"generated"`
	Plugins   []LatestPlugin `json:"plugins"`
	// Unknown top-level fields are ignored by encoding/json.
}

// LatestPlugin is one row in latest.json.
type LatestPlugin struct {
	Slug         string   `json:"slug"`
	Latest       string   `json:"latest"`
	Highlights   []string `json:"highlights,omitempty"`
	ChangelogURL string   `json:"changelogUrl,omitempty"`
}

// CatalogDocument is the installer /tatsunarisounds/updates/v1/catalog.json envelope.
type CatalogDocument struct {
	Schema           int         `json:"schema"`
	Generated        string      `json:"generated"`
	Generator        string      `json:"generator,omitempty"`
	MinClientVersion string      `json:"minClientVersion,omitempty"`
	TTLHint          int         `json:"ttlHint,omitempty"`
	Channels         []Channel   `json:"channels"`
	Client           *ClientInfo `json:"client,omitempty"`
	Plugins          []Plugin    `json:"plugins"`
	Notice           *Notice     `json:"notice,omitempty"`
}

// Channel describes a release channel (stable / beta / dev).
type Channel struct {
	ID            string    `json:"id"`
	Name          Localized `json:"name"`
	Description   Localized `json:"description,omitempty"`
	RequiresOptIn bool      `json:"requiresOptIn,omitempty"`
	Retention     int       `json:"retention,omitempty"`
}

// ClientInfo is the installer's own latest version metadata (never blocks UX).
type ClientInfo struct {
	Latest       string        `json:"latest"`
	Assets       []ClientAsset `json:"assets"`
	ChangelogURL string        `json:"changelogUrl,omitempty"`
	MinSupported string        `json:"minSupported,omitempty"`
}

// ClientAsset is one OS/arch installer binary.
type ClientAsset struct {
	OS     string `json:"os"`   // macos | windows
	Arch   string `json:"arch"` // arm64 | amd64
	URL    string `json:"url"`
	Size   int64  `json:"size"`
	SHA256 string `json:"sha256"`
}

// Notice is an optional, dismissible whole-catalog announcement.
type Notice struct {
	ID      string    `json:"id,omitempty"`
	Message Localized `json:"message"`
}

// PluginIds holds the wire identifiers for collision / dev-coexistence checks.
type PluginIds struct {
	ClapID         string `json:"clapId"`
	VST3UID        string `json:"vst3Uid"`
	AUSubtype      string `json:"auSubtype"`
	AUManufacturer string `json:"auManufacturer"`
}

// Plugin is one (slug, variant) row in catalog.json.
type Plugin struct {
	Slug              string    `json:"slug"`
	Variant           string    `json:"variant"` // stable | dev
	Name              Localized `json:"name"`
	DisplayNameSuffix string    `json:"displayNameSuffix,omitempty"`
	ShortDescription  Localized `json:"shortDescription,omitempty"`
	Description       Localized `json:"description,omitempty"`
	Category          string    `json:"category"`
	Vendor            string    `json:"vendor"`
	IconURL           string    `json:"iconUrl,omitempty"`
	ScreenshotURLs    []string  `json:"screenshotUrls,omitempty"`
	HomepageURL       string    `json:"homepageUrl,omitempty"`
	ManualURL         string    `json:"manualUrl,omitempty"`
	PluginIds         PluginIds `json:"pluginIds"`
	Latest            string    `json:"latest"`
	Versions          []Version `json:"versions"`
	Deprecated        bool      `json:"deprecated,omitempty"`
	ReplacedBy        string    `json:"replacedBy,omitempty"`
	MinClientVersion  string    `json:"minClientVersion,omitempty"`
	Tags              []string  `json:"tags,omitempty"`
}

// Version is one released version of a plugin.
type Version struct {
	Version            string            `json:"version"`
	Channel            string            `json:"channel"`
	ReleasedAt         string            `json:"releasedAt"`
	Changelog          Localized         `json:"changelog,omitempty"`
	ChangelogURL       string            `json:"changelogUrl,omitempty"`
	StateCompatVersion string            `json:"stateCompatVersion"`
	MinOSVersion       map[string]string `json:"minOsVersion,omitempty"`
	Yanked             bool              `json:"yanked,omitempty"`
	YankedReason       string            `json:"yankedReason,omitempty"`
	SupersededBy       string            `json:"supersededBy,omitempty"`
	KnownIssues        Localized         `json:"knownIssues,omitempty"`
	Assets             []Asset           `json:"assets"`
}

// Asset is one downloadable plugin bundle.
type Asset struct {
	Format      string `json:"format"` // vst3 | au | clap (unknown → skip row)
	OS          string `json:"os"`     // macos | windows
	Arch        string `json:"arch"`   // universal | x86_64 | arm64
	URL         string `json:"url"`
	Size        int64  `json:"size"`
	SHA256      string `json:"sha256"`
	Subpath     string `json:"subpath"`
	BundleName  string `json:"bundleName"`
	ArchiveRoot string `json:"archiveRoot,omitempty"`
	Compression string `json:"compression,omitempty"`
	Notarized   *bool  `json:"notarized,omitempty"`
	Signed      *bool  `json:"signed,omitempty"`
}

// ParseIssue records a non-fatal row-level problem (design invariant 2).
type ParseIssue struct {
	Kind    string // "plugin" | "version" | "asset" | "client" | "envelope"
	Slug    string
	Variant string
	Version string
	Detail  string
}

// ParseResult is a successfully decoded document plus any degraded rows.
type ParseResult[T any] struct {
	Doc    T
	Issues []ParseIssue
}
