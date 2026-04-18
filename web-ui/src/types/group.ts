export interface Group {
  id: number
  displayName: string
  description: string
}

export interface GroupEnumResponse {
  count: number
  groups: Group[]
}
