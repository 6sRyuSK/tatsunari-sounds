package updates

// Payload size ceilings (plan §3). Kept as named constants so fixture tests can
// assert the boundary without hard-coding magic numbers elsewhere.
const (
	MaxLatestBytes    = 256 * 1024
	MaxCatalogBytes   = 8 * 1024 * 1024
	MaxChangelogBytes = 64 * 1024
	SHA256HexLen      = 64
)

// SchemaVersion is the frozen /tatsunarisounds/updates/v1/ envelope version.
const SchemaVersion = 1
