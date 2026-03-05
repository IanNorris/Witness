import { ref } from 'vue'
import { useCameraStore } from '../stores/cameras'
import { useClipStore } from '../stores/clips'

export interface WsEvent {
  event: string
  data: Record<string, unknown>
}

const ws = ref<WebSocket | null>(null)
const connected = ref(false)
let reconnectTimer: ReturnType<typeof setTimeout> | null = null
let reconnectDelay = 1000
let listeners: Array<(evt: WsEvent) => void> = []

function getWsUrl() {
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:'
  return `${proto}//${location.host}/ws/events`
}

function connect() {
  if (ws.value && ws.value.readyState <= WebSocket.OPEN) return

  const socket = new WebSocket(getWsUrl())
  ws.value = socket

  socket.onopen = () => {
    connected.value = true
    reconnectDelay = 1000
  }

  socket.onmessage = (msg) => {
    try {
      const evt = JSON.parse(msg.data) as WsEvent
      dispatch(evt)
    } catch {
      // ignore malformed messages
    }
  }

  socket.onclose = () => {
    connected.value = false
    ws.value = null
    scheduleReconnect()
  }

  socket.onerror = () => {
    socket.close()
  }
}

function scheduleReconnect() {
  if (reconnectTimer) return
  reconnectTimer = setTimeout(() => {
    reconnectTimer = null
    reconnectDelay = Math.min(reconnectDelay * 1.5, 10000)
    connect()
  }, reconnectDelay)
}

function dispatch(evt: WsEvent) {
  for (const fn of listeners) {
    try { fn(evt) } catch { /* */ }
  }
}

function onEvent(fn: (evt: WsEvent) => void) {
  listeners.push(fn)
  return () => {
    listeners = listeners.filter(l => l !== fn)
  }
}

function disconnect() {
  if (reconnectTimer) {
    clearTimeout(reconnectTimer)
    reconnectTimer = null
  }
  ws.value?.close()
  ws.value = null
  connected.value = false
}

// Auto-refresh: reload page when server has a newer web build
function installBuildHashHandler() {
  console.log(`[Witness] Build hash: ${__BUILD_HASH__}`)
  // Check if we just reloaded due to hash mismatch
  const reloadReason = sessionStorage.getItem('witness_reload_reason')
  if (reloadReason) {
    console.log(`[Witness] Page was reloaded: ${reloadReason}`)
    sessionStorage.removeItem('witness_reload_reason')
  }
  onEvent((evt) => {
    const hash = (evt.event === 'init' && evt.data.buildHash)
      ? evt.data.buildHash as string
      : (evt.event === 'build:hash' && evt.data.hash)
        ? evt.data.hash as string
        : null
    if (hash && hash !== __BUILD_HASH__) {
      const reason = `Build hash mismatch (local=${__BUILD_HASH__}, server=${hash}) via ${evt.event}`
      console.log(`[Witness] ${reason}, reloading...`)
      sessionStorage.setItem('witness_reload_reason', reason)
      location.reload()
    }
  })
}

// Built-in handler: update camera store on camera events
function installCameraHandler() {
  onEvent((evt) => {
    const cameraStore = useCameraStore()

    if (evt.event === 'init' && evt.data.cameras) {
      const cams = evt.data.cameras as Array<{
        cameraID: number; name: string; status: string; recording: boolean
      }>
      for (const c of cams) {
        const cam = cameraStore.getCameraById(c.cameraID)
        if (cam) {
          cam.status = c.status
          cam.isRecording = c.recording
        }
      }
      // Trigger array reactivity
      cameraStore.cameras = [...cameraStore.cameras]
    }

    if (evt.event === 'camera:recording') {
      const d = evt.data as { cameraID: number; recording: boolean }
      const cam = cameraStore.getCameraById(d.cameraID)
      if (cam) {
        cam.isRecording = d.recording
        cameraStore.cameras = [...cameraStore.cameras]
      }
    }

    if (evt.event === 'camera:state') {
      const d = evt.data as { cameraID: number; status?: string; recording?: boolean }
      const cam = cameraStore.getCameraById(d.cameraID)
      if (cam) {
        if (d.status !== undefined) cam.status = d.status
        if (d.recording !== undefined) cam.isRecording = d.recording
        cameraStore.cameras = [...cameraStore.cameras]
      }
    }
  })
}

// Built-in handler: refresh recent clips when a new clip arrives
function installClipHandler() {
  onEvent((evt) => {
    if (evt.event === 'clip:new') {
      const clipStore = useClipStore()
      clipStore.fetchRecent(30)
    }
  })
}

export function useEventStream() {
  return {
    connect,
    disconnect,
    connected,
    onEvent,
    installBuildHashHandler,
    installCameraHandler,
    installClipHandler,
  }
}
