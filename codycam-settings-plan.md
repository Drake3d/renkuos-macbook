# Plan: CodyCam's Video settings = the Camera preferences

**Shelved 2026-11-25:** the user decided to leave CodyCam alone. Its Video
settings keep the generic window with the same controls and saving. Kept
here only in case it comes back up.

Status (2026-11-25): CodyCam's settings now have the same controls as Camera
prefs and save them (tested, works). Not yet the same look, and the window
opens too small and is awkward to resize. Nothing from camera item 4 onward
is committed yet (sizes, mirror, Picture size setting, shared settings
writer, CodyCam parameter work), all on branch `apfs` in /Volumes/Source/Source.

## Why it looks different now

CodyCam builds its window from the camera's parameter list with Haiku's
generic media theme (`BMediaTheme::ViewFor`, CodyCam.cpp ~line 189). The
window takes the theme view's starting size (`controls->Bounds()`,
ControlWindow constructor ~line 885) and nothing works out the size the
controls need. That's why it opens small. A generic theme can never look like
Camera prefs.

## Approach: the camera opens Camera prefs as its own settings window

The media kit already has a hook for this: a camera's add-on can supply its
own settings window (`BControllable::StartControlPanel`), and an app asks for
it with `BMediaRoster::StartControlPanel(node)`.

1. **Media add-on** (src/add-ons/media/media-add-ons/facetimehd/Producer.cpp):
   override `StartControlPanel` to launch Camera prefs by its signature
   `application/x-vnd.RenkuOS-Camera` (bring it to the front if it is already
   running), return its messenger, and return B_OK.
   (The kit's default version has no `return` at its end, a real bug, so
   don't depend on it.)
2. **CodyCam** (src/apps/codycam/CodyCam.cpp, `msg_video`): first try
   `fMediaRoster->StartControlPanel(fProducerNode, &messenger)`. If that works,
   don't open the theme window. Otherwise (USB webcams and other cameras),
   fall back to the theme window as now.
3. **Theme window fallback, for other cameras:** open it at the size its
   controls need (the view's preferred size or `GetPreferredSize`), and not
   resizable (`B_NOT_RESIZABLE | B_NOT_ZOOMABLE`). Do the same again when the
   window is rebuilt on `B_MEDIA_WEB_CHANGED`.
4. **Camera prefs window** (src/preferences/camera/CameraWindow.cpp:144):
   still resizable sideways (`B_NOT_V_RESIZABLE` only). Make it one fixed
   size that fits every control: `B_NOT_RESIZABLE`, with the layout working
   out the size (`B_AUTO_UPDATE_SIZE_LIMITS` stays).
5. **Live updates:** with prefs open next to CodyCam, check that slider and
   Mirror changes show in the CodyCam picture straight away. The driver
   allows several opens and applies controls to the running stream, so this
   is expected to work. Only check.
6. Keep the parameter list in the add-on (just done). Other apps that use the
   generic theme, such as the Media preferences, still get the same controls
   and saving.

Result: the same window, the same controls and the same file, whether you
open it from Deskbar › Preferences › Camera or from CodyCam › File › Video
settings. Nothing to keep in sync by hand.

### Alternative considered (not recommended)
Copy the Camera prefs controls into CodyCam as a FaceTime-only view. That
means two copies of the same UI to keep matching, and FaceTime-specific
code in a general app. Opening Camera prefs reuses the one window instead.

## Test on the MacBook
- CodyCam › File › Video settings opens Camera prefs (fixed size, every
  control visible, no resizing needed). Opening it twice brings the same
  window forward, not a second one.
- Changes show live in CodyCam, and come back after a restart.
- Deskbar › Preferences › Camera still works on its own.
- (If a USB webcam is around) its Video settings still open, sized to fit.

## Build and copy
    cd /Volumes/Source/Source/generated.x86_64
    jam -q -j8 Camera facetimehd.media_addon CodyCam facetimehd.hpkg
CodyCam is in the system package, not facetimehd.hpkg. Either copy the
CodyCam binary to the USB drive on its own (put it in
~/config/non-packaged/apps) or build an image. Check which is quicker
before starting.

## Then commit (after the user tests)
Commit item 4 (sizes, mirror, Picture size) and the CodyCam settings work as
separate commits, `git commit -s` with the Assisted-by and Co-Authored-By
lines. Then update HANDOFF notes and, when asked, push to `personal`.
