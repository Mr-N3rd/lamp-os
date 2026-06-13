<script setup lang="ts">
import { computed, ref, watch } from 'vue'
import ComponentForm from '@/components/Form.vue'
import type { FieldDefinition, FormValues } from '@/types'
import { useLampStore } from '@/stores/lamp'
import { useExpressionsStore } from '@/stores/expressions'

interface ExpressionEntry {
  type: string
  enabled: boolean
  target: number
  colors: (string | null)[]
  [key: string]: unknown
}

const props = defineProps<{
  open: boolean
  mode: 'add' | 'edit'
  entry?: ExpressionEntry
  existingKeys: Array<{ type: string; target: number }>
}>()

const emit = defineEmits<{
  close: []
}>()

const lampStore = useLampStore()
const expressionsStore = useExpressionsStore()

const TARGET_LABELS: Record<number, string> = { 1: 'Shade', 2: 'Base', 3: 'Both' }
const TARGET_VALUES = [1, 2, 3]

const selectedType = ref<string>('')
const selectedTarget = ref<number>(2)
const formValues = ref<FormValues>({})
const saving = ref(false)
const errorMsg = ref<string | null>(null)

const allTypes = computed(() => expressionsStore.expressionsList)

const fields = computed<FieldDefinition[]>(() => {
  if (!selectedType.value) return []
  return expressionsStore.getExpressionFields(selectedType.value).filter((f) => f.name !== 'target')
})

const isTargetUsed = (type: string, target: number) =>
  props.existingKeys.some((k) => k.type === type && k.target === target)

const allTargetsTaken = (type: string) => TARGET_VALUES.every((t) => isTargetUsed(type, t))

const targetDisabled = (target: number) => {
  if (props.mode === 'edit') return true
  return isTargetUsed(selectedType.value, target)
}

const canSubmit = computed(() => {
  if (!lampStore.wsConnected) return false
  if (saving.value) return false
  if (!selectedType.value) return false
  if (props.mode === 'add' && isTargetUsed(selectedType.value, selectedTarget.value)) return false
  return true
})

const submitLabel = computed(() => (props.mode === 'edit' ? 'Update' : 'Add'))

const seedFormFromDefaults = (type: string) => {
  const def = expressionsStore.getExpression(type)
  if (!def) return
  const values: FormValues = { colors: ['#FFFFFFFF'] }
  def.fields.forEach((f) => {
    if (f.default === undefined || !f.name) return
    if (f.transform && Array.isArray(f.transform)) {
      const arr = f.default as number[]
      f.transform.forEach((suffix, i) => {
        values[`${f.name}${suffix}`] = arr[i]
      })
    } else if (f.name !== 'target') {
      values[f.name] = f.default
    }
  })
  formValues.value = values
}

const seedFormFromEntry = (entry: ExpressionEntry) => {
  const values: FormValues = {}
  for (const [k, v] of Object.entries(entry)) {
    if (k === 'type' || k === 'target' || k === 'enabled') continue
    values[k] = v
  }
  formValues.value = values
}

watch(
  () => props.open,
  (open) => {
    if (!open) return
    errorMsg.value = null
    saving.value = false
    if (props.mode === 'edit' && props.entry) {
      selectedType.value = props.entry.type
      selectedTarget.value = props.entry.target
      seedFormFromEntry(props.entry)
    } else {
      const firstAvailable = allTypes.value.find((t) => !allTargetsTaken(t.index))
      selectedType.value = firstAvailable?.index ?? ''
      if (selectedType.value) {
        const firstFreeTarget = TARGET_VALUES.find((t) => !isTargetUsed(selectedType.value, t)) ?? 2
        selectedTarget.value = firstFreeTarget
        seedFormFromDefaults(selectedType.value)
      }
    }
  },
  { immediate: true },
)

watch(selectedType, (next, prev) => {
  if (!props.open || props.mode === 'edit' || !next || next === prev) return
  if (isTargetUsed(next, selectedTarget.value)) {
    const firstFree = TARGET_VALUES.find((t) => !isTargetUsed(next, t))
    if (firstFree) selectedTarget.value = firstFree
  }
  seedFormFromDefaults(next)
})

