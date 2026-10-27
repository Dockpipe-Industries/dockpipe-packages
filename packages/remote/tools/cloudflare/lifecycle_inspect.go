package cloudflare

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"sort"
	"strings"
	"time"
	"unicode"
	"unicode/utf8"

	"dockpipe/src/lib/infrastructure/process"
)

const maximumInstallations = 32
const maximumProcesses = 4096
const maximumActiveImages = 64
const maximumInventoryBytes = 1 << 20

var machineIdentifier = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9_.-]{0,127}$`)

type inspectedExecutable struct {
	path    string
	info    os.FileInfo
	version string
}

type inventoryInspector struct {
	platform string
	paths    []string
	probe    func(context.Context, string) (string, error)
	active   func(context.Context) ([]inspectedExecutable, []string)
	now      func() time.Time
	budget   time.Duration
}

// InspectDependencies only inventories local executables and readable process
// images. It never invokes login, install, update, service or tunnel commands.
func InspectDependencies(ctx context.Context, machine string) (DependencyInventory, error) {
	paths, gaps := executableCandidates()
	inspector := inventoryInspector{
		platform: runtime.GOOS,
		paths:    paths,
		probe:    probeExecutableVersion,
		active:   inspectActiveImages,
		now:      time.Now,
		budget:   30 * time.Second,
	}
	return inspector.inspect(ctx, machine, gaps)
}

func (inspector inventoryInspector) inspect(ctx context.Context, machine string, gaps []string) (DependencyInventory, error) {
	if !machineIdentifier.MatchString(machine) {
		return DependencyInventory{}, errors.New("machine must be a bounded nonempty identifier")
	}
	workContext := ctx
	if inspector.budget > 0 {
		var cancel context.CancelFunc
		workContext, cancel = context.WithTimeout(ctx, inspector.budget)
		defer cancel()
	}
	report := DependencyInventory{
		Schema: InventorySchema,
		Provider: InventoryProvider{
			ID:      LifecycleProviderID,
			Version: LifecycleProviderVersion,
		},
		Machine:      machine,
		ObservedAt:   inspector.now().UTC(),
		Observations: []DependencyObservation{},
		Gaps:         append([]string{}, gaps...),
	}
	report.Gaps = append(report.Gaps, "Container images, package-manager inventories, service definitions and inaccessible or undiscovered installation locations are not inspected.")
	var installed []inspectedExecutable
	seen := map[string]bool{}
	for _, path := range inspector.paths {
		if workContext.Err() != nil {
			break
		}
		canonical, err := filepath.EvalSymlinks(path)
		if err != nil || !safeInventoryPath(canonical) || seen[canonical] {
			continue
		}
		info, err := os.Stat(canonical)
		if err != nil || !info.Mode().IsRegular() || (runtime.GOOS != "windows" && info.Mode().Perm()&0111 == 0) {
			continue
		}
		duplicate := false
		for _, previous := range installed {
			if os.SameFile(previous.info, info) {
				duplicate = true
				break
			}
		}
		if duplicate {
			continue
		}
		if len(installed) == maximumInstallations {
			report.Gaps = append(report.Gaps, "Installation inventory reached its 32 executable limit; additional candidates were not inspected.")
			break
		}
		seen[canonical] = true
		version, _ := inspector.probe(workContext, canonical)
		after, err := os.Stat(canonical)
		if err != nil || !unchangedImage(info, after) {
			version = ""
		}
		installed = append(installed, inspectedExecutable{path: canonical, info: info, version: version})
	}
	var active []inspectedExecutable
	if workContext.Err() == nil {
		var activeGaps []string
		active, activeGaps = inspector.active(workContext)
		report.Gaps = append(report.Gaps, activeGaps...)
	}
	if err := ctx.Err(); err != nil {
		return DependencyInventory{}, err
	}
	if workContext.Err() != nil {
		report.Gaps = append(report.Gaps, "The 30-second inventory budget expired; installation and active-image coverage is partial.")
	}
	for _, executable := range installed {
		observation := executableObservation(executable.path, "host-executable-version", executable.version, "", inspector.platform)
		for _, image := range active {
			if unchangedImage(executable.info, image.info) {
				observation.ActiveState = "running"
				observation.ActiveVersion = image.version
				if image.version != "" && executable.version == image.version {
					observation.Assessment = assessDependency(image.version, inspector.platform, true)
				}
			}
		}
		report.Observations = append(report.Observations, observation)
	}
	for _, image := range active {
		observation := executableObservation(image.path, "linux-process-image-version", "", image.version, inspector.platform)
		observation.ActiveState = "running"
		observation.Assessment = assessDependency(image.version, inspector.platform, image.version != "")
		report.Observations = append(report.Observations, observation)
	}
	if len(report.Observations) == 0 {
		report.Gaps = append(report.Gaps, "No readable cloudflared executable was discovered; absence of observations is not evidence that the dependency is absent or fixed.")
	}
	sort.Slice(report.Observations, func(left, right int) bool { return report.Observations[left].ID < report.Observations[right].ID })
	return report, nil
}

func executableObservation(path, method, installed, active, platform string) DependencyObservation {
	digest := sha256.Sum256([]byte(method + "\n" + path))
	return DependencyObservation{
		ID:               "cloudflared-" + hex.EncodeToString(digest[:12]),
		Dependency:       "cloudflared",
		Location:         path,
		Method:           method,
		InstalledVersion: installed,
		ActiveVersion:    active,
		ActiveState:      "unknown",
		Assessment:       assessDependency(installed, platform, false),
	}
}

func safeInventoryPath(path string) bool {
	if !filepath.IsAbs(path) || len(path) > 4096 || !utf8.ValidString(path) {
		return false
	}
	return !strings.ContainsFunc(path, unicode.IsControl)
}

func unchangedImage(before, after os.FileInfo) bool {
	return before != nil && after != nil && os.SameFile(before, after) && before.Size() == after.Size() && before.ModTime() == after.ModTime()
}

func executableCandidates() ([]string, []string) {
	name := "cloudflared"
	if runtime.GOOS == "windows" {
		name += ".exe"
	}
	var paths, gaps []string
	for index, directory := range filepath.SplitList(os.Getenv("PATH")) {
		if index == 64 {
			gaps = append(gaps, "PATH inventory reached its 64 entry limit.")
			break
		}
		if filepath.IsAbs(directory) && safeInventoryPath(directory) {
			paths = append(paths, filepath.Join(directory, name))
		}
	}
	for _, directory := range []string{"/usr/bin", "/usr/local/bin", "/usr/sbin", "/usr/local/sbin", "/opt/homebrew/bin", "/opt/cloudflared", "/app/bin"} {
		paths = append(paths, filepath.Join(directory, name))
	}
	if executable, err := os.Executable(); err == nil {
		paths = append(paths, filepath.Join(filepath.Dir(executable), name))
	}
	if runtime.GOOS == "windows" {
		paths = append(paths, `C:\Program Files\cloudflared\cloudflared.exe`, `C:\ProgramData\chocolatey\bin\cloudflared.exe`)
	}
	return paths, gaps
}

type inventoryOutput struct {
	data []byte
}

func (output *inventoryOutput) Write(data []byte) (int, error) {
	if len(output.data)+len(data) > 4096 {
		return 0, errors.New("version output exceeded its bound")
	}
	output.data = append(output.data, data...)
	return len(data), nil
}

func probeExecutableVersion(ctx context.Context, path string) (string, error) {
	ctx, cancel := context.WithTimeout(ctx, 2*time.Second)
	defer cancel()
	command := exec.CommandContext(ctx, path, "--version")
	command.Dir = string(filepath.Separator)
	command.Env = []string{"PATH=/usr/bin:/bin", "LANG=C", "LC_ALL=C"}
	command.WaitDelay = 100 * time.Millisecond
	output := &inventoryOutput{}
	command.Stdout, command.Stderr = output, output
	if err := process.RunNested(command); err != nil {
		return "", errors.New("executable version is unavailable")
	}
	version := cloudflaredVersion(output.data)
	if version == "" {
		return "", errors.New("unrecognized cloudflared version output")
	}
	return version, nil
}

func InspectCommand(ctx context.Context, arguments []string) error {
	flags := flag.NewFlagSet("cloudflare-edge lifecycle-inspect", flag.ContinueOnError)
	flags.SetOutput(io.Discard)
	output := flags.String("output", "", "new private report output")
	machine := flags.String("machine", "", "bounded machine identity")
	if err := flags.Parse(arguments); err != nil {
		return errors.New("lifecycle-inspect requires --output <absolute new file> --machine <identifier>")
	}
	if flags.NArg() != 0 || !safeInventoryPath(*output) || filepath.Clean(*output) != *output || !machineIdentifier.MatchString(*machine) {
		return errors.New("lifecycle-inspect requires --output <absolute new file> --machine <identifier>")
	}
	report, err := InspectDependencies(ctx, *machine)
	if err != nil {
		return err
	}
	data, err := json.Marshal(report)
	if err != nil || len(data)+1 > maximumInventoryBytes {
		return errors.New("dependency report exceeds its 1 MiB bound")
	}
	if err := ctx.Err(); err != nil {
		return err
	}
	return writeInventoryReport(*output, data)
}

func writeInventoryReport(path string, data []byte) error {
	file, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if err != nil {
		return errors.New("dependency report output already exists or cannot be created")
	}
	complete := false
	defer func() {
		file.Close()
		if !complete {
			os.Remove(path)
		}
	}()
	if _, err := file.Write(append(data, '\n')); err != nil {
		return errors.New("dependency report could not be written")
	}
	if err := file.Sync(); err != nil {
		return errors.New("dependency report could not be synchronized")
	}
	if err := file.Close(); err != nil {
		return errors.New("dependency report could not be closed")
	}
	complete = true
	return nil
}
