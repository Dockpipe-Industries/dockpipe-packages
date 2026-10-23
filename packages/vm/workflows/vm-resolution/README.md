# VM display resolution

From the VM menu, choose **Display resolution** and enter the desired width and
height. The action uses the selected VM automatically. The CLI uses the same
workflow and environment contract:

```sh
dockpipe --package vm --workflow vm-resolution \
  --env DOCKPIPE_VM_TARGET_RUN_ID=your-run-id \
  --env DOCKPIPE_VM_DISPLAY_WIDTH=1920 \
  --env DOCKPIPE_VM_DISPLAY_HEIGHT=1080
```

Install the QEMU resolver so its management tool is available. Run IDs come from
`vm-manage` snapshots. The VM process must have a live verified identity and a
key-authenticated Windows connection. The logged-in Guest Tools application
applies the mode in the interactive session and saves it for subsequent logins.
The application also lists the actual driver-supported modes beside its
**Apply resolution** button. Unsupported dimensions fail without changing the
mode. This does not install a graphics driver or enable GPU passthrough.

The workflow uses native host `run:` Python inference. It requires the new core
CLI candidate containing that general primitive; released CLI 0.6.25 does not
provide this behavior. Its immutable CLI pin must be updated before this workflow
is published. On Windows, Python 3 may be named `python3.exe` or `python.exe`;
Unix uses `python3`. An explicit `DOCKPIPE_HOST_PYTHON_BIN` must be an absolute
executable path. No Git Bash is needed. The CLI-only RunId overrides the selected
VM resource; when it is empty, `DOCKPIPE_SURFACE_RESOURCE_ID` supplies the target.
