# BVM code style (derived from Botspot's own code in `bvm`)

Written from reading the hand-written parts of `bvm` (git HEAD before the GPU additions). Counts are from that version.
Code ported from other projects (Mido, pi-apps, wor-flasher, windows-esd-to-iso) keeps its original style and is not the reference.

## 1. When something is a function and when it stays inline

- **The script is a list of helper functions at the top, then one big `case "$mode" in` where every mode is straight-line code.**
  `new-vm`, `download`, `prepare`, `firstboot`, `boot*`, `connect*`, `mount`, `expand` are NOT functions. They are long, uninterrupted,
  top-to-bottom sequences with a comment above each step, and are read like a recipe. `boot*` is ~300 lines inline.
- A piece of code becomes a function when it is **used in more than one place** (`process_exists`, `umount_retry`, `mount_qcow2`,
  `unmount_qcow2`, `remove_UCPD_from_mountpoint`, `list_cores_to_use`, `is_atomics_cpu`, `get_space_free`, `status*`),
  or when it is **reusable on its own / borrowed** (`package_installed` from pi-apps), or when it must be `export -f`'d for the yad GUI
  (`usb_passthrough_frontend`), or when it is a big self-contained job with two variants chosen by the caller (`download_windows_11arm64` vs
  `..._22631`, `install_dependencies`).
- **Never a function for a one-liner used once**, and no getter/setter pairs. State is a plain file read and written where needed:
  `echo 5 > "$vmdir/gui-steps-complete"`, `if [ 4 -gt "$(cat "$vmdir/gui-steps-complete")" ];then echo 4 > ...`. No temp-file-and-rename,
  no validation layer.
- Functions are small and do one thing; the exception is a long linear job (`download_*`, `install_dependencies`) which is kept as one
  long function rather than split into sub-functions. Functions do not call other helper functions just to save two lines.
- Function names: `snake_case`, verb or noun first, describing the job (`mount_qcow2`, `remove_microsoft_defender`, `qemu_newer_than`,
  `list_cores_to_use`). No prefixes. (A prefix is only worth it for a group of functions that would otherwise clash.)
- Helper functions that the modes share live together above the mode dispatch. Ported functions keep their source comment ("#taken from pi-apps").

## 2. Error handling and flow

- `error "message"` prints red and does `exit 1`; it is called **directly inside functions** as well (`error "connect_qcow2_to_nbd: Partition 4 not found"`).
  Function errors start with the function name and a colon; errors in mode code are plain sentences for the user.
  Callers that must survive an error call it in a subshell: `(error "...")` or `(mount_qcow2 ...)`.
- Idioms: `command || error "what failed"` (23x), `[ -z "$x" ] && error "function: no x specified!"` as a guard at the top (11x),
  `if ! command ;then ... error ... fi`, `command || exit 1` after calling a function that may have failed.
- Returns: `return 0/1` as a yes/no answer for predicate-like functions (`process_exists`, `is_atomics_cpu`, `qemu_newer_than`, `list_cores_to_use`).
  Callers use them as `if is_atomics_cpu ;then`.
- No `set -e`, no `set -u`, no `trap ... ERR`. `trap "..." EXIT SIGINT` only for cleanup of something just created.
- Pre-flight/compatibility checks are one `if / elif / elif` chain where each branch is `error "User error: ..."` (the first problem stops the script).
- Messages: `status "Doing X..."` (cyan, ends the line) for steps that print their own output or take long. `status -n "Doing X... "` prints **no newline**
  and its text **ends with a space**, so the following `status_green Done` lands on the same line: `Doing X... Done`. Use that pair only for a step that
  is not expected to print anything unless it fails (an error message then appears on the next line), e.g. `status -n "Making unattended.iso... "`.
  `status_green "..."` for success, `warning "..."` for yellow. Messages are full sentences that say what to do next ("Please run ... ", "Run $0 help to see ...").
  Next-step hints: `status_green "You should now be ready for the next step: $0 prepare $vmdir"`.
- `debug "..."` for developer output (only prints with `bvm_debug=true`).

## 3. Variables

- Script state and locals: `lowercase_snake_case` (`full_qemu_flags`, `use_taskset`, `qemu_exit_code`, `nbd_device`, `balloon_size`, `retrying`).
  Inside functions: `local` (62x). In mode code: plain globals, no `local` (there is no function scope).
- UPPERCASE is for the few global constants (`DIRECTORY`) and for ported code (`SOURCE_FILE`, `URL`, `WIN_LANG`, `TMP`).
- Settings the user can change are lowercase and live in `bvm-config` (`vm_mem`, `rdp_port`, `disksize`, `debloat`, `fullscreen`, `usb_passthrough`).
  Defaults are set with `[ -z "$x" ] && x=default` right after the config file is sourced.
- Booleans are the strings `true`/`false`, tested `[ "$fullscreen" == true ]`. Not 0/1 (except throw-away flags like `installneeded=1`, `retrying=0`).
- Lists of command-line flags are arrays named `*_flags` (`display_flags`, `audio_flags`, `usb_forwarding_flags`, `full_qemu_flags`),
  built with `(...)` and `+=(...)`, expanded `"${x[@]}"`.
