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
mode. An active package-native view additionally limits requested modes to 4096
pixels per axis and a 32 MiB 32-bit frame. For example, 4096 by 2048 fits that
budget, while 3072 by 3072 does not. Ordinary QEMU windows and the generic Guest
Tools API retain their broader driver-dependent range. The guest still verifies
its actual enumerated mode and tests it before applying it; fitting the host
renderer alone does not establish framebuffer or driver support.

These checks cover resolution requests through this VM menu and workflow. An
independent change inside the Windows Settings app or Guest Tools can still
select a driver-supported mode beyond the native renderer's limits. This action
does not install a graphics driver or enable GPU passthrough.

The workflow uses native host `run:` Python inference. It requires the new core
CLI candidate containing that general primitive; released CLI 0.6.25 does not
provide this behavior. Its immutable CLI pin must be updated before this workflow
is published. On Windows, Python 3 may be named `python3.exe` or `python.exe`;
Unix uses `python3`. An explicit `DOCKPIPE_HOST_PYTHON_BIN` must be an absolute
executable path. No Git Bash is needed. The CLI-only RunId overrides the selected
VM resource; when it is empty, `DOCKPIPE_SURFACE_RESOURCE_ID` supplies the target.
