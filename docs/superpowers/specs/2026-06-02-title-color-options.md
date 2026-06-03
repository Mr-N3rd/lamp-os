# Title color options

## Context

Today the app uses `BrandColors.lampWhite` (#FDFDFD) for both titles and body text. Font-weight differences (w600 on titles, regular on body) carry the whole typographic hierarchy. The user wants titles to stand out visually — and if there's a meaningful split between several typographic levels (AppBar title, section header, body, caption, muted), each gets its own color.

This doc lists 4 options the user can pick from. No code change in this commit — options only.

## Current palette (relevant subset)

From `lib/core/theme/brand_colors.dart`:

| Token | Hex | Currently used for |
|---|---|---|
| `lampWhite` | `#FDFDFD` | All titles + body text |
| `fogGrey` | `#CCCCCC` | Some captions ("Sparking inspiration…") |
| `slateGrey` | `#888888` | Hints, disabled-state text |
| `ashGrey` | `#555555` | Inactive slider track |
| `headerYellow` | `#FFFA77` | "Nearby lamps" / "Seen recently" column headers, input-field labels |
| `warmWhite` | `#FABB3E` | Warm-white slider thumb |
| `amberGold` | `#E1A44A` | Warm metallic accents (unused in core text) |
| `auroraBlue` | `#446C9C` | Primary interactive (button bg, link) |
| `glowPink` | `#EFA3C8` | Secondary, active slider track |

Brand context: warm/vintage art-community vibe (Lamplit Art Society). Logo uses `headerYellow` with wide letter-spacing. Existing in-use accent on titles is `headerYellow`.

## Five typographic levels we'd map

1. **AppBar title** — top-of-screen title (e.g. "Setup", "Expressions").
2. **Section title** — within-screen group headers (e.g. "Connectivity", "LEDs", expression param labels).
3. **Row title** — primary text in a list item (e.g. "Home Mode", "Password", a nearby lamp's name).
4. **Body text** — secondary text, descriptions, subtitles.
5. **Caption / muted** — timestamps, hints, "0/5 lamps", "tap to set", etc.

## Options

### Option A — Warm hierarchy (nostalgic, matches existing logo accent)

| Level | Color | Hex |
|---|---|---|
| AppBar title | `headerYellow` | `#FFFA77` |
| Section title | `warmWhite` | `#FABB3E` |
| Row title | `lampWhite` | `#FDFDFD` |
| Body text | `fogGrey` | `#CCCCCC` |
| Caption / muted | `slateGrey` | `#888888` |

**Feel:** doubles down on the warm Lamplit brand. AppBar and section titles use the same yellow/gold family already used by the Info wordmark and form labels, so it feels intentional and unified.

**Risk:** lots of yellow-ish on screen. Can feel "vintage-saturated" if overdone.

---

### Option B — Cool/warm tension (modern, playful)

| Level | Color | Hex |
|---|---|---|
| AppBar title | `auroraBlue` | `#446C9C` |
| Section title | `glowPink` | `#EFA3C8` |
| Row title | `lampWhite` | `#FDFDFD` |
| Body text | `fogGrey` | `#CCCCCC` |
| Caption / muted | `slateGrey` | `#888888` |

**Feel:** uses the existing secondary palette (the blue/pink that already appear on buttons + sliders). Creates strong cool/warm contrast between AppBar and section titles. Body text stays white for max legibility.

**Risk:** less aligned with the warm Lamplit brand identity. Reads more "tech product" than "artist collective."

---

### Option C — Conservative cool (minimal departure)

| Level | Color | Hex |
|---|---|---|
| AppBar title | `auroraBlue` | `#446C9C` |
| Section title | `auroraBlueHover` | `#5A7BA8` |
| Row title | `lampWhite` | `#FDFDFD` |
| Body text | `fogGrey` | `#CCCCCC` |
| Caption / muted | `slateGrey` | `#888888` |

**Feel:** stays in the blue family. AppBar and section titles are two shades of the same blue, body+caption stay grey. Quietest of the four options.

**Risk:** undersells the "make titles pop" intent — section vs row distinction is subtle. Best for dense screens where you don't want loud color.

---

### Option D — Maximum hierarchy (loud, content-heavy)

| Level | Color | Hex |
|---|---|---|
| AppBar title | `headerYellow` | `#FFFA77` |
| Section title | `glowPink` | `#EFA3C8` |
| Row title | `warmWhite` | `#FABB3E` |
| Body text | `lampWhite` | `#FDFDFD` |
| Caption / muted | `slateGrey` | `#888888` |

**Feel:** every level gets its own color. Yellow AppBar + pink section + warm-gold row. Body stays white for legibility.

**Risk:** lots of colors competing. May feel busy on screens with deeply nested hierarchy (e.g. expression editor). Best for screens that genuinely need 4+ distinct levels and don't have too much content per level.

---

## Recommendation

Option A or B. Both have a clear two-tier title accent. A leans into the existing brand; B leans modern.

C is the safe "barely changes anything" option. D is the loud "everything's distinct" option — probably overkill for most screens.

## Once you pick

I'll wire the chosen palette into `app_theme.dart`'s `TextTheme` so AppBar + body inherit automatically, and adjust the call sites that hard-code colors today (the `_SectionLabel` in `expression_params_panel.dart`, `settings_row.dart`, nearby-lamp list items in `info_screen.dart`). Estimated ~30 minutes after you pick.
