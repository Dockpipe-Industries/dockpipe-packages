package cloudflare

import "testing"

func TestLifecycleAssessmentStrictCalendarVersions(t *testing.T) {
	tests := []struct {
		name     string
		version  string
		platform string
		active   bool
		status   string
		fixed    string
	}{
		{"old running", "2026.9.9", "linux", true, "affected", "2026.10.2"},
		{"old installed", "2025.12.100", "linux", false, "affected", "2026.10.2"},
		{"fixed installed active unknown", "2026.10.2", "linux", false, "unknown", "2026.10.2"},
		{"fixed running", "2026.10.2", "linux", true, "not_affected", "2026.10.2"},
		{"later numeric running", "2026.11.0", "linux", true, "not_affected", "2026.10.2"},
		{"mac channel unconfirmed", "2026.10.2", "darwin", true, "unknown", "2026.10.3"},
		{"mac conservative floor", "2026.10.3", "darwin", true, "not_affected", "2026.10.3"},
		{"prerelease", "2026.10.3-rc1", "linux", true, "unknown", "2026.10.2"},
		{"vendor suffix", "2026.10.3+vendor", "linux", true, "unknown", "2026.10.2"},
		{"leading zero month", "2026.010.3", "linux", true, "unknown", "2026.10.2"},
		{"leading zero patch", "2026.10.03", "linux", true, "unknown", "2026.10.2"},
		{"invalid month", "2026.13.3", "linux", true, "unknown", "2026.10.2"},
		{"missing", "", "linux", true, "unknown", "2026.10.2"},
		{"malformed", "2026.10", "linux", true, "unknown", "2026.10.2"},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			assessment := assessDependency(test.version, test.platform, test.active)
			if assessment.Status != test.status || assessment.FixedVersion != test.fixed {
				t.Fatalf("assessment = %+v, want %s / %s", assessment, test.status, test.fixed)
			}
			if assessment.AdvisoryURL != advisoryURL || assessment.Reason == "" {
				t.Fatalf("assessment lacks advisory provenance: %+v", assessment)
			}
		})
	}
}

func TestLifecycleVersionBannerDoesNotExportUnrecognizedOutput(t *testing.T) {
	tests := []struct {
		output string
		want   string
	}{
		{"cloudflared version 2026.10.2 (built 2026-10-16-1553 UTC)\n", "2026.10.2"},
		{"cloudflared version 2026.10.3-rc1\n", "2026.10.3-rc1"},
		{"cloudflared version 2026.10.2\nTOKEN=private", ""},
		{"TOKEN=private", ""},
		{"cloudflared version 2026.10.2\ncloudflared version 2026.10.3", ""},
		{"cloudflared version 2026.10.2 (private\nvalue)", ""},
	}
	for _, test := range tests {
		if got := cloudflaredVersion([]byte(test.output)); got != test.want {
			t.Errorf("banner %q = %q, want %q", test.output, got, test.want)
		}
	}
}
