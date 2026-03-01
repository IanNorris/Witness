export interface Camera {
  id: number
  name: string
  status: string
  isRecording: boolean
  groups: number[]
  previewUrl: string
  streamUrl: string
  stats: CameraStats
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
  ID: number
  Name: string
  Status: string
  IsRecording: boolean
  Groups: string
  FPS: number
  Bitrate: number
  Uptime: string
  Reconnects: number
}
