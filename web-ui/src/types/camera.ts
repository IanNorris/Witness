export interface Camera {
  id: number
  name: string
  status: string
  isRecording: boolean
  groups: number[]
  previewUrl: string
  streamUrl: string
  stats: CameraStats
  lowLatencyHLS?: boolean
  ptzEnabled?: boolean
}

export interface CameraStats {
  fps: number
  bitrate: number
  uptime: string
  reconnects: number
}

export interface CameraEnumResponse {
  cameras: CameraData[]
}

export interface CameraData {
  id: number
  name: string
  status: string
  recording: boolean
  groups: number[]
  description: string
  enabled: number
  lastTimestamp: number
  frameCount: number
  connectionString?: string
  connectionStringSub?: string
  lowLatencyHLS?: number
  ptzEnabled?: number
}