const handleFormUpdate = (values: FormValues) => {
  formValues.value = { ...formValues.value, ...values }
}

const handleColorPreview = (fieldName: string, value: string) => {
  if (fieldName !== 'colors') return
  lampStore.previewExpressionColor(value, selectedTarget.value)
}

const handleColorPreviewEnd = (fieldName: string) => {
  if (fieldName !== 'colors') return
  lampStore.restoreColorsAfterPreview()
}

const buildEntry = (): ExpressionEntry => {
  const entry: ExpressionEntry = {
    type: selectedType.value,
    enabled: props.entry?.enabled ?? true,
    target: selectedTarget.value,
    colors: (formValues.value.colors as string[] | undefined) ?? ['#FFFFFFFF'],
  }
  for (const [k, v] of Object.entries(formValues.value)) {
    if (k === 'colors') continue
    entry[k] = v
  }
  return entry
}

const onSubmit = async () => {
  if (!canSubmit.value) return
  saving.value = true
  errorMsg.value = null
  try {
    const entry = buildEntry()
    if (props.mode === 'edit' && props.entry) {
      await lampStore.updateExpression(
        { type: props.entry.type, target: props.entry.target },
        entry,
      )
    } else {
      await lampStore.addExpression(entry)
    }
    emit('close')
  } catch (err) {
    errorMsg.value = err instanceof Error ? err.message : String(err)
  } finally {
    saving.value = false
  }
}

const onCancel = () => {
  emit('close')
}
</script>

<template>
  <Teleport to="body">
    <div v-if="open" class="sheet-overlay" @click.self="onCancel">
      <div class="sheet">
        <header class="sheet-header">
          <h3>{{ mode === 'edit' ? 'Edit expression' : 'Add expression' }}</h3>
          <button class="sheet-close" @click="onCancel" aria-label="Close">×</button>
        </header>

        <div class="sheet-body">
          <div v-if="mode === 'edit'" class="sheet-key">
            <span class="sheet-key-type">{{ expressionsStore.getExpressionName(selectedType) }}</span>
            <span class="sheet-key-sep">—</span>
            <span class="sheet-key-target">{{ TARGET_LABELS[selectedTarget] }}</span>
          </div>

          <template v-else>
            <div class="sheet-field">
              <label class="sheet-label">Type</label>
              <select v-model="selectedType" class="sheet-select">
                <option
                  v-for="t in allTypes"
                  :key="t.index"
                  :value="t.index"
                  :disabled="allTargetsTaken(t.index) && t.index !== selectedType"
                >
                  {{ t.name }}
                </option>
              </select>
            </div>

            <div class="sheet-field">
              <label class="sheet-label">Target</label>
              <div class="sheet-targets" role="radiogroup">
                <button
                  v-for="t in TARGET_VALUES"
                  :key="t"
                  type="button"
                  class="sheet-target"
                  :class="{ 'sheet-target--active': selectedTarget === t }"
                  :disabled="targetDisabled(t)"
                  @click="selectedTarget = t"
                >
                  {{ TARGET_LABELS[t] }}
                </button>
              </div>
            </div>
          </template>

          <ComponentForm
            v-if="selectedType"
            :fields="fields"
            :model-value="formValues"
            @update:model-value="handleFormUpdate"
            @preview="handleColorPreview"
            @close="handleColorPreviewEnd"
            :show-button="false"
            :disabled="!lampStore.wsConnected"
          />

          <p v-if="errorMsg" class="sheet-error">{{ errorMsg }}</p>
        </div>

        <footer class="sheet-footer">
          <button class="sheet-btn sheet-btn--cancel" @click="onCancel">Cancel</button>
          <button
            class="sheet-btn sheet-btn--primary"
            :disabled="!canSubmit"
            @click="onSubmit"
          >
            {{ saving ? 'Saving…' : submitLabel }}
          </button>
        </footer>
      </div>
    </div>
  </Teleport>
</template>

