package model

// Move is a single staged-bundle -> final-destination copy. Src is a bundle
// directory (".vst3" / ".component") sitting under the installer's temp root;
// Dst is its final location under one of the known install roots.
type Move struct {
	Src string `json:"src"`
	Dst string `json:"dst"`
}

// InstallPlan is the serialized contract between the unprivileged TUI and the
// privileged __apply helper. The same struct also drives the in-process
// (per-user) apply, so there is exactly one apply engine.
//
// Everything the helper does is derived from this file; the helper re-validates
// every path against an allowlist before acting with elevated rights.
type InstallPlan struct {
	Moves []Move `json:"moves"`

	// Quarantine lists destination bundle dirs to strip of
	// com.apple.quarantine after the move (macOS only; no-op elsewhere).
	Quarantine []string `json:"quarantine,omitempty"`

	// RefreshAU asks the applier to re-register AudioUnits after install
	// (macOS: killall -9 AudioComponentRegistrar). Set when any AU is moved.
	RefreshAU bool `json:"refreshAU,omitempty"`

	// ResultPath is where the applier writes its ApplyResult JSON so the parent
	// can read outcomes back across the elevation boundary.
	ResultPath string `json:"resultPath"`
}

// PlanItem couples a plugin/format/scope selection to the concrete download and
// destination it resolves to. The TUI builds a []PlanItem for the confirm
// screen; the progress step turns successfully-staged items into Moves.
type PlanItem struct {
	Slug        string
	Name        string
	Format      Format
	Scope       Scope
	Action      string // "install" or "update"
	Asset       Asset
	Destination string // final bundle dir
}
