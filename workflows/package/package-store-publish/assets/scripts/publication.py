#!/usr/bin/env python3
"""Prepare immutable store payloads, then publish metadata after bulk verification."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def enabled(value):
    return value.lower() in ("1", "true")


def checksum(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def content_type(name):
    types = [
        (".json", "application/json"),
        (".sha256", "text/plain; charset=utf-8"),
        (".tar.gz", "application/gzip"),
        (".zip", "application/zip"),
        (".deb", "application/vnd.debian.binary-package"),
        (".rpm", "application/x-rpm"),
        (".msi", "application/x-msi"),
        (".pkg.tar.zst", "application/zstd"),
    ]
    for suffix, value in types:
        if name.endswith(suffix):
            return value
    return "application/octet-stream"


def metadata_file(name):
    return name.endswith((".json", ".json.sha256", ".json.sig", ".json.asc"))


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def prepare(environment, output):
    skipped = enabled(environment.get("PACKAGE_RELEASE_SKIP_UPLOAD", "0"))
    dry_run = enabled(environment.get("R2_PUBLISH_DRY_RUN", "1"))
    root = Path(environment.get("DOCKPIPE_SOURCE_ROOT", os.getcwd()))
    files = []
    if not skipped:
        directory = root / environment.get("RELEASE_ARTIFACTS_DIR", "release/artifacts")
        if not directory.is_dir():
            raise ValueError("Package release artifacts directory is missing")
        files = sorted(path for path in directory.iterdir() if path.is_file())
        if not files:
            raise ValueError("No package release artifacts were selected")
        if any(path.is_symlink() for path in files):
            raise ValueError("Package publication requires regular immutable source files")
    prefix = environment.get("R2_PREFIX", "").strip("/")
    bucket = environment.get("R2_BUCKET", "")
    endpoint = environment.get("R2_ENDPOINT_URL") or environment.get("AWS_ENDPOINT_URL_S3", "")
    account = environment.get("R2_ACCOUNT_ID") or environment.get("CLOUDFLARE_ACCOUNT_ID", "")
    if not endpoint and account:
        endpoint = f"https://{account}.r2.cloudflarestorage.com"
    if not skipped and (not bucket or not endpoint):
        raise ValueError("Select R2_BUCKET and the HTTPS publication endpoint")
    entries = []
    for path in files:
        entries.append({
            "source": str(path.resolve(strict=True)),
            "key": "/".join(part for part in (prefix, path.name) if part),
            "size": path.stat().st_size,
            "sha256": checksum(path),
            "content_type": content_type(path.name),
        })
    payloads = [entry for entry in entries if not metadata_file(Path(entry["source"]).name)]
    metadata = [entry for entry in entries if metadata_file(Path(entry["source"]).name)]
    manifest = {"schema": 1, "objects": payloads}
    plan = {
        "schema": 1, "bucket": bucket, "endpoint": endpoint.rstrip("/"),
        "skipped": skipped, "dry_run": dry_run,
        "metadata": metadata, "manifest": str((output / "manifest.json").absolute()),
        "state": str((output / "state.json").absolute()),
    }
    write_json(output / "manifest.json", manifest)
    write_json(output / "metadata.json", plan)
    return {
        "R2_BUCKET": bucket,
        "R2_ENDPOINT_URL": endpoint.rstrip("/"),
        "R2_BULK_MANIFEST": plan["manifest"],
        "R2_BULK_STATE": plan["state"],
        "R2_BULK_TRUSTED_RECEIPTS": "",
        "R2_BULK_DRY_RUN": "true" if dry_run else "false",
        "R2_BULK_SKIP": "true" if skipped else "false",
        "PACKAGE_STORE_METADATA_PLAN": str((output / "metadata.json").absolute()),
    }


def publish_metadata(plan_path, binary):
    plan = json.loads(plan_path.read_text())
    if plan["skipped"] or plan["dry_run"]:
        print("Package store metadata publication skipped; no provider writes")
        return
    state = json.loads(Path(plan["state"]).read_text())
    manifest = json.loads(Path(plan["manifest"]).read_text())
    canonical_objects = []
    for item in manifest["objects"]:
        canonical_objects.append(dict(item, publish_last=False))
    identity = hashlib.sha256(json.dumps({
        "endpoint": plan["endpoint"], "bucket": plan["bucket"],
        "objects": canonical_objects, "trusted": {},
    }, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    if (state.get("schema") != 1 or state.get("phase") != "verified-head"
            or state.get("bucket") != plan["bucket"]
            or state.get("endpoint") != plan["endpoint"]
            or state.get("identity") != identity):
        raise ValueError("Exact package payload verification must precede metadata publication")
    for item in plan["metadata"]:
        path = Path(item["source"])
        if path.stat().st_size != item["size"] or checksum(path) != item["sha256"]:
            raise ValueError("Package store metadata changed after preparation")
    # Metadata is deliberately ordered and remains owned by this publisher.
    for item in plan["metadata"]:
        subprocess.run([
            str(binary), "release", "upload", item["source"],
            "--bucket", plan["bucket"], "--endpoint-url", plan["endpoint"],
            "--key", item["key"], "--content-type", item["content_type"],
        ], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("phase", choices=("prepare", "metadata"))
    parser.add_argument("--output", type=Path)
    parser.add_argument("--environment-output", type=Path)
    arguments = parser.parse_args()
    if arguments.phase == "metadata":
        publish_metadata(Path(os.environ["PACKAGE_STORE_METADATA_PLAN"]), Path(os.environ["DOCKPIPE_BIN"]))
        return
    values = prepare(os.environ, arguments.output)
    if any("\n" in value or "\r" in value for value in values.values()):
        raise ValueError("Publication environment values must be single-line")
    arguments.environment_output.parent.mkdir(parents=True, exist_ok=True)
    arguments.environment_output.write_text("".join(f"{key}={value}\n" for key, value in values.items()))
    print(json.dumps({"payload_manifest": values["R2_BULK_MANIFEST"],
                      "dry_run": values["R2_BULK_DRY_RUN"], "skip": values["R2_BULK_SKIP"]}))


if __name__ == "__main__":
    main()
