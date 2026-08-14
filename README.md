# QuickYeet

QuickYeet is a Windows Explorer context-menu app for moving a file to a familiar destination with as little friction as possible.

Right-click a file and open **QuickYeet**. On Windows 11, QuickYeet appears in the first-layer context menu and expands into a native flyout containing recent and pinned destinations. The flyout remembers frequently used destinations and keeps the most useful ones close at hand.

If a file with the same name already exists, QuickYeet adds a numbered suffix so both files are preserved.

## Using QuickYeet

1. Right-click a file in Windows Explorer.
2. Expand **QuickYeet** from the Windows 11 first-layer menu.
3. Pick a frequent destination, send the file to Recycle Bin, browse for another folder, or open **Manage Destinations** for recursive navigation and destination management.
4. QuickYeet moves the file and records folder destinations for future suggestions.

## Configuration

QuickYeet keeps its destination settings in a local YAML config file. Each destination records its folder location, display name, and pin state.

Recycle Bin appears automatically among the yeet options. It can be turned off in the YAML config file.

Choose **Manage Destinations**, then **Open Config Folder**, to open the directory containing the config file in Windows Explorer.

## Agent build and install

The repository includes the `quickyeet-build-and-install` skill. It shows an agent how to compile QuickYeet, personally sign that build, and install it.

## Examples

### Windows 11 native flyout

The first-layer command expands into a native Windows 11 flyout. Choosing a listed destination moves the selected files immediately. **Manage Destinations** opens QuickYeet's popup for recursive navigation, aliases, and pin management.

```text
Windows 11 context menu     QuickYeet native flyout
+----------------------+    +--------------------------+
| Open                 |    | RECENT                   |
| Share                |    | Downloads                |
| QuickYeet          > |--->| Client work              |
| Show more options    |    |--------------------------|
+----------------------+    | PINNED                   |
                            | Archive                  |
                            |--------------------------|
                            | Recycle Bin              |
                            | Browse...                |
                            | Manage Destinations...   |
                            +--------------------------+
                                         |
                                         | Manage Destinations
                                         v
                            +--------------------------+
                            | QUICKYEET                |
                            | Client work            > |
                            | Downloads                |
                            | Archive                  |
                            | Browse...     New Folder |
                            | Open Config Folder       |
                            +--------------------------+
```

### Name collision

QuickYeet preserves the existing file and gives the moved file a predictable suffix.

```text
D:\Inbox\notes.txt
        |
        | QuickYeet -> D:\Archive
        v
D:\Archive\
|-- notes.txt         existing file
`-- notes (2).txt     moved file
```

## Features

- QuickYeet appears in Windows 11's first-layer context menu as a native flyout.
- Moves multiple selected files at once.
- Recent and pinned destinations can be chosen directly from the Windows 11 flyout.
- **Manage Destinations** opens the popup for recursive folder navigation, aliases, pinning, and removal. Click **Yeet** for a quick move.
- Recent and frequent destinations appear in a Recents section.
- Destinations can be pinned or removed from the list.
- Destinations can be given an alias.
- Destination entries can expand to show their subdirectories.
- Every destination and expanded subdirectory includes a **New Folder** action.
- Any other folder can be selected without leaving the workflow.
- Recycle Bin is always available as a yeet option unless it is turned off in the YAML config file.
- File moves never overwrite an existing file. Name collisions receive a numbered suffix.
- A local YAML config file stores destination locations, display names, and pin states.
- **Open Config Folder** opens the config file's directory from the destination manager.
- Destination history and configuration stay on the local machine.
- The included `quickyeet-build-and-install` skill guides agents through compiling, personally signing, and installing their own QuickYeet build.
- QuickYeet ships as an installer package for easy deployment.
- MSI releases upgrade earlier per-machine releases in place. Upgrades leave Explorer running and may request a reboot only to finish deleting a legacy shell DLL.

## Changelog

- Added in-place MSI upgrades.
- Isolated Explorer contract tests from local configuration and hardened source-control defaults for line endings and local signing passwords.
- Resolved an issue that delayed every system right-click context-menu item in Windows Explorer.

## License

QuickYeet is licensed under the [MIT License](LICENSE.md).
