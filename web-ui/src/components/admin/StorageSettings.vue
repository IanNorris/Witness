<script setup lang="ts">
import { ref, computed, onMounted } from 'vue'
import { api } from '../../composables/useApi'

const loading = ref(true)

const settingKeys = [
  'continuous_recording_retention_days',
  'continuous_recording_quota_gb',
  'clip_cleanup_enabled',
  'clip_retention_days',
]

const settings = ref<Record<string, string>>({})

interface DiskInfo {
  diskTotalGB: number
  diskFreeGB: number
  diskUsed: number
  diskTotal: number
  diskFree: number
  segmentCount: number
  segmentTotalDuration: number
  segmentTotalBytes: number
  segmentTotalGB: number
  cachePath: string
  cameras: Array<{
    cameraId: number
    segmentCount: number
    totalBytes: number
    totalGB: number
  }>
}

const diskInfo = ref<DiskInfo | null>(null)
const diskError = ref<string | null>(null)
const scanResult = ref<{ totalBytes: number; totalGB: number; fileCount: number } | null>(null)
const scanning = ref(false)

const diskFreePercent = computed(() => {
  if (!diskInfo.value || !diskInfo.value.diskTotal) return 0
  return (diskInfo.value.diskFree / diskInfo.value.diskTotal) * 100
})

const diskUsedPercent = computed(() => {
  if (!diskInfo.value) return 0
  return ((diskInfo.value.diskTotal - diskInfo.value.diskFree) / diskInfo.value.diskTotal) * 100
})

const diskBarClass = computed(() => {
  const free = diskFreePercent.value
  if (free < 10) return 'bg-danger'
  if (free < 25) return 'bg-warning'
  return 'bg-success'
})

const quotaUsedPercent = computed(() => {
  if (!diskInfo.value) return null
  const quotaGB = parseFloat(settings.value['continuous_recording_quota_gb'] || '0')
  if (quotaGB <= 0) return null
  return (diskInfo.value.segmentTotalGB / quotaGB) * 100
})

const dvrDurationFormatted = computed(() => {
  if (!diskInfo.value) return ''
  const totalSec = diskInfo.value.segmentTotalDuration
  const days = Math.floor(totalSec / 86400)
  const hours = Math.floor((totalSec % 86400) / 3600)
  if (days > 0) return `${days}d ${hours}h`
  return `${hours}h`
})

async function fetchSettings() {
  loading.value = true
  try {
    const data = await api<Record<string, string>>('/api/setup/settings')
    if (data) {
      for (const key of settingKeys) {
        settings.value[key] = data[key] ?? ''
      }
      // Set defaults for display if empty
      if (!settings.value['continuous_recording_retention_days'])
        settings.value['continuous_recording_retention_days'] = '3'
      if (!settings.value['continuous_recording_quota_gb'])
        settings.value['continuous_recording_quota_gb'] = '0'
      if (!settings.value['clip_retention_days'])
        settings.value['clip_retention_days'] = '10'
    }
  } finally {
    loading.value = false
  }
}

async function fetchDiskInfo() {
  diskError.value = null
  try {
    diskInfo.value = await api<DiskInfo>('/debug/disk')
  } catch (e) {
    diskError.value = `Failed to load disk info: ${e}`
  }
}

async function scanTotalStorage() {
  scanning.value = true
  try {
    scanResult.value = await api<{ totalBytes: number; totalGB: number; fileCount: number }>('/debug/disk/scan', {
      method: 'POST',
    })
  } catch (e) {
    scanResult.value = null
  } finally {
    scanning.value = false
  }
}

async function saveSetting(name: string) {
  await api('/api/settings/set', {
    method: 'POST',
    body: { name, value: settings.value[name] ?? '' },
  })
}

function formatBytes(bytes: number): string {
  if (bytes >= 1024 * 1024 * 1024) return (bytes / (1024 * 1024 * 1024)).toFixed(1) + ' GB'
  if (bytes >= 1024 * 1024) return (bytes / (1024 * 1024)).toFixed(1) + ' MB'
  return (bytes / 1024).toFixed(1) + ' KB'
}

onMounted(() => {
  fetchSettings()
  fetchDiskInfo()
})
</script>

