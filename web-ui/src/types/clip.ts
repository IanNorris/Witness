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
