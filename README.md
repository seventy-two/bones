# shelf

A plain file browser for Windows: drives, pinned folders and your WSL home
down the side, and a folder as thumbnails or as details. None of
Explorer's ribbon, Home page, gallery, OneDrive or network clutter.

- **This PC** is where it opens (unless given a folder): a list of the
  drives with their free space and the default WSL distro's home, each
  with its three most used folders under it. A visit counts once you've
  stayed 2 seconds (so folders you click through don't), and old visits
  fade by half every two weeks. Right-click a folder there to hide it
  (it's no longer counted); a drive's right-click menu shows hidden ones
  again. It's at
  the top of the sidebar, the first step of the path bar, and Up from a
  drive.
- **Sidebar**: pinned folders (+ pins where you are, drag to reorder,
  right-click to move or unpin), every drive with its free space, and
  the default WSL distro's home (`/root`; other distros are left out, and
  `/` is one step up the path bar). It's read from the registry, so a
  stopped distro isn't started until you click it.
- **Thumbnails or details** (Ctrl+1 / Ctrl+2), chosen per folder: a
  folder that's mostly pictures and videos (at least 3 and half its files,
  or 20 or more) opens as thumbnails, anything else as details. A view
  picked by hand in a folder sticks for that folder (the last 500 are
  kept). Right-click › View › "Choose by what's in the folder" turns the
  automatic part off. "Folders first" (sort menu,
  or right-click empty space > Sort by) can be turned off to sort folders
  in with the files. Details sorts by name,
  date modified, date created, type or size from the headers; the
  thumbnail view has the same sort in a menu, and a size slider
  (or Ctrl+scroll).
- **Thumbnails, faster than Explorer's**:
  - only what's on screen (and the next screen down) is asked for, newest
    first, so a fast scroll doesn't queue up everything it went past;
  - images are decoded by WIC straight at a small size: JPEGs are scaled
    inside the decoder (1/2, 1/4, 1/8), so a 24 MP photo never decodes in
    full; camera raws use their embedded preview; EXIF rotation is applied;
  - videos, PDFs and the like go to the shell's own thumbnailers, on
    threads of their own so they never hold up the images;
  - everything goes in shelf's own disk cache (`%LOCALAPPDATA%\shelf\thumbs`,
    keyed by path, size and time, kept under 1 GB), which also covers WSL
    and network folders that Windows doesn't cache;
  - the worker threads run at below-normal priority.

  On a test folder of 120 4000×3000 JPEGs, 8 PNGs and 3 videos: 0.6 s
  for all of them the first time, 0.27 s from the cache, against 1.8 s
  through the shell's thumbnail route (what Explorer uses) with five
  threads.
- Files: open (Enter, double-click), Open with, cut/copy/paste (Explorer's
  clipboard, so it works both ways, with the shell's progress and clash
  dialogs), rename in place (F2), recycle (Del; Shift+Del deletes), new
  folder (Ctrl+Shift+N), copy path (Ctrl+Shift+C; Linux paths too),
  properties (Alt+Enter), drag files out to other apps and drop files in,
  and "More options…" for Explorer's own right-click menu.
- Moving about: Alt+Left/Right and the mouse's side buttons, Backspace for
  up, click the path bar (or Ctrl+L) to type a path; a `/linux/path` opens
  in the default distro. Ctrl+F filters the folder (`*.jpg` works). Typing
  a name's start jumps to it.
- Folders show what's in them: up to three of their pictures (or
  videos) fanned out in the folder, pictures first, by name. They're drawn
  in the app's slate colours rather than Windows' yellow.
- The folder refreshes itself when its contents change.

## Default for folders

`scripts\default-folders.ps1` makes shelf the way folders and drives open for
your user (double-clicking a folder, "open folder" in other apps,
`start C:\x`); `-Undo` puts Explorer back. It only writes under
`HKCU\Software\Classes`: an "Open in shelf" verb on `Directory` and `Drive`,
named as the default there and on `Folder`. Virtual folders (This PC, Recycle
Bin, Control Panel) have no such verb, so they still open in Explorer, and
Explorer's own Open stays in the right-click menu.

`scripts\shelf-wine.ahk` (AutoHotkey v1, in the Startup folder) maps Win+E to
shelf and Win+Shift+E to Explorer.

Windows still uses Explorer for "Show in folder" (apps call
`SHOpenFolderAndSelectItems`), the Open/Save dialogs, the desktop and the
taskbar.

## Building

`build.bat` (Visual Studio 2022 C++ tools) builds `build\shelf.exe` and
runs the tests. The tests also build anywhere:

    g++ -std=c++17 -Isrc tests/tests.cpp src/paths.cpp -o tests && ./tests

`shelf.exe <folder>` opens a folder. `shelf.exe -bench <folder> <report>`
times thumbnails for a folder with no window.

ImGui's internal checks write to `%TEMP%\shelf-assert.log` rather than
stopping the app.
