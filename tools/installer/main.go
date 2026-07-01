// Command tatsunari-installer is a cross-platform TUI installer for the
// tatsunari-plugins audio plugins. Run with no arguments it launches the TUI;
// --no-tui drives it headlessly (used for CI smoke and scripting); the hidden
// __apply subcommand is the privileged file-mover invoked under OS elevation.
package main

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"runtime"
	"strings"

	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/app"
	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/install"
	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/model"
	"github.com/6sRyuSK/tatsunari-plugins/tools/installer/internal/release"
)

// version is overridden at build time via -ldflags "-X main.version=<tag>".
var version = "dev"

func main() {
	// The privileged helper is a positional subcommand so it never collides
	// with user-facing flags. It is intentionally undocumented.
	if len(os.Args) > 1 && os.Args[1] == "__apply" {
		os.Exit(runApply(os.Args[2:]))
	}
	os.Exit(run(os.Args[1:]))
}

func run(args []string) int {
	opts, err := parseFlags(args)
	if err != nil {
		fmt.Fprintln(os.Stderr, "error:", err)
		return 2
	}
	if opts.showVersion {
		fmt.Println("tatsunari-installer", version)
		return 0
	}
	if opts.showHelp {
		printUsage(os.Stdout)
		return 0
	}

	if opts.noTUI || opts.dryRun {
		return runHeadless(opts)
	}
	return runTUI(opts)
}

type options struct {
	noTUI       bool
	dryRun      bool
	jsonOut     bool
	showVersion bool
	showHelp    bool
	plugins     string
	formats     string
	scope       model.Scope
	targetOS    model.OS
}

func parseFlags(args []string) (options, error) {
	opts := options{scope: model.ScopeSystem, targetOS: detectOS()}
	for i := 0; i < len(args); i++ {
		a := args[i]
		next := func() (string, error) {
			if i+1 >= len(args) {
				return "", fmt.Errorf("%s requires a value", a)
			}
			i++
			return args[i], nil
		}
		switch {
		case a == "--no-tui":
			opts.noTUI = true
		case a == "--dry-run":
			opts.dryRun = true
		case a == "--json":
			opts.jsonOut = true
		case a == "--version" || a == "-v":
			opts.showVersion = true
		case a == "--help" || a == "-h":
			opts.showHelp = true
		case a == "--plugins":
			v, err := next()
			if err != nil {
				return opts, err
			}
			opts.plugins = v
		case strings.HasPrefix(a, "--plugins="):
			opts.plugins = strings.TrimPrefix(a, "--plugins=")
		case a == "--formats":
			v, err := next()
			if err != nil {
				return opts, err
			}
			opts.formats = v
		case strings.HasPrefix(a, "--formats="):
			opts.formats = strings.TrimPrefix(a, "--formats=")
		case a == "--scope":
			v, err := next()
			if err != nil {
				return opts, err
			}
			s, err := parseScope(v)
			if err != nil {
				return opts, err
			}
			opts.scope = s
		case strings.HasPrefix(a, "--scope="):
			s, err := parseScope(strings.TrimPrefix(a, "--scope="))
			if err != nil {
				return opts, err
			}
			opts.scope = s
		case a == "--os":
			v, err := next()
			if err != nil {
				return opts, err
			}
			o, err := parseOS(v)
			if err != nil {
				return opts, err
			}
			opts.targetOS = o
		case strings.HasPrefix(a, "--os="):
			o, err := parseOS(strings.TrimPrefix(a, "--os="))
			if err != nil {
				return opts, err
			}
			opts.targetOS = o
		default:
			return opts, fmt.Errorf("unknown argument %q", a)
		}
	}
	return opts, nil
}

