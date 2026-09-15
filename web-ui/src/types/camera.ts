export interface Camera {
  id: number
  name: string
  status: string
  isRecording: boolean
  motionActive: boolean
  groups: number[]
  previewUrl: string
  streamUrl: string
  stats: CameraStats
  codec?: string
	width?: number
	height?: number
  hasSubStream?: boolean
  subCodec?: string
	subWidth?: number
	subHeight?: number
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
  motionActive?: boolean
  groups: number[]
  description: string
  enabled: number
  lastTimestamp: number
  frameCount: number
  codec?: string
	width?: number
	height?: number
  hasSubStream?: boolean
  subCodec?: string
	subWidth?: number
	subHeight?: number
  connectionString?: string
  connectionStringSub?: string
  lowLatencyHLS?: number
  ptzEnabled?: number
}