<style scoped>
.sheet-overlay {
  position: fixed;
  inset: 0;
  background: rgba(0, 0, 0, 0.8);
  display: flex;
  align-items: flex-end;
  justify-content: center;
  /* Below Color.vue's dialog (z-index 1000) so the picker renders on top. */
  z-index: 900;
  backdrop-filter: blur(4px);
}

.sheet {
  background: var(--color-background-mute);
  border-top-left-radius: 20px;
  border-top-right-radius: 20px;
  width: 100%;
  max-width: 540px;
  max-height: 90vh;
  display: flex;
  flex-direction: column;
  box-shadow: 0 -8px 32px rgba(0, 0, 0, 0.5);
  animation: slide-up 0.22s ease-out;
}

@keyframes slide-up {
  from { transform: translateY(100%); }
  to   { transform: translateY(0); }
}

.sheet-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 20px 20px 14px;
  border-bottom: 1px solid var(--color-border);
}

.sheet-header h3 {
  margin: 0;
  font-size: 1.1rem;
  font-weight: 700;
  color: var(--brand-lamp-white);
}

.sheet-close {
  width: 36px;
  height: 36px;
  border-radius: 10px;
  border: 1px solid var(--color-border);
  background: var(--color-background-soft);
  color: var(--brand-fog-grey);
  font-size: 20px;
  cursor: pointer;
}

.sheet-body {
  padding: 20px;
  overflow-y: auto;
  flex: 1;
  display: flex;
  flex-direction: column;
  gap: 16px;
}

.sheet-key {
  display: flex;
  align-items: baseline;
  gap: 10px;
  padding: 12px 16px;
  background: var(--color-background-soft);
  border: 1px solid var(--color-border);
  border-radius: 12px;
}

.sheet-key-type {
  font-weight: 700;
  color: var(--brand-lamp-white);
}

.sheet-key-sep {
  color: var(--brand-fog-grey);
}

.sheet-key-target {
  color: var(--brand-aurora-blue);
  font-weight: 600;
}

.sheet-field {
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.sheet-label {
  font-size: 0.9rem;
  font-weight: 600;
  color: var(--brand-fog-grey);
}

.sheet-select {
  width: 100%;
  padding: 12px 14px;
  border-radius: 10px;
  border: 1px solid var(--color-border);
  background: var(--color-background-soft);
  color: var(--brand-lamp-white);
  font-size: 0.95rem;
}

.sheet-targets {
  display: flex;
  border-radius: 10px;
  overflow: hidden;
  border: 1px solid var(--color-border);
}

.sheet-target {
  flex: 1;
  padding: 10px;
  border: none;
  background: transparent;
  color: var(--brand-fog-grey);
  font-weight: 600;
  cursor: pointer;
  border-right: 1px solid var(--color-border);
}

.sheet-target:last-child {
  border-right: none;
}

.sheet-target:hover:not(:disabled):not(.sheet-target--active) {
  background: var(--color-background-soft);
  color: var(--brand-lamp-white);
}

.sheet-target--active {
  background: var(--brand-aurora-blue);
  color: var(--brand-lamp-white);
}

.sheet-target:disabled {
  opacity: 0.4;
  cursor: not-allowed;
}

.sheet-error {
  color: #ff6b6b;
  font-size: 0.9rem;
  margin: 0;
}

.sheet-footer {
  display: flex;
  gap: 10px;
  padding: 14px 20px 20px;
  border-top: 1px solid var(--color-border);
}

.sheet-btn {
  flex: 1;
  padding: 12px 18px;
  border-radius: 10px;
  font-weight: 600;
  font-size: 0.95rem;
  cursor: pointer;
  border: none;
  transition: opacity 0.2s ease;
}

.sheet-btn--cancel {
  background: var(--color-background-soft);
  color: var(--brand-fog-grey);
  border: 1px solid var(--color-border);
}

.sheet-btn--primary {
  background: linear-gradient(135deg, var(--brand-aurora-blue), var(--brand-glow-pink));
  color: var(--brand-lamp-white);
}

.sheet-btn:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}
</style>