<template>
  <div>
    <h6 class="mb-3">Storage &amp; DVR Settings</h6>

    <div v-if="loading" class="text-center py-3">
      <div class="spinner-border spinner-border-sm" />
    </div>

    <div v-else>
      <!-- Disk Usage -->
      <div class="card bg-dark border-secondary mb-3">
        <div class="card-body">
          <h6 class="card-title small text-muted mb-2">Disk Usage</h6>
          <div v-if="diskInfo">
            <div class="d-flex justify-content-between small mb-1">
              <span>{{ diskInfo.diskFreeGB.toFixed(1) }} GB free ({{ diskFreePercent.toFixed(1) }}%)</span>
              <span>{{ diskInfo.diskTotalGB.toFixed(1) }} GB total</span>
            </div>
            <div class="progress mb-2" style="height: 8px;">
              <div class="progress-bar" :class="diskBarClass"
                   :style="{ width: diskUsedPercent.toFixed(1) + '%' }" />
            </div>
            <div class="small text-muted mb-2">
              Cache path: <code>{{ diskInfo.cachePath }}</code>
            </div>
            <div class="d-flex align-items-center gap-2">
              <button class="btn btn-sm btn-outline-secondary" :disabled="scanning" @click="scanTotalStorage">
                {{ scanning ? 'Scanning...' : '📁 Scan total Witness storage' }}
              </button>
              <span v-if="scanResult" class="small">
                <strong>{{ formatBytes(scanResult.totalBytes) }}</strong>
                <span class="text-muted ms-1">({{ scanResult.fileCount.toLocaleString() }} files)</span>
              </span>
            </div>
          </div>
          <div v-else-if="diskError" class="text-danger small">{{ diskError }}</div>
          <div v-else class="text-muted small">Loading...</div>
        </div>
      </div>

      <!-- DVR Segment Stats -->
      <div v-if="diskInfo && diskInfo.segmentCount > 0" class="card bg-dark border-secondary mb-3">
        <div class="card-body">
          <h6 class="card-title small text-muted mb-2">DVR Storage</h6>
          <div class="row g-2 small mb-2">
            <div class="col-auto">
              <span class="text-muted">Segments:</span>
              <strong class="ms-1">{{ diskInfo.segmentCount.toLocaleString() }}</strong>
            </div>
            <div class="col-auto">
              <span class="text-muted">Size:</span>
              <strong class="ms-1">{{ diskInfo.segmentTotalGB.toFixed(2) }} GB</strong>
            </div>
            <div class="col-auto">
              <span class="text-muted">Duration:</span>
              <strong class="ms-1">{{ dvrDurationFormatted }}</strong>
            </div>
            <div v-if="quotaUsedPercent !== null" class="col-auto">
              <span class="text-muted">Quota:</span>
              <strong class="ms-1" :class="quotaUsedPercent > 90 ? 'text-danger' : quotaUsedPercent > 75 ? 'text-warning' : ''">
                {{ quotaUsedPercent.toFixed(1) }}% used
              </strong>
            </div>
          </div>
          <div v-if="diskInfo.cameras && diskInfo.cameras.length > 0">
            <table class="table table-sm table-dark table-borderless mb-0 small">
              <thead>
                <tr class="text-muted">
                  <th>Camera</th>
                  <th class="text-end">Segments</th>
                  <th class="text-end">Size</th>
                </tr>
              </thead>
              <tbody>
                <tr v-for="cam in diskInfo.cameras" :key="cam.cameraId">
                  <td>Camera {{ cam.cameraId }}</td>
                  <td class="text-end">{{ cam.segmentCount.toLocaleString() }}</td>
                  <td class="text-end">{{ formatBytes(cam.totalBytes) }}</td>
                </tr>
              </tbody>
            </table>
          </div>
          <button class="btn btn-sm btn-outline-secondary mt-2" @click="fetchDiskInfo">
            ↻ Refresh
          </button>
        </div>
      </div>

      <!-- Settings -->
      <div class="card bg-dark border-secondary mb-3">
        <div class="card-body">
          <h6 class="card-title small text-muted mb-2">Continuous Recording</h6>
          <div class="row g-3">
            <div class="col-md-4">
              <label class="form-label small">Retention (days)</label>
              <input v-model="settings['continuous_recording_retention_days']"
                     type="number" min="1" max="365"
                     class="form-control form-control-sm"
                     @blur="saveSetting('continuous_recording_retention_days')" />
              <div class="form-text small">Segments older than this are deleted. Default: 3</div>
            </div>
            <div class="col-md-4">
              <label class="form-label small">Quota (GB)</label>
              <input v-model="settings['continuous_recording_quota_gb']"
                     type="number" min="0" step="1"
                     class="form-control form-control-sm"
                     @blur="saveSetting('continuous_recording_quota_gb')" />
              <div class="form-text small">Max disk usage for DVR. 0 = unlimited (time-based only)</div>
            </div>
          </div>
        </div>
      </div>

      <div class="card bg-dark border-secondary mb-3">
        <div class="card-body">
          <h6 class="card-title small text-muted mb-2">Clip Cleanup</h6>
          <div class="row g-3">
            <div class="col-md-4">
              <label class="form-label small">Enabled</label>
              <select v-model="settings['clip_cleanup_enabled']"
                      class="form-select form-select-sm"
                      @change="saveSetting('clip_cleanup_enabled')">
                <option value="">Disabled</option>
                <option value="true">Enabled</option>
              </select>
              <div class="form-text small">When enabled, old clips are automatically deleted</div>
            </div>
            <div class="col-md-4">
              <label class="form-label small">Retention (days)</label>
              <input v-model="settings['clip_retention_days']"
                     type="number" min="1" max="365"
                     class="form-control form-control-sm"
                     @blur="saveSetting('clip_retention_days')" />
              <div class="form-text small">Clips older than this are deleted. Default: 10</div>
            </div>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>
