<script setup lang="ts">
import { computed, onBeforeUnmount, ref } from 'vue'
import { useLampStore } from '@/stores/lamp'
import { useExpressionsStore } from '@/stores/expressions'
import ExpressionSheet from '@/components/ExpressionSheet.vue'

interface ExpressionEntry {
  type: string
  enabled: boolean
  target: number
  colors: (string | null)[]
  [key: string]: unknown
}

const lampStore = useLampStore()
const expressionsStore = useExpressionsStore()

const TARGET_LABELS: Record<number, string> = { 1: 'Shade', 2: 'Base', 3: 'Both' }

const TEST_PREVIEW_MS = 5000
const testTimer = ref<ReturnType<typeof setTimeout> | null>(null)

const clearTestTimer = () => {
  if (testTimer.value) {
    clearTimeout(testTimer.value)
    testTimer.value = null
  }
}

onBeforeUnmount(() => {
  if (testTimer.value) {
    clearTestTimer()
    lampStore.testExpressionComplete()
  }
})

const sheetOpen = ref(false)
const sheetMode = ref<'add' | 'edit'>('add')
const sheetEntry = ref<ExpressionEntry | null>(null)

const entries = computed<ExpressionEntry[]>(
  () => (lampStore.state.expressions as ExpressionEntry[] | undefined) ?? [],
)

const existingKeys = computed(() =>
  entries.value.map((e) => ({ type: e.type, target: e.target })),
)

const labelFor = (entry: ExpressionEntry) =>
  `${expressionsStore.getExpressionName(entry.type)} — ${TARGET_LABELS[entry.target] ?? 'Target'}`

const openAdd = () => {
  sheetMode.value = 'add'
  sheetEntry.value = null
  sheetOpen.value = true
}

const openEdit = (entry: ExpressionEntry) => {
  sheetMode.value = 'edit'
  sheetEntry.value = entry
  sheetOpen.value = true
}

const closeSheet = () => {
  sheetOpen.value = false
  sheetEntry.value = null
}

const testEntry = (entry: ExpressionEntry) => {
  clearTestTimer()
  lampStore.testExpression(entry.type, entry.target)
  testTimer.value = setTimeout(() => {
    testTimer.value = null
    lampStore.testExpressionComplete()
  }, TEST_PREVIEW_MS)
}

const deleteEntry = async (entry: ExpressionEntry) => {
  if (!confirm(`Delete ${labelFor(entry)}?`)) return
  try {
    await lampStore.removeExpression(entry.type, entry.target)
  } catch (err) {
    console.warn('[expressions] remove failed:', err)
  }
}

const toggleEnabled = async (entry: ExpressionEntry, enabled: boolean) => {
  const next: ExpressionEntry = { ...entry, enabled }
  try {
    await lampStore.updateExpression(
      { type: entry.type, target: entry.target },
      next,
    )
  } catch (err) {
    console.warn('[expressions] toggle failed:', err)
  }
}

const allTargetsTakenForAllTypes = computed(() =>
  expressionsStore.expressionsList.every((t) =>
    [1, 2, 3].every((target) => existingKeys.value.some((k) => k.type === t.index && k.target === target)),
  ),
)
</script>

