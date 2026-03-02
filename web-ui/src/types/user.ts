export interface User {
  username: string
  admin: boolean
  enabled: boolean
  groups: number[]
  mustChangePassword: boolean
}

export interface Group {
  id: number
  name: string
  description: string
}

export interface AuthProfile {
  csrf: string
  username: string
  admin: boolean
  displayName: string
}