// runHeadless performs discovery and either prints a plan (--dry-run) or
// downloads, verifies and installs the selected plugins.
func runHeadless(opts options) int {
	ctx := context.Background()
	client := release.NewClient(release.DefaultOwner, release.DefaultRepo, os.Getenv("GITHUB_TOKEN"))

	// installed versions from the receipt drive update detection.
	var installedVersions map[string]string
	if rec, err := install.LoadReceipt(); err == nil {
		installedVersions = rec.InstalledVersions()
	}

	cat, err := app.Discover(ctx, client, installedVersions)
	if err != nil {
		fmt.Fprintln(os.Stderr, "discover failed:", err)
		return 1
	}

	if opts.targetOS == "" {
		fmt.Fprintln(os.Stderr, "no target OS: pass --os macOS|Windows")
		return 2
	}

	slugs := resolveSlugs(opts.plugins, cat, opts.targetOS)
	formats, err := parseFormats(opts.formats, opts.targetOS)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 2
	}
	if len(slugs) == 0 || len(formats) == 0 {
		// Nothing selected: just report what discovery found.
		return printCatalog(opts, cat)
	}

	items, err := app.BuildPlanItems(cat, app.Selection{
		OS: opts.targetOS, Slugs: slugs, Formats: formats, Scope: opts.scope,
	})
	if err != nil {
		fmt.Fprintln(os.Stderr, "plan failed:", err)
		return 1
	}

	if opts.dryRun {
		return printPlan(opts, cat.Tag, items)
	}
	return headlessInstall(ctx, opts, client, cat, items)
}

// headlessInstall runs the actual (non-dry) install for --no-tui.
func headlessInstall(ctx context.Context, opts options, client *release.Client, cat release.Catalog, items []model.PlanItem) int {
	installer := &app.Installer{Client: client, Checksums: cat.Checksums, OS: opts.targetOS}
	res, installed, err := installer.Run(ctx, items, opts.scope, func(ev app.ProgressEvent) {
		if !opts.jsonOut {
			if ev.Err != nil {
				fmt.Fprintf(os.Stderr, "  %-10s %s (%s): %v\n", ev.Phase, ev.Item.Slug, ev.Item.Format, ev.Err)
			} else {
				fmt.Fprintf(os.Stderr, "  %-10s %s (%s)\n", ev.Phase, ev.Item.Slug, ev.Item.Format)
			}
		}
	})
	if err != nil {
		fmt.Fprintln(os.Stderr, "install failed:", err)
		return 1
	}

	versionOf := map[string]string{}
	for _, p := range cat.Plugins {
		versionOf[p.Slug] = p.Version
	}
	if err := app.WriteReceipt(installed, versionOf); err != nil {
		fmt.Fprintln(os.Stderr, "warning: could not write receipt:", err)
	}

	if opts.jsonOut {
		return emitJSON(map[string]any{"tag": cat.Tag, "installed": res.Installed, "errors": res.Errors})
	}
	fmt.Printf("installed %d bundle(s); %d error(s)\n", len(res.Installed), len(res.Errors))
	for _, e := range res.Errors {
		fmt.Fprintln(os.Stderr, "  error:", e)
	}
	if len(res.Errors) > 0 {
		return 1
	}
	return 0
}

func printCatalog(opts options, cat release.Catalog) int {
	if opts.jsonOut {
		return emitJSON(map[string]any{"tag": cat.Tag, "plugins": cat.Plugins})
	}
	fmt.Printf("release %s — %d plugins (target %s)\n", cat.Tag, len(cat.Plugins), opts.targetOS)
	for _, p := range cat.Plugins {
		var fmts []string
		for _, f := range []model.Format{model.FormatVST3, model.FormatAU} {
			if p.HasFormat(opts.targetOS, f) {
				fmts = append(fmts, string(f))
			}
		}
		fmt.Printf("  %-24s %-8s [%s]\n", p.Slug, p.Version, strings.Join(fmts, ","))
	}
	return 0
}

