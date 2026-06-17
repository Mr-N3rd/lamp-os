interface KnockoutPixel {
  p: number
  b: number
}

interface LampSettings {
  name?: string
  brightness?: number
  homeMode?: boolean
  homeModeSSID?: string
  homeModeBrightness?: number
  password?: string
}

interface ShadeSettings {
  px?: number
  colors?: string[]
}

interface BaseSettings {
  px?: number
  colors?: string[]
  ac?: number
  knockout?: KnockoutPixel[]
}

/**
 * Social mode settings for lamp-to-lamp interaction.
 *
 * Currently implemented:
 *   - enabled: master switch for social greeting reactions
 *   - friendsOnly: when true, only react to lamps listed in `friends`
 *   - friends: list of friend lamp names/IDs
 *   - cooldownMs: minimum milliseconds between reactions
 *
 * Future-facing: community presets, group channels, event-mode overrides, etc.
 */
export interface SocialSettings {
  enabled?: boolean
  friendsOnly?: boolean
  friends?: string[]
  cooldownMs?: number
}

export interface Settings {
  lamp?: LampSettings
  shade?: ShadeSettings
  base?: BaseSettings
  social?: SocialSettings
}