- Function input is `$1`, `$2` or a global (`$vmdir`, `$rdp_port`, `$debloat`). Functions read the globals directly instead of taking them as
  parameters (`vmdir` in the download functions, `rdp_port` in `mount_qcow2`), and say so in the comment or with a guard
  (`[ -z "$vmdir" ] && error "...: vmdir must be set"`). `DIRECTORY`/`vmdir` are the "set it yourself when sourcing" convention.

## 4. Syntax

- 2-space indent, no tabs. **Blank lines inside a block keep the indentation** (`  ` on the empty line; 176 such lines vs 87 truly empty).
- `if cond ;then`, `while cond;do`, `for x in $list ;do`: **`;then` / `;do` glued, no space before `;` after a closing `]`** (135x vs 18x `; then`).
  After a command (not a `]`) a space before `;then` is common: `if sudo ntfsfix --clear-dirty "$dev" ;then`.
- Single brackets only: `[ "$a" == "$b" ]`, `[ ! -z "$x" ]` (12x; never `-n`), `[ -z "$x" ]`, `[ ! -f ... ]`. `[[ ]]` only 5x, in ported code.
  `==` is used inside `[ ]`. Chains with `&&` / `||` between separate `[ ]` tests.
- `$(...)`, never backticks. Arithmetic `$((a+b))`. Quote variables that can contain spaces; `"$vmdir/disk.qcow2"`.
- Pipelines of `grep | awk | sed | head | tr` are fine and everywhere (`cat file | grep` even); no attempt to avoid external processes.
- Output: `echo -e "\e[91m...\e[0m"` for colours (15x `echo -e`, one `printf`), `echo "text" 1>&2` for stderr, `read -p "question [Y/n] " answer`.
- Line continuation `\` with the continued lines indented by two more spaces; long QEMU flag lists have 1-2 related flags per line.
- `case` is used for small dispatches (`case "$mode" in`, `case $? in`) with `;;` on its own line, patterns indented one level.
- `sudo` is called freely at the point of use. Files are written with `echo ... > file`, `echo -e "..." | sudo tee ...`, or a multi-line quoted string.
- `mktemp -d` for temporary directories, removed with `rmdir`/`rm -rf` at the end of the same block.
- Anything the GUI click handler must call: `export -f name` directly under the function.
- IFS: the script sets `IFS=$'\n'` globally (after the helper functions are defined). Code that splits on spaces sets `IFS=' '` locally right before the loop.

## 5. Comments

- Style is **lowercase, no space after the `#`, no full stop**: `#find /dev/nbd* device not in use` (287 comments like this vs 37 with `# Capital`, mostly ported code).
- **Function description goes on the same line after the opening brace**: `name() { #what it does. Input: ... Output: ...`. Typical phrases:
  "#Input: folder to check. Output: show many bytes...", "#return 0 if the $1 PID is running, otherwise 1", "#exit 0 if $1 package is installed, otherwise exit 1".
  Details follow on the next line(s) as `#...` inside the body.
- One comment per step of a sequence, directly above the step, describing the intent ("#try to fix the filesystem so it can be mounted R/W even after unsafe shutdown").
  No banners, no ASCII boxes, no section dividers made of dashes.
- Trailing comments after a command on the same line are common: `sync #wait for any writes to finish`, `rm -f ... #...`.
- The *why* and the history are written down, with links: `#fixes https://github.com/Botspot/bvm/issues/26`, "lesson learned: ...", "#workaround for users who ...",
  example values of variables in the comment (`#example value of dir: /tmp/tmp.coL1nQguHD/NetKVM/w11/ARM64`).
- `#for GUI: track which step was completed` style tags to explain why a line exists. Notes for future debugging go in comments, long diagnostics go at the end of the file.
- Comments are short: one line per idea, rarely more than 2-3 lines in a row.

## 6. Structure of the files

- Top of the file: helper functions (`error`, `warning`, `status`, ...), the GUI/mode helpers, then arguments, config defaults, system checks, then `case "$mode"`.
- Config defaults for new settings go next to the other `[ -z "$x" ] && x=...` lines; documentation of the setting goes into `resources/bvm-config` as a commented example.
- Help text for a mode goes into the `help)` branch in the same voice ("Main command to use the VM.").

## 7. Habits to avoid when adding code (what was wrong with the first GPU foundation code)

- Many tiny single-purpose functions (getter/setter, `gpu_step_clear`), and a variable for a path that is already short (write `"$vmdir/gpu"`, not `gpu_dir`): inline them.
- Optional vmdir parameters on every function: use the global `$vmdir` like the other functions do.
- Defensive layers (atomic write via temp file, state validation, `flock`, nested helper functions that are defined and `unset` inside another function).
- `/* ---- banner ---- */` comment blocks and long multi-paragraph comments.
- Per-check "ok" printing and an error counter: the existing checks stop at the first problem with `error`.
- Naming helpers with a prefix and inventing parameters (`-y`, optional second arguments) that nothing needs yet.
