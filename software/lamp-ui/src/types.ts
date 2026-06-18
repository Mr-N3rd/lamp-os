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

export type SocialMode = 'butterfly' | 'shy' | 'greet' | 'off'

/**
 * Social mode settings for lamp-to-lamp interaction.
 *
 * Currently implemented:
 *   - mode: social behavior mode
 *   - friends: list of friend lamp names/IDs
 *   - cooldownMs: minimum milliseconds between reactions
 *
 * Legacy fields are optional so older saved settings can still be normalized.
 */
export interface SocialSettings {
  mode?: SocialMode
  friends?: string[]
  cooldownMs?: number
  enabled?: boolean
  friendsOnly?: boolean
}

export interface Settings {
  lamp?: LampSettings
  shade?: ShadeSettings
  base?: BaseSettings
  social?: SocialSettings
}
