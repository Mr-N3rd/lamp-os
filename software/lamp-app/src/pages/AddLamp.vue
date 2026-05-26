<script setup lang="ts">
import { useRouter } from 'vue-router'
const router = useRouter()
</script>

<template>
  <main class="add-lamp">
    <h1>Add a lamp</h1>
    <p>BLE / mDNS scan UI lives here (Phase 6).</p>
    <button @click="router.back()">Back</button>
  </main>
</template>

<style scoped>
.add-lamp { padding: 24px; }
</style>