func printPlan(opts options, tag string, items []model.PlanItem) int {
	if opts.jsonOut {
		return emitJSON(map[string]any{"tag": tag, "scope": opts.scope, "items": items})
	}
	fmt.Printf("release %s — plan (%d item(s), scope=%s)\n", tag, len(items), opts.scope)
	for _, it := range items {
		fmt.Printf("  [%s] %-24s %-4s -> %s\n", it.Action, it.Slug, it.Format, it.Destination)
	}
	return 0
}

func emitJSON(v any) int {
	enc := json.NewEncoder(os.Stdout)
	enc.SetIndent("", "  ")
	if err := enc.Encode(v); err != nil {
		fmt.Fprintln(os.Stderr, "json:", err)
		return 1
	}
	return 0
}

// resolveSlugs expands "all" (or empty when nothing chosen) and filters to
// plugins that actually have an asset for the target OS.
func resolveSlugs(spec string, cat release.Catalog, osID model.OS) []string {
	spec = strings.TrimSpace(spec)
	if spec == "" {
		return nil
	}
	hasAsset := func(p model.Plugin) bool {
		return p.HasFormat(osID, model.FormatVST3) || p.HasFormat(osID, model.FormatAU)
	}
	if spec == "all" {
		var out []string
		for _, p := range cat.Plugins {
			if hasAsset(p) {
				out = append(out, p.Slug)
			}
		}
		return out
	}
	return splitCSV(spec)
}

func parseFormats(spec string, osID model.OS) ([]model.Format, error) {
	spec = strings.TrimSpace(spec)
	if spec == "" {
		return nil, nil
	}
	var out []model.Format
	for _, tok := range splitCSV(spec) {
		switch strings.ToLower(tok) {
		case "vst3":
			out = append(out, model.FormatVST3)
		case "au":
			if osID == model.OSWindows {
				return nil, fmt.Errorf("AU is not available on Windows")
			}
			out = append(out, model.FormatAU)
		default:
			return nil, fmt.Errorf("unknown format %q (want vst3 or au)", tok)
		}
	}
	return out, nil
}

func parseScope(v string) (model.Scope, error) {
	switch strings.ToLower(v) {
	case "system":
		return model.ScopeSystem, nil
	case "user":
		return model.ScopeUser, nil
	}
	return "", fmt.Errorf("unknown scope %q (want system or user)", v)
}

func parseOS(v string) (model.OS, error) {
	switch strings.ToLower(v) {
	case "macos", "mac", "darwin":
		return model.OSMacOS, nil
	case "windows", "win":
		return model.OSWindows, nil
	}
	return "", fmt.Errorf("unknown os %q (want macOS or Windows)", v)
}

// detectOS maps the runtime to a target OS; empty on unsupported platforms
// (e.g. linux CI), where --os must be given explicitly.
func detectOS() model.OS {
	switch runtime.GOOS {
	case "darwin":
		return model.OSMacOS
	case "windows":
		return model.OSWindows
	default:
		return ""
	}
}

func splitCSV(s string) []string {
	var out []string
	for _, p := range strings.Split(s, ",") {
		if p = strings.TrimSpace(p); p != "" {
			out = append(out, p)
		}
	}
	return out
}

func printUsage(w *os.File) {
	fmt.Fprintf(w, `tatsunari-installer %s — TUI installer for tatsunari-plugins

Usage:
  tatsunari-installer                 launch the interactive TUI
  tatsunari-installer --no-tui --plugins <a,b|all> --formats <vst3,au> [--scope system|user]
  tatsunari-installer --dry-run ...   compute and print the plan only

Flags:
  --no-tui            headless mode
  --dry-run           show the plan; download nothing, install nothing
  --json              machine-readable output
  --plugins <list>    comma-separated slugs, or "all"
  --formats <list>    comma-separated: vst3, au (au is macOS-only)
  --scope <s>         system (default, needs OS password) or user (no password)
  --os <os>           override target OS: macOS or Windows
  --version, --help
`, version)
}
