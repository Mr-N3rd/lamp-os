<script setup lang="ts">
// Vector lamp glyph. Shade trapezoid on top filled with the lamp's shade
// color, rectangular base below filled with the base color, thin neck
// joining them. Drawn in a 24×30 viewBox so it always looks like a
// little lamp regardless of the size prop the caller picks.

interface Props {
  shade: number[]   // RGBW or RGB; only the first three channels rendered
  base:  number[]
  size?: number     // rendered width in px; height auto-scales 1.25x
}
const props = withDefaults(defineProps<Props>(), { size: 28 })

const rgb = (c: number[]) => `rgb(${c[0] ?? 0}, ${c[1] ?? 0}, ${c[2] ?? 0})`
</script>

<template>
  <svg
    :width="props.size"
    :height="props.size * 1.25"
    viewBox="0 0 24 30"
    aria-hidden="true"
    class="lamp-icon"
  >
    <!-- shade -->
    <polygon
      :fill="rgb(props.shade)"
      points="6,3 18,3 21,13 3,13"
      stroke="rgba(255,255,255,0.4)"
      stroke-width="0.6"
      stroke-linejoin="round"
    />
    <!-- neck -->
    <rect x="11" y="13" width="2" height="9" fill="rgba(190,190,190,0.45)" />
    <!-- base -->
    <rect
      :fill="rgb(props.base)"
      x="4" y="22" width="16" height="6"
      rx="1.5"
      stroke="rgba(255,255,255,0.4)"
      stroke-width="0.6"
    />
  </svg>
</template>

<style scoped>
.lamp-icon {
  display: block;
  flex-shrink: 0;
}
</style>
