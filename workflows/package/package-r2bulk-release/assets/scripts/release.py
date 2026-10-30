#!/usr/bin/env python3
"""Release exactly one portable workflow; never promote the global catalog."""
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tarfile

NAME = "dockpipe.cloudflare.r2bulk"
VERSION = "0.1.1"
TAG = NAME + "-v" + VERSION
REPOSITORY = "Dockpipe-Industries/dockpipe-packages"
SOURCE = Path("packages/cloud/storage/resolvers/r2") / NAME
CLI_SHA256 = "fc73040456eb229b13b05ed59632d6b30d4ad9419b22afd3bcd4babe85d8f183"


def run(command, **kwargs):
    return subprocess.run([str(part) for part in command], check=True, **kwargs)


def output(command, **kwargs):
    return subprocess.check_output([str(part) for part in command], text=True, **kwargs).strip()


def digest(path):
    hasher = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            hasher.update(block)
    return hasher.hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def admission(root, binary):
    if not binary.is_absolute() or not binary.is_file() or digest(binary) != CLI_SHA256:
        raise ValueError("Select the SHA-256 pinned released DockPipe 0.6.25 executable")
    revision = os.environ.get("R2BULK_SOURCE_COMMIT", "")
    if not re.fullmatch(r"[0-9a-f]{40}", revision):
        raise ValueError("R2BULK_SOURCE_COMMIT must identify the exact committed release source")
    if output(["git", "-C", root, "rev-parse", "HEAD"]) != revision:
        raise ValueError("Release source commit differs from HEAD")
    if output(["git", "-C", root, "status", "--porcelain", "--untracked-files=all"]):
        raise ValueError("Commit the package and producer; release source checkout must be clean")
    return revision


def release_directory(binary):
    explicit = os.environ.get("R2BULK_RELEASE_OUTPUT")
    if explicit:
        path = Path(explicit)
        if not path.is_absolute():
            raise ValueError("R2BULK_RELEASE_OUTPUT must be absolute")
        return path
    return Path(output([binary, "scope", "artifacts", "r2bulk-release-0.1.1"]))


def extract(archive, destination):
    """The portable release contains only regular files and directories."""
    with tarfile.open(archive, "r:gz") as bundle:
        members = bundle.getmembers()
        for member in members:
            path = Path(member.name)
            if path.is_absolute() or ".." in path.parts or not (member.isfile() or member.isdir()):
                raise ValueError("Unexpected archive member: " + member.name)
        bundle.extractall(destination, members=members)


def package_entry(manifest):
    packages = manifest["packages"]
    workflows = packages.get("workflows", [])
    if packages.get("core") or packages.get("resolvers") or len(workflows) != 1:
        raise ValueError("Release catalog must contain exactly the requested workflow")
    entry = workflows[0]
    if entry["name"] != NAME or entry["version"] != VERSION:
        raise ValueError("Unexpected workflow identity in catalog")
    if Path(entry["tarball"]).name != entry["tarball"]:
        raise ValueError("Catalog archive must be a local asset basename")
    return entry


def inventory(root):
    entries = []
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise ValueError("Portable source must not contain symlinks")
        if path.is_file():
            entries.append({"path": path.relative_to(root).as_posix(),
                            "sha256": digest(path), "size_bytes": path.stat().st_size})
    return entries


