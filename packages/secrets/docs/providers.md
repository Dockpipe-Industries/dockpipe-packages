# Secret environment providers

The `secrets` package owns the provider adapters below. Install a compatible published package and the chosen vendor CLI; Dockpipe core supplies the provider-neutral resolver exchange. See the [core secret-environment contract](https://github.com/Dockpipe-Industries/dockpipe/blob/master/docs/runtime/vault.md).

These setup details were preserved from the Dockpipe vault reference during a documentation-only ownership audit. Vendor CLI compatibility and live authentication were not requalified; the Infisical version below records prior verification rather than a requirement to install that exact version.

## Provider parameters

| Resolver | Required parameters | Optional parameters | Binding source | Host authentication |
| --- | --- | --- | --- | --- |
| `onepassword` | `environment_id` | `account` | 1Password Environment variable | Authenticated `op` CLI; requires a build supporting `op run --environment` (check your installed CLI for this command) |
| `aws-secretsmanager` | `secret_id` | `region`, `profile`, `version_stage` | Top-level string key in a JSON `SecretString` | AWS CLI credential chain, including profiles, SSO, and workload identity |
| `azure-keyvault` | `vault_name` | `subscription` | Key Vault secret name | Authenticated Azure CLI, including managed/workload identity |
| `infisical` | `project_id`, `environment` | `path`, `domain`, `profile`, `organization_id` | Variable in the selected project/environment/path | CLI session with browser login when needed, or `INFISICAL_TOKEN` |

Azure retrieves each bound secret by name. AWS reads one JSON secret; `SecretBinary` and non-string selected fields are unsupported. Secret resolution errors withhold stdout/stderr so credentials cannot be printed by the resolver error path. Dockpipe does not provision access policies or machine credentials. `op`/Infisical may fetch their whole remote environment internally, so use a tightly scoped remote environment as well as a narrow binding list.

### Infisical browser login

Set a dedicated `profile` and the intended `domain` and `organization_id` alongside
the project/environment parameters. Dockpipe checks that CLI session before reading
secrets. If it is missing, expired or rejected, a terminal run starts browser login,
then resumes the same workflow. Existing profiles are renewed with `--profile`;
new profiles are created with `--save-as`. The global default profile is unchanged.
An inherited `INFISICAL_PROFILE` is also accepted when `profile` is omitted.
Choose the intended account in the browser. `organization_id` scopes subsequent
CLI status/read commands; it is not passed as the CLI's direct-password login flag.

```json
{
  "resolver": "infisical",
  "parameters": {
    "project_id": "project-id",
    "environment": "dev",
    "path": "/",
    "domain": "https://app.infisical.com/api",
    "profile": "my-project-dev",
    "organization_id": "organization-id"
  },
  "bindings": {
    "TOKEN": "APP_TOKEN"
  }
}
```

Use a current Infisical CLI with `login status --json` and named-profile support
(verified with 0.43.140). Authentication belongs to the CLI's credential store;
Dockpipe does not write tokens or application secrets to project files. The login
UI displays only a validated browser challenge URL. On Linux Dockpipe also attempts
to open that URL; it remains clickable if the browser launcher is unavailable.

Both stdin and stderr must be terminals, and `CI` must be unset or false/0.
Unattended runs fail with an authentication-required message instead of opening a
browser. `INFISICAL_TOKEN` always uses machine/token authentication without browser
fallback. Keyring, network, permission and missing-secret failures do not trigger
login loops. Login has a five-minute deadline and is attempted at most once.

Examples of provider parameters:

Each object below is one environment entry:

```json
{"resolver":"aws-secretsmanager","parameters":{"secret_id":"packages/production","region":"us-east-1"},"bindings":{"TOKEN":"publish_token"}}
```

```json
{"resolver":"azure-keyvault","parameters":{"vault_name":"packages-production"},"bindings":{"TOKEN":"publish-token"}}
```

```json
{"resolver":"infisical","parameters":{"project_id":"project-id","environment":"prod","path":"/packages"},"bindings":{"TOKEN":"PUBLISH_TOKEN"}}
```

