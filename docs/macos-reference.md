# macOS reference values

The `macos` and `macos-light` themes (kept in `~/.config/glassosd/themes/`, not in
this repo) are matched to Apple's own design kits. Kit values come first; videos
and screenshots are only a fallback. This file records where the numbers come
from, so the next update starts from the source rather than from guesses.

## Where Apple's kits are

| macOS | Format | How to read it |
|---|---|---|
| 27 "Golden Gate" (current) | Figma Community file only: <https://www.figma.com/community/file/1651309434229735362/macos-27> | Duplicate it into a Figma account, then use the Figma MCP (`use_figma`/`get_metadata`) on the copy |
| 26 Tahoe | Figma / sketch.com share only | Same as 27 |
| 15 Sequoia | `devimages-cdn.apple.com/design/resources/download/macOS-Sequoia-Design-Templates-Sketch.dmg` | Open with 7-Zip, `bsdtar` the `.sketch`, read `document.json` → `foreignSymbols` `System/Notification/Default` |

Since macOS 26 there is no DMG; guessed `devimages-cdn` names return 403.
Community files cannot be read through the API until they are duplicated.

## macOS 27 kit: where to look

These node IDs are kept when the file is duplicated:

- Page **Notifications**
- Component set `4358:11429` **Notification**, variants:
  - `130:10293` Mode=Dark, Stack=False
  - `4358:11430` Mode=Light, Stack=False
  - `4358:11608` Mode=Dark, Stack=True
  - `4358:11618` Mode=Light, Stack=True
- Materials:
  - `4358:11539` **Liquid Glass - Notification - Dark**
  - `4358:11549` **Liquid Glass - Notification - Light**
- Other useful pages: **Materials**, **Colors**, **Text Styles**, **Examples** (`Examples/Notifications`)

`get_design_context` flattens blend modes. To see every fill, effect and blend
mode, walk the nodes with `use_figma` (read-only).

## macOS 27 banner values (read 2026-11-20)

**Geometry**
- Card 344×77, corner radius 20.
- Padding 12 top / 14 right / 12 bottom / 10 left.
- App icon 32 inside a 40-wide box (4 each side), 8 gap before the text.
- Right column 32 wide: timestamp, then an optional 32×32 image with radius 6.
- Stack: sheets are 312×68 (16 in from each side) and show 8 below the card. Sheets have no shadow.

**Type** (SF Pro)
- Title: Bold 13/16, letter-spacing −2%.
- Body: Regular 13/16, letter-spacing −0.8%.
- Timestamp: Regular 11/14.

**Dark**
- Material:
  - GLASS effect: frost 25, refraction 0.7, depth 30, light 0.25 at angle 0, dispersion 0.2.
  - Fill `rgba(51,51,51,0.30)` LUMINOSITY, then `rgba(51,51,51,0.10)` NORMAL.
  - Inner shadows: `#1A1A1A` LINEAR_DODGE, radius 5, offset 0,±40, spread −40. These light the top and bottom rims.
- Shadows:
  - `0 8 48` at 0.45.
  - Outline: `#A6A6A6` LINEAR_BURN, spread 0.5, plus two side copies at ±1.25 x, spread −0.75.
- Text:
  - Title and body `#F5F5F5` LINEAR_DODGE.
  - Timestamp `#404040` LINEAR_DODGE.

**Light**
- Material:
  - Same GLASS effect.
  - Fill `rgba(191,191,191,0.25)` LIGHTEN, then `#1A1A1A` LINEAR_DODGE.
  - Inner shadows `#282828` LINEAR_DODGE.
- Shadows:
  - `0 8 48` at 0.25.
  - Outline `#DBDBDB` LINEAR_BURN.
- Text:
  - Title and body `#1A1A1A` LINEAR_BURN.
  - Timestamp `#BFBFBF` LINEAR_BURN.

## Screenshots are not kit values

Check a screenshot's date before using it. A July 2025 macOS dark banner
screenshot is macOS 26 Tahoe, not 27. Its brighter all-round rim (white ~0.16)
and timestamp (white ~0.75) were not adopted, because the 27 kit lights only the
top and bottom rims and sets the timestamp at +25%.

## How the themes translate this

- **Luminosity and lighten layers.** KWin has no blend modes for a client
  surface. These layers become `blur.materialLuma` instead: kwin-glassblur remaps
  backdrop luminance into that range. Only the NORMAL tint becomes
  `card.background`.
  - Dark: the kit implies 0.7·L + 0.06, a range of [6, 76]. The theme
    uses **[9, 50]**. Over a dark app (#191D20) both give a fill of about
    #22, which a real macOS dark banner also shows. At 76, white text over a white window
    went grey-on-grey. Apple's glass flips darker over bright content and its
    text is additive (LINEAR_DODGE); neither can be done here.
  - Light: about [29, 100], plus `card.background` white at 0.08.
- **Outline.** The LINEAR_BURN ring becomes `glass.outline`: black at
  1 − grey/255, which is 0.35 for dark and 0.14 for light. The dodge rims become
  `glass.edge`.
- **Refraction and dispersion.** The lensing and colour fringing at the rim have
  no KWin equivalent and are left out.
- **Motion.** The kit has no animation data. The theme's curves come from a 60 fps
  recording: 400 ms in, `0.35,0.85,0.35,1`; 250 ms out, `0.15,0.05,0.30,1`.
- **Font.** It stays Inter, because SF Pro is not licensed for use off Apple
  platforms. The kit's tracking is kept: `font.notifyTitleTracking` −0.02 and
  `font.notifyBodyTracking` −0.008 (em). Inter is wider than SF Pro, so
  without them lines wrap where macOS's would not.
- **App icons.** A macOS banner always shows the sending app's icon. Linux
  apps often send a monochrome theme glyph instead (`mail-message`, a calendar
  action icon). With `notify.appIconFirst` on, a glyph is replaced by the
  desktop entry's icon when that icon is colour artwork. The card then drops
  the badge and the round avatar mask (`IconInfo.prefersApp`). An app with no
  desktop entry, or with a glyph for its own icon, keeps the tinted glyph.
- **Clear all.** macOS draws nothing under its banners: a stack is cleared
  with its own close button, and clear-all lives in Notification Centre. The
  macOS themes set `notify.clearAll` to false.

The centre cards (`entry.*`) and the OSD use the same material as the banner,
as they do on macOS. The centre's backing sheet (`panel.background`) is only a
light tint, because on macOS the cards float over the desktop with no sheet
behind them.