def build(root, binary, directory, revision):
    if directory.exists() and any(directory.iterdir()):
        raise ValueError("Use a fresh output scope; existing release evidence is immutable")
    directory.mkdir(parents=True, exist_ok=True)
    project = directory / "project"
    source = project / "source" / NAME
    source.parent.mkdir(parents=True)
    # Export tracked bytes from the admitted commit, without copying ignored files.
    snapshot = directory / "source.tar.gz"
    run(["git", "-C", root, "archive", "--format=tar.gz", "-o", snapshot, revision, SOURCE])
    exported = directory / "exported"
    extract(snapshot, exported)
    shutil.copytree(exported / SOURCE, source)
    if not re.search(r"(?m)^version:\s*0\.1\.1\s*$", (source / "package.yml").read_text()):
        raise ValueError("Source package must declare version 0.1.1")
    write_json(project / "dockpipe.config.json", {"schema": 1, "packages": {"namespace": "dockpipe-cloudflare"}})
    assets = directory / "assets"
    assets.mkdir()
    run([binary, "package", "compile", "workflows", "--workdir", project, "--from", source.parent, "--force"])
    run([binary, "package", "build", "store", "--workdir", project, "--out", assets, "--only", "workflows"])
    manifest_path = assets / "packages-store-manifest.json"
    manifest = json.loads(manifest_path.read_text())
    entry = package_entry(manifest)
    # This package has no native binary. Host platform is not a distribution limit.
    manifest.pop("platform", None)
    manifest.pop("store_root", None)
    write_json(manifest_path, manifest)
    if digest(assets / entry["tarball"]) != entry["sha256"]:
        raise ValueError("Compiled archive checksum mismatch")
    write_json(assets / "source-inventory.json", inventory(exported / SOURCE))
    write_json(assets / "provenance.json", {
        "schema": 1, "repository": REPOSITORY, "source_commit": revision,
        "package": NAME, "version": VERSION, "tag": TAG,
        "dockpipe_version": "0.6.25", "dockpipe_sha256": CLI_SHA256,
        "requirements_lock_sha256": digest(source / "requirements.lock"),
        "archive_sha256": entry["sha256"], "distribution": "portable Python and shell workflow",
    })


def provider_free_environment():
    environment = dict(os.environ)
    for key in list(environment):
        if key.startswith(("AWS_", "R2_")):
            environment.pop(key)
    environment.update(AWS_EC2_METADATA_DISABLED="true", AWS_SHARED_CREDENTIALS_FILE=os.devnull,
                       AWS_CONFIG_FILE=os.devnull)
    return environment


