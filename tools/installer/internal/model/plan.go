package model

// Move is a single staged-bundle -> final-destination copy. Src is a bundle
// directory (".vst3" / ".component") sitting under the installer's temp root;
// Dst is its final location under one of the known install roots.
type Move struct {
	Src string `json:"src"`
	Dst string `json:"dst"`

	// Receipt is the bookkeeping identity this move belongs to. It is carried
	// in the plan so the applier can record what it ACTUALLY installed: for a
	// system-scope run the receipt lives in a root-owned directory the
	// unprivileged parent cannot write, and the content is only knowable after
	// the moves have been attempted. Nil for moves that are not plugin bundles
	// (the installer's own self-install copy).
	Receipt *ReceiptRef `json:"receipt,omitempty"`
}

// ReceiptRef is the receipt row a Move contributes to. It is plain data: the
// applier never derives a path from it, only the entry's contents.
type ReceiptRef struct {
	Slug    string  `json:"slug"`
	Variant Variant `json:"variant"`
	Version string  `json:"version"`
	Format  Format  `json:"format"`
	Scope   Scope   `json:"scope"`
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

	// ReceiptPath, when set, is the receipt file the applier merges this plan's
	// successful moves into. It is only set for SYSTEM-scope plans, whose
	// receipt (/Library/Application Support/… or %ProgramData%\…) is root-owned:
	// writing it inside the one privileged apply keeps the "at most one
	// elevation prompt" guarantee that a second, post-apply elevation would
	// break. User-scope receipts are written by the unprivileged parent and this
	// stays empty.
	//
	// The applier re-validates this path exactly like a destination — it must
	// sit under an allowlisted install root and be named receipt.json — so a
	// tampered plan cannot turn it into an arbitrary privileged write.
	ReceiptPath string `json:"receiptPath,omitempty"`
}

// PlanItem couples a plugin/format/scope selection to the concrete download and
// destination it resolves to. The TUI builds a []PlanItem for the confirm
// screen; the progress step turns successfully-staged items into Moves.
type PlanItem struct {
	Slug        string
	Variant     Variant
	Name        string
	Format      Format
	Scope       Scope
	Version     string // selected version to install (receipt + download)
	Channel     string // stable | beta | dev
	Action      string // "install" or "update"
	Asset       Asset
	Destination string // final bundle dir
}