<template>
  <section class="tab-panel" aria-label="Expressions">
    <InfoPanel>
      Add expressions to give your lamp personality. Each expression runs on a chosen target
      (shade, base, or both). Tap Test to preview one.
    </InfoPanel>

    <ul v-if="entries.length" class="expr-list">
      <li v-for="entry in entries" :key="`${entry.type}-${entry.target}`" class="expr-row">
        <button
          type="button"
          class="expr-toggle"
          :class="{ 'expr-toggle--active': entry.enabled }"
          :disabled="lampStore.disabled"
          :aria-checked="entry.enabled"
          role="switch"
          @click="toggleEnabled(entry, !entry.enabled)"
        >
          <div class="expr-toggle-track"><div class="expr-toggle-thumb"></div></div>
        </button>

        <button
          class="expr-btn expr-btn--test"
          :disabled="lampStore.disabled"
          @click="testEntry(entry)"
        >
          Test
        </button>

        <span class="expr-label">{{ labelFor(entry) }}</span>

        <div class="expr-actions-right">
          <button class="expr-btn expr-btn--edit" :disabled="lampStore.disabled" @click="openEdit(entry)">
            Edit
          </button>
          <button class="expr-btn expr-btn--delete" :disabled="lampStore.disabled" @click="deleteEntry(entry)">
            Delete
          </button>
        </div>
      </li>
    </ul>

    <p v-else class="expr-empty">No expressions yet. Tap "+ Add expression" to get started.</p>

    <button
      class="expr-add"
      :disabled="lampStore.disabled || allTargetsTakenForAllTypes"
      @click="openAdd"
    >
      + Add expression
    </button>

    <ExpressionSheet
      :open="sheetOpen"
      :mode="sheetMode"
      :entry="sheetEntry ?? undefined"
      :existing-keys="existingKeys"
      @close="closeSheet"
    />
  </section>
</template>

<style scoped>
.tab-panel {
  animation: fadeIn 0.3s ease-in-out;
}

@keyframes fadeIn {
  from { opacity: 0; transform: translateY(10px); }
  to   { opacity: 1; transform: translateY(0); }
}

.expr-list {
  list-style: none;
  padding: 0;
  margin: 0 0 16px;
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.expr-row {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 12px 14px;
  background: var(--color-background-mute);
  border: 1px solid var(--color-border);
  border-radius: 12px;
  flex-wrap: wrap;
}

.expr-toggle {
  position: relative;
  width: 40px;
  height: 22px;
  border: none;
  border-radius: 11px;
  background: var(--color-background-soft);
  cursor: pointer;
  padding: 0;
  flex-shrink: 0;
  box-shadow: inset 0 1px 2px rgba(0, 0, 0, 0.3);
}

.expr-toggle:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}

.expr-toggle-track {
  position: relative;
  width: 100%;
  height: 100%;
}

.expr-toggle-thumb {
  position: absolute;
  top: 2px;
  left: 2px;
  width: 18px;
  height: 18px;
  background: var(--brand-slate-grey);
  border-radius: 50%;
  transition: left 0.2s ease, background 0.2s ease;
}

.expr-toggle--active .expr-toggle-thumb {
  left: calc(100% - 20px);
  background: var(--brand-lumen-green);
}

.expr-label {
  flex: 1;
  min-width: 160px;
  font-weight: 600;
  color: var(--brand-lamp-white);
}

.expr-actions-right {
  display: flex;
  gap: 6px;
  flex-shrink: 0;
  margin-left: auto;
}

.expr-btn {
  padding: 8px 12px;
  border-radius: 8px;
  font-size: 0.85rem;
  font-weight: 600;
  cursor: pointer;
  border: 1px solid var(--color-border);
  background: var(--color-background-soft);
  color: var(--brand-fog-grey);
}

.expr-btn:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}

.expr-btn--test {
  border-color: var(--brand-aurora-blue);
  color: var(--brand-aurora-blue);
}

.expr-btn--delete {
  border-color: rgba(255, 107, 107, 0.5);
  color: #ff6b6b;
}

.expr-empty {
  text-align: center;
  color: var(--brand-fog-grey);
  padding: 32px 16px;
  background: var(--color-background-mute);
  border: 1px dashed var(--color-border);
  border-radius: 12px;
  margin: 0 0 16px;
}

.expr-add {
  width: 100%;
  padding: 14px;
  border-radius: 12px;
  border: none;
  cursor: pointer;
  font-weight: 700;
  font-size: 0.95rem;
  color: var(--brand-lamp-white);
  background: linear-gradient(135deg, var(--brand-aurora-blue), var(--brand-glow-pink));
  transition: opacity 0.2s ease;
}

.expr-add:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}
</style>
