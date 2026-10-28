package cloudflare

import (
	"regexp"
	"strconv"
	"strings"
)

var calendarVersion = regexp.MustCompile(`^([1-9][0-9]{3})\.([1-9]|1[0-2])\.(0|[1-9][0-9]{0,5})$`)
var versionBanner = regexp.MustCompile(`^cloudflared version ([0-9]{4}\.[A-Za-z0-9.+_-]{1,75})(?: \([ -~]{1,256}\))?$`)

func parseCalendarVersion(value string) ([3]int, bool) {
	var version [3]int
	parts := calendarVersion.FindStringSubmatch(value)
	if parts == nil {
		return version, false
	}
	for index := range version {
		number, err := strconv.Atoi(parts[index+1])
		if err != nil {
			return [3]int{}, false
		}
		version[index] = number
	}
	return version, true
}

func cloudflaredVersion(output []byte) string {
	parts := versionBanner.FindStringSubmatch(strings.TrimSpace(string(output)))
	if parts == nil {
		return ""
	}
	return parts[1]
}

func compareCalendarVersion(left, right [3]int) int {
	for index := range left {
		if left[index] < right[index] {
			return -1
		}
		if left[index] > right[index] {
			return 1
		}
	}
	return 0
}

func assessDependency(version, platform string, activeProven bool) DependencyAssessment {
	fixed := "2026.10.2"
	if platform == "darwin" {
		fixed = "2026.10.3"
	}
	assessment := DependencyAssessment{Status: "unknown", AdvisoryURL: advisoryURL, FixedVersion: fixed}
	parsed, valid := parseCalendarVersion(version)
	if !valid {
		assessment.Reason = "Version is unavailable or is not a strict stable calendar release; prerelease and vendor suffixes are not assumed fixed."
		return assessment
	}
	if compareCalendarVersion(parsed, [3]int{2026, 10, 2}) < 0 {
		assessment.Status = "affected"
		assessment.Reason = "Observed executable predates Cloudflare's advisory fix. Installed presence alone does not establish active use."
		return assessment
	}
	if platform == "darwin" && compareCalendarVersion(parsed, [3]int{2026, 10, 3}) < 0 {
		assessment.Reason = "Cloudflare names 2026.10.2, but this macOS distribution channel is unconfirmed; this provider conservatively requires 2026.10.3."
		return assessment
	}
	if !activeProven {
		assessment.Reason = "Installed version meets the advisory floor, but the active executable image/version has not been proven."
		return assessment
	}
	assessment.Status = "not_affected"
	assessment.Reason = "The observed active executable image meets this advisory's version floor; other processes, containers and verification paths remain separate coverage."
	return assessment
}
