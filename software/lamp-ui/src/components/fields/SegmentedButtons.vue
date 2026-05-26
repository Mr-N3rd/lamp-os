<script setup lang="ts">
import type { FieldValidationResult } from '@/types'

interface SegmentedOption {
  value: number | string
  label: string
}

interface Props {
  modelValue: number | string
  options: SegmentedOption[]
  disabled?: boolean
  required?: boolean
}

const props = withDefaults(defineProps<Props>(), {
  disabled: false,
  required: false,
})

const emit = defineEmits<{
  'update:modelValue': [value: number | string]
}>()

function select(value: number | string) {
  if (props.disabled) return
  emit('update:modelValue', value)
}

const validate = (): FieldValidationResult => ({ valid: true })
defineExpose({ validate })
</script>

<template>
  <div class="segmented-buttons" role="radiogroup">
    <button
      v-for="opt in options"
      :key="String(opt.value)"
      type="button"
      class="segmented-button"
      :class="{ 'segmented-button--active': String(opt.value) === String(modelValue) }"
      :disabled="disabled"
      :aria-pressed="String(opt.value) === String(modelValue)"
      @click="select(opt.value)"
    >
      {{ opt.label }}
    </button>
  </div>
</template>

<style scoped>
.segmented-buttons {
  display: flex;
  width: 100%;
  border-radius: 10px;
  overflow: hidden;
  border: 1px solid var(--color-border);
  background: var(--color-background-soft);
}

.segmented-button {
  flex: 1;
  padding: 10px 14px;
  border: none;
  border-right: 1px solid var(--color-border);
  background: transparent;
  color: var(--brand-fog-grey);
  font-size: 0.95rem;
  font-weight: 600;
  cursor: pointer;
  transition: background 0.15s ease, color 0.15s ease;
}

.segmented-button:last-child {
  border-right: none;
}

.segmented-button:hover:not(:disabled):not(.segmented-button--active) {
  background: var(--color-background-mute);
  color: var(--brand-lamp-white);
}

.segmented-button--active {
  background: var(--brand-aurora-blue);
  color: var(--brand-lamp-white);
}

.segmented-button:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}
</style>