def qualify(binary, directory, revision):
    assets = directory / "assets"
    manifest = json.loads((assets / "packages-store-manifest.json").read_text())
    entry = package_entry(manifest)
    if digest(assets / entry["tarball"]) != entry["sha256"]:
        raise ValueError("Archive changed before qualification")
    provenance = json.loads((assets / "provenance.json").read_text())
    if provenance["source_commit"] != revision:
        raise ValueError("Qualification source differs from build source")
    unpacked = directory / "qualification"
    if unpacked.exists():
        raise ValueError("Qualification evidence already exists; use a fresh release output")
    extract(assets / entry["tarball"], unpacked)
    package = unpacked / "workflows" / NAME
    if digest(package / "requirements.lock") != provenance["requirements_lock_sha256"]:
        raise ValueError("Extracted SDK lock differs from admitted source")
    for path in package.rglob("*"):
        if path.is_file():
            magic = path.read_bytes()[:4]
            if magic.startswith((b"\x7fELF", b"MZ")) or magic in (b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf"):
                raise ValueError("Portable workflow unexpectedly contains a native binary")
    python = Path(os.environ["R2_BULK_PYTHON"])
    if not python.is_absolute() or not python.is_file():
        raise ValueError("R2_BULK_PYTHON must select the locked SDK environment")
    environment = provider_free_environment()
    environment["R2_BULK_PYTHON"] = str(python)
    with (directory / "package-tests.log").open("w") as log:
        run(["bash", package / "tests/run.sh"], env=environment, stdout=log, stderr=subprocess.STDOUT)
    test_log = (directory / "package-tests.log").read_text()
    count = re.search(r"(?m)^Ran (\d+) tests? in ", test_log)
    if count is None or int(count.group(1)) != 27 or re.search(r"(?m)^OK \(skipped=", test_log):
        raise ValueError("Release requires all 27 extracted package tests without skips")
    # A second interpreter has no site packages: dry-run and skip cannot import SDK.
    wrapper = directory / "python-without-site.sh"
    wrapper.write_text("#!/usr/bin/env bash\nexec " + shlex.quote(sys.executable) + " -S \"$@\"\n")
    wrapper.chmod(0o755)
    payload = directory / "payload.txt"
    payload.write_text("portable release qualification\n")
    objects = directory / "objects.json"
    write_json(objects, {"schema": 1, "objects": [{"source": str(payload), "key": "qualification/payload.txt",
               "size": payload.stat().st_size, "sha256": digest(payload), "publish_last": False}]})
    for mode in ("dry-run", "skip"):
        environment = provider_free_environment()
        environment.update(R2_BULK_PYTHON=str(wrapper), R2_BULK_SKIP=str(mode == "skip").lower(),
                           R2_BULK_DRY_RUN=str(mode == "dry-run").lower(),
                           R2_BULK_MANIFEST=str(objects), R2_BULK_STATE=str(directory / (mode + "-state.json")),
                           R2_BUCKET="qualification-bucket", R2_ENDPOINT_URL="https://example.invalid")
        if mode == "skip":
            environment.pop("R2_BULK_MANIFEST")
            environment.pop("R2_BULK_STATE")
            environment.pop("R2_BUCKET")
            environment.pop("R2_ENDPOINT_URL")
        with (directory / (mode + ".log")).open("w") as log:
            run([binary, "--workflow-file", package / "config.yml", "--workdir", directory / "project"],
                env=environment, stdout=log, stderr=subprocess.STDOUT)
        phases = []
        for line in (directory / (mode + ".log")).read_text().splitlines():
            if line.startswith("{"):
                message = json.loads(line)
                if message.get("phase") == ("skipped" if mode == "skip" else "dry-run"):
                    if message.get("provider_operations") is not False:
                        raise ValueError("Provider-free qualification did not report its boundary")
                    phases.append(message["stage"])
        if phases != ["preflight", "transfer", "verify"]:
            raise ValueError("Released CLI did not execute all three " + mode + " phases")
    write_json(assets / "qualification.json", {
        "schema": 1, "source_commit": revision, "archive_sha256": entry["sha256"],
        "host": platform.platform(), "native_host": platform.system(),
        "package_tests": "27 extracted archive tests/run.sh tests with exact locked SDK; passed without skips",
        "dry_run": "three released-CLI phases, no SDK or credentials; passed",
        "skip": "three released-CLI phases, no SDK or credentials or manifest; passed",
        "windows": "portable mocked tests only; no native Windows execution",
        "macos": "no native macOS execution",
    })
    paths = sorted(path for path in assets.iterdir() if path.is_file())
    (assets / "SHA256SUMS").write_text("".join(digest(path) + "  " + path.name + "\n" for path in paths))


def verify_assets(assets, revision):
    qualification = json.loads((assets / "qualification.json").read_text())
    provenance = json.loads((assets / "provenance.json").read_text())
    if qualification["source_commit"] != revision or provenance["source_commit"] != revision:
        raise ValueError("Publication source differs from qualified source")
    entry = package_entry(json.loads((assets / "packages-store-manifest.json").read_text()))
    archive_hash = digest(assets / entry["tarball"])
    if any(expected != archive_hash for expected in (
            entry["sha256"], qualification["archive_sha256"], provenance["archive_sha256"])):
        raise ValueError("Manifest, qualification and provenance must identify the same archive")
    paths = []
    for line in (assets / "SHA256SUMS").read_text().splitlines():
        expected, name = line.split("  ", 1)
        if Path(name).name != name or not re.fullmatch(r"[0-9a-f]{64}", expected):
            raise ValueError("Invalid asset checksum entry")
        path = assets / name
        if digest(path) != expected:
            raise ValueError("Qualified asset changed: " + name)
        paths.append(path)
    actual = {path.name for path in assets.iterdir() if path.is_file() and path.name != "SHA256SUMS"}
    if actual != {path.name for path in paths}:
        raise ValueError("Asset checksum inventory differs from release directory")
    return paths + [assets / "SHA256SUMS"]


def require_absent(endpoint):
    result = subprocess.run(["gh", "api", "--include", endpoint], capture_output=True, text=True)
    if result.returncode == 0:
        raise ValueError("Existing release/tag requires separate review: " + endpoint)
    if not re.search(r"(?m)^HTTP/\S+ 404(?: |$)", result.stdout):
        raise ValueError("Cannot prove release/tag absence: " + result.stderr.strip())


def verify_tag(revision):
    reference = json.loads(output(["gh", "api", "repos/" + REPOSITORY + "/git/ref/tags/" + TAG]))
    target = reference["object"]
    for _ in range(4):
        if target["type"] == "commit":
            if target["sha"] != revision:
                raise ValueError("Release tag differs from the qualified source commit")
            return
        if target["type"] != "tag" or not re.fullmatch(r"[0-9a-f]{40}", target["sha"]):
            raise ValueError("Unexpected release tag target")
        target = json.loads(output(["gh", "api", "repos/" + REPOSITORY + "/git/tags/" + target["sha"]]))["object"]
    raise ValueError("Release tag indirection exceeds the bounded verification limit")


def publish(directory, revision):
    assets = directory / "assets"
    paths = verify_assets(assets, revision)
    execute = os.environ.get("R2BULK_RELEASE_EXECUTE", "false")
    if execute not in ("true", "false"):
        raise ValueError("R2BULK_RELEASE_EXECUTE must be exactly true or false")
    if execute != "true":
        print(json.dumps({"dry_run": True, "repository": REPOSITORY, "tag": TAG,
                          "source_commit": revision, "assets": [path.name for path in paths]}))
        return
    verify_tag(revision)
    require_absent("repos/" + REPOSITORY + "/releases/tags/" + TAG)
    run(["gh", "release", "create", TAG, "--repo", REPOSITORY, "--verify-tag",
         "--draft", "--title", NAME + " " + VERSION, "--notes",
         "Focused portable R2 bulk workflow. Qualification and provenance are attached. No native Windows/macOS claim."])
    run(["gh", "release", "upload", TAG, "--repo", REPOSITORY, *paths])
    run(["gh", "release", "edit", TAG, "--repo", REPOSITORY, "--draft=false", "--latest=false"])
    verify_tag(revision)
    readback = directory / "public-readback"
    readback.mkdir()
    environment = dict(os.environ)
    for key in ("GH_TOKEN", "GITHUB_TOKEN", "GH_ENTERPRISE_TOKEN", "GITHUB_ENTERPRISE_TOKEN"):
        environment.pop(key, None)
    for path in paths:
        url = "https://github.com/" + REPOSITORY + "/releases/download/" + TAG + "/" + path.name
        target = readback / path.name
        run(["curl", "--disable", "--fail", "--location", "--retry", "4", "--retry-all-errors",
             "--silent", "--show-error", "--connect-timeout", "15", "--max-time", "120",
             "--max-filesize", str(path.stat().st_size),
             "--proto", "=https", "--proto-redir", "=https", "--output", target, url], env=environment)
        if digest(target) != digest(path):
            raise ValueError("Public unauthenticated release asset digest mismatch: " + path.name)
    write_json(directory / "publication.json", {"tag": TAG, "source_commit": revision,
               "public_readback": "all asset SHA-256 values matched", "latest": False})


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in ("build", "qualify", "publish"):
        raise ValueError("Select build, qualify or publish")
    root = Path(output(["git", "rev-parse", "--show-toplevel"]))
    binary = Path(os.environ["DOCKPIPE_BIN"])
    revision = admission(root, binary)
    directory = release_directory(binary)
    if sys.argv[1] == "build":
        build(root, binary, directory, revision)
    elif sys.argv[1] == "qualify":
        qualify(binary, directory, revision)
    else:
        publish(directory, revision)


if __name__ == "__main__":
    main()
