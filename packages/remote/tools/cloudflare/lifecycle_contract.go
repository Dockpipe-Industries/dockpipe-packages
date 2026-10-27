package cloudflare

import "time"

const InventorySchema = "dockpipe.dependency.inventory/v1"
const LifecycleProviderID = "dockpipe.cloudflare.remote-edge"
const LifecycleProviderVersion = "0.3.1"
const advisoryURL = "https://blog.cloudflare.com/emudhra-certificate-compromise/"

type InventoryProvider struct {
	ID      string `json:"id"`
	Version string `json:"version"`
}

type DependencyAssessment struct {
	Status       string `json:"status"`
	AdvisoryURL  string `json:"advisory_url"`
	FixedVersion string `json:"fixed_version"`
	Reason       string `json:"reason"`
}

type DependencyObservation struct {
	ID               string               `json:"id"`
	Dependency       string               `json:"dependency"`
	Location         string               `json:"location"`
	Method           string               `json:"method"`
	InstalledVersion string               `json:"installed_version"`
	ActiveVersion    string               `json:"active_version"`
	ActiveState      string               `json:"active_state"`
	Assessment       DependencyAssessment `json:"assessment"`
}

type DependencyInventory struct {
	Schema       string                  `json:"schema"`
	Provider     InventoryProvider       `json:"provider"`
	Machine      string                  `json:"machine"`
	ObservedAt   time.Time               `json:"observed_at"`
	Observations []DependencyObservation `json:"observations"`
	Gaps         []string                `json:"gaps"`
}
