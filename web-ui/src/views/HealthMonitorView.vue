<script setup lang="ts">
import HealthDashboard from '../components/admin/HealthDashboard.vue'
</script>

<template>
  <main class="health-monitor-shell" data-bs-theme="dark">
    <HealthDashboard standalone />
  </main>
</template>

<style scoped>
.health-monitor-shell {
  height: 100vh;
  height: 100dvh;
  box-sizing: border-box;
  padding: 1rem;
  overflow: auto;
  color: #e1e4e8;
  background: #0f1117;
}
</style>
