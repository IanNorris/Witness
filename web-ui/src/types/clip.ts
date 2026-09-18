export interface Clip {
  uid: number
  camera: number
  cameraName: string
  timestamp: number
  duration: number
  tags: string
  saved: boolean
  recordMode: string
  description: string
  detectionVersion: number
  lighting: LightingCondition
  reviewed: boolean
  recognizedFaces?: string[]
  audioEvents?: AudioEvent[]
}

export interface AudioEvent {
  group: string
  startTime: number
  endTime: number
  peakScore: number
  modelVersion: string
}

export interface Tag {
  id: number
  name: string
  display: string
  icon: string
  sortOrder: number
  hidden: boolean
  clipCount: number
}

export const LightingCondition = {
  Unknown: 0,
  Day: 1,
  Night: 2,
} as const

export type LightingCondition = (typeof LightingCondition)[keyof typeof LightingCondition]
